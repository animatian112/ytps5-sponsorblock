// youtube-tv - Auto-update: check, download and stage a newer release.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The release lives in the project's own GitHub repository (haesele/ytps5): the newest
// release is the source of truth, its asset PPSA98260.zip is the payload and the tag is
// the content version the app compares against (see src/update/update_check.{h,c}).
//
// Downloading and staging run on their own thread (a release is megabytes and streams
// through the console's payload loader), so the frame only reads a snapshot. Two pieces
// do the work, both ported from ps5-native-app-boilerplate:
//
//   - update_check_*  the release feed: one HTTPS GET and the JSON reader;
//   - self_update_*   the engine: sends self-updater.elf to the loader on port 9021,
//                     streams the download to it, and stages the verified archive.
//
// Layout: one worker thread (1 MiB stack, like the other workers) runs check and install
// jobs in turn and publishes a Snapshot under a mutex. install() hands the engine the
// release it found and follows it every 250 ms; once the helper reports it has verified
// and unpacked the archive, the go-ahead is given and the state becomes staged, which
// tells the frame to close the app (the helper then puts the files in place).

#include <string_view> // ahead of the header below: it declares std::string_view

#include "yt/update.hpp"

#include "update/self_update.h"
#include "update/update_check.h"

#include "yt/log.hpp"

#include <pthread.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>

namespace yt::update
{
namespace
{

constexpr std::size_t kWorkerStack = 1u << 20;      // 1 MiB
constexpr int kPollMs = 250;                        // how often install() republishes progress
constexpr char kTitleId[] = "PPSA98260";            // the app's own title ID (param.json)
constexpr char kAppName[] = "YouTube SponsorBlock"; // what the helper's notification names

enum class Job : unsigned char
{
    none,
    check,
    install,
};

// Everything the worker and the frame share.
struct Shared
{
    mutable std::mutex mutex;
    std::condition_variable wake;
    // The worker parks on `wake` for the life of the process; `stopping` and `done` let the
    // process exit: at exit the handler below wakes it, waits for it to leave the park loop
    // and only then lets the static state be destroyed.
    std::condition_variable done;
    bool stopping = false;
    bool worker_exited = false;

    Job pending = Job::none;
    bool worker_started = false;
    bool worker_busy = false;
    bool engine_live = false; // the engine has a job that must not be forgotten
    bool cancel_requested = false;

    Snapshot snapshot;
    std::uint64_t revision = 0;

    // What the last check found, apart from the snapshot.
    std::string latest;
    std::string asset_url;
    std::string asset_sha256;
    long long asset_size = 0;
    bool has_asset = false;

    self_update_job engine{}; // zero until self_update_start()
};

Shared g;

// The worker is the only writer; every write bumps the revision the frame watches and
// records it in the snapshot it publishes.
template <typename Apply> void edit(Apply apply)
{
    std::lock_guard<std::mutex> lock(g.mutex);
    apply(g.snapshot);
    g.snapshot.revision = ++g.revision;
}

bool cancelled_by_user()
{
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.cancel_requested;
}

void sleep_ms(int milliseconds)
{
    timespec pause{};
    pause.tv_sec = milliseconds / 1000;
    pause.tv_nsec = static_cast<long>(milliseconds % 1000) * 1000000L;
    (void)nanosleep(&pause, nullptr);
}

char *copy_text(char *out, std::size_t capacity, const std::string &text)
{
    std::snprintf(out, capacity, "%s", text.c_str());
    return out;
}

void *worker_main(void *);

// pthread_create with an explicit stack; std::thread cannot size its stack. Called with
// the shared mutex held, so two check()/install() calls cannot start two workers.
// Registered once, right after the worker was created: without it the detached worker is
// still parked on `g.wake` while the process tears the static state down, which hangs the
// exit (seen in the unit tests, where the process must end cleanly).
void stop_worker_at_exit()
{
    std::unique_lock<std::mutex> lock(g.mutex);
    g.stopping = true;
    g.wake.notify_all();
    // A job in flight is finished first; the helper does the real file work, so waiting a
    // moment for the park loop to notice is enough and never blocks the console's quit.
    g.done.wait_for(lock, std::chrono::milliseconds(250), [] { return g.worker_exited; });
}

void start_worker_locked()
{
    if (g.worker_started)
        return;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0)
        return;
    pthread_attr_setstacksize(&attributes, kWorkerStack);
    pthread_t thread;
    const int created = pthread_create(&thread, &attributes, worker_main, nullptr);
    pthread_attr_destroy(&attributes);
    if (created != 0)
        return;
    pthread_detach(thread);
    g.worker_started = true;
    // Once: remembered by the flag itself, so a second start cannot register twice.
    static bool registered = false;
    if (!registered)
    {
        registered = true;
        (void)std::atexit(stop_worker_at_exit);
    }
}

// ---- The check -----------------------------------------------------------------------------

std::string describe_check(int reason, int http_status)
{
    char text[96];
    switch (reason)
    {
    case UPDATE_CHECK_NETWORK:
        return "the release feed could not be reached";
    case UPDATE_CHECK_HTTP_STATUS:
        std::snprintf(text, sizeof(text), "the release feed answered %d", http_status);
        return text;
    case UPDATE_CHECK_TOO_LARGE:
        return "the release feed's answer was too large";
    case UPDATE_CHECK_BAD_RESPONSE:
        return "the release feed's answer could not be read";
    default:
        return "the update check failed";
    }
}

void run_check()
{
    edit(
        [](Snapshot &snapshot)
        {
            snapshot.state = State::checking;
            snapshot.error.clear();
            snapshot.notes.clear();
        });

    update_check_release release{};
    int http_status = 0;
    int platform_error = 0;
    const int reason = update_check_run_release(&release, &http_status, &platform_error);
    // The feed answers 404 for a repository with no published release yet. That is an
    // ordinary "nothing to update to", not a failure: report it once, then behave like
    // up_to_date (no card, no retry, and the same auto-check rules).
    if (reason == UPDATE_CHECK_HTTP_STATUS && http_status == 404)
    {
        yt::log::line("update: no release published yet");
        edit(
            [](Snapshot &snapshot)
            {
                snapshot.state = State::no_release;
                snapshot.error.clear();
                snapshot.notes.clear();
            });
        return;
    }
    if (reason != UPDATE_CHECK_OK)
    {
        const char *const why = update_check_reason_text(static_cast<update_check_reason>(reason));
        const std::string message = describe_check(reason, http_status);
        yt::log::line("update: check failed (%s)", why);
        edit(
            [&message](Snapshot &snapshot)
            {
                snapshot.state = State::failed;
                snapshot.error = message;
            });
        return;
    }

    const std::string latest = normalise_version(release.tag);
    const std::string notes(release.notes);
    std::string current;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        current = g.snapshot.current_version;
    }
    // param.json reports the running content version; without it only "different" is known.
    const bool newer = current.empty() || is_newer(latest, current);
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.latest = latest;
        g.asset_url = release.has_asset ? release.asset_url : std::string();
        g.asset_sha256 = release.asset_sha256;
        g.asset_size = static_cast<long long>(release.asset_size);
        g.has_asset = release.has_asset != 0;
    }
    yt::log::line("update: release tag=%s asset=%d bytes=%lld newer=%d", latest.c_str(),
                  release.has_asset, static_cast<long long>(release.asset_size), newer ? 1 : 0);
    edit(
        [&](Snapshot &snapshot)
        {
            snapshot.latest_version = latest;
            snapshot.notes = notes;
            snapshot.asset_url = release.has_asset ? release.asset_url : std::string();
            snapshot.asset_size = static_cast<long long>(release.asset_size);
            snapshot.error.clear();
            snapshot.state = newer ? State::available : State::up_to_date;
        });
}

// ---- The install ---------------------------------------------------------------------------

void finish_engine()
{
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.engine_live = false;
    }
    self_update_finish(&g.engine);
}

self_update_offer make_offer()
{
    std::lock_guard<std::mutex> lock(g.mutex);
    self_update_offer offer{};
    copy_text(offer.title, sizeof(offer.title), kTitleId);
    copy_text(offer.installed, sizeof(offer.installed), g.snapshot.current_version);
    copy_text(offer.available, sizeof(offer.available), g.latest);
    copy_text(offer.version, sizeof(offer.version), g.latest);
    copy_text(offer.name, sizeof(offer.name), kAppName);
    copy_text(offer.artifact, sizeof(offer.artifact), g.asset_url);
    copy_text(offer.sha256, sizeof(offer.sha256), g.asset_sha256);
    offer.size = static_cast<std::uint64_t>(g.asset_size);
    return offer;
}

void fail(const std::string &message)
{
    edit(
        [&message](Snapshot &snapshot)
        {
            snapshot.state = State::failed;
            snapshot.error = message;
            snapshot.downloaded = 0;
            snapshot.progress = 0.0f;
        });
}

void run_install()
{
    const self_update_offer offer = make_offer();
    if (!self_update_start(&g.engine, self_update_console(), &offer))
    {
        fail("the update could not be started");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.engine_live = true;
    }
    edit(
        [](Snapshot &snapshot)
        {
            snapshot.state = State::downloading;
            snapshot.downloaded = 0;
            snapshot.progress = 0.0f;
            snapshot.error.clear();
        });
    yt::log::line("update: installing %s (%lld bytes)", offer.available,
                  static_cast<long long>(offer.size));

    if (cancelled_by_user())
        self_update_cancel(&g.engine);

    for (;;)
    {
        self_update_status status{};
        self_update_poll(&g.engine, &status);
        if (cancelled_by_user())
            self_update_cancel(&g.engine);
        switch (status.phase)
        {
        case SELF_UPDATE_DOWNLOADING:
        case SELF_UPDATE_UNPACKING:
        {
            const double done = static_cast<double>(status.done);
            const double total = static_cast<double>(status.total);
            float progress = 0.0f;
            if (total > 0.0)
                progress = static_cast<float>(done / total);
            edit(
                [&](Snapshot &snapshot)
                {
                    snapshot.state = State::downloading;
                    snapshot.downloaded = static_cast<long long>(status.done);
                    if (status.total != 0)
                        snapshot.asset_size = static_cast<long long>(status.total);
                    snapshot.progress = progress;
                });
            sleep_ms(kPollMs);
            break;
        }
        case SELF_UPDATE_READY:
            // Verified and unpacked beside the app: give the go-ahead, so the helper is
            // waiting for the app to close.
            if (self_update_apply(&g.engine))
            {
                edit(
                    [](Snapshot &snapshot)
                    {
                        snapshot.state = State::staged;
                        snapshot.downloaded = snapshot.asset_size;
                        snapshot.progress = 1.0f;
                        snapshot.error.clear();
                    });
                yt::log::line("update: staged %s", offer.available);
                return;
            }
            // apply() failed; the engine is FAILED now and the next turn reports it.
            break;
        case SELF_UPDATE_FAILED:
            yt::log::line("update: failed: %s", status.error);
            fail(status.error[0] != '\0' ? status.error : "the update failed");
            finish_engine();
            return;
        case SELF_UPDATE_CANCELLED:
            fail("update cancelled");
            finish_engine();
            return;
        default:
            sleep_ms(kPollMs);
            break;
        }
    }
}

void *worker_main(void *)
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g.mutex);
            g.wake.wait(lock, [] { return g.pending != Job::none || g.stopping; });
            if (g.stopping && g.pending == Job::none)
            {
                g.worker_exited = true;
                g.done.notify_all();
                return nullptr;
            }
            job = g.pending;
            g.pending = Job::none;
            g.worker_busy = true;
        }
        if (job == Job::check)
            run_check();
        else
            run_install();
        std::lock_guard<std::mutex> lock(g.mutex);
        g.worker_busy = false;
    }
    return nullptr;
}

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

} // namespace

void set_current_version(std::string version)
{
    std::lock_guard<std::mutex> lock(g.mutex);
    g.snapshot.current_version = std::move(version);
    g.snapshot.revision = ++g.revision;
}

void check()
{
    std::lock_guard<std::mutex> lock(g.mutex);
    // One job at a time, and never over a staged update.
    if (g.pending != Job::none || g.worker_busy || g.engine_live)
        return;
    g.pending = Job::check;
    start_worker_locked();
    g.wake.notify_one();
}

bool install()
{
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.pending != Job::none || g.worker_busy || g.engine_live)
        return false;
    if (g.snapshot.state != State::available || !g.has_asset || g.asset_url.empty())
        return false;
    if (g.asset_sha256.size() != 64)
    {
        // The helper checks the size and SHA-256 before it touches the app; without the
        // digest there is nothing to check the download against.
        g.snapshot.state = State::failed;
        g.snapshot.error = "the release asset has no sha256 digest";
        g.snapshot.revision = ++g.revision;
        return false;
    }
    if (!is_newer(g.latest, g.snapshot.current_version))
    {
        // The helper only installs over a content version it can compare with (NN.NNN.NNN).
        g.snapshot.state = State::failed;
        g.snapshot.error = "the app's version could not be read";
        g.snapshot.revision = ++g.revision;
        return false;
    }
    g.cancel_requested = false;
    g.pending = Job::install;
    start_worker_locked();
    g.wake.notify_one();
    return true;
}

void cancel()
{
    std::lock_guard<std::mutex> lock(g.mutex);
    // Before the worker has started the engine there is no phase to read: a cancel for a
    // queued install is remembered and honoured as soon as it starts. The worker is the
    // only thread that touches the engine, so the flag is all this sets.
    if (g.snapshot.state != State::downloading && g.pending != Job::install)
        return;
    g.cancel_requested = true;
}

Snapshot snapshot()
{
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.snapshot;
}

std::uint64_t revision()
{
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.revision;
}

std::string normalise_version(std::string tag)
{
    std::size_t begin = 0;
    std::size_t end = tag.size();
    while (begin < end && is_space(tag[begin]))
        ++begin;
    while (end > begin && is_space(tag[end - 1]))
        --end;
    if (begin < end && (tag[begin] == 'v' || tag[begin] == 'V'))
        ++begin;
    return tag.substr(begin, end - begin);
}

bool is_newer(std::string_view candidate, std::string_view current)
{
    // Both are content versions (NN.NNN.NNN), compared as three numbers by the same reader
    // the helper uses; a tag that isn't one is not comparable and never newer.
    const std::string left(candidate);
    const std::string right(current);
    int comparable = 0;
    const int order = update_check_version_compare(left.c_str(), right.c_str(), &comparable);
    return comparable != 0 && order > 0;
}

} // namespace yt::update

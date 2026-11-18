// youtube-tv - Settings > Developer > Run diagnostics: every part of the app, checked.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/diagnostics.hpp"

#include "core/save_file.hpp"
#include "yt/http.hpp"
#include "yt/innertube.hpp"
#include "yt/log.hpp"
#include "yt/log_share.hpp"
#include "yt/types.hpp"

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <utility>

namespace hui::app
{

struct Diagnostics::State
{
    mutable std::mutex mutex;
    std::vector<DiagLine> lines;
    bool running = false;
    std::uint64_t revision = 0;
    DiagInput input;
};

namespace
{

using Clock = std::chrono::steady_clock;

// "Me at the zoo": public, short, never age-gated; any YouTube answers it.
constexpr const char *kKnownVideo = "jNQXAC9IVRw";

void add(Diagnostics::State &state, DiagStatus status, const char *section, std::string name,
         std::string detail)
{
    yt::log::line("diag: [%s] %s / %s: %s", diag_status_word(status), section, name.c_str(),
                  detail.c_str());
    const std::lock_guard<std::mutex> lock(state.mutex);
    state.lines.push_back({status, section, std::move(name), std::move(detail)});
    ++state.revision;
}

long long ms_since(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

std::string ms_text(Clock::time_point start)
{
    return std::to_string(ms_since(start)) + " ms";
}

const char *codec_name(yt::Codec codec)
{
    switch (codec)
    {
    case yt::Codec::avc:
        return "avc";
    case yt::Codec::vp9:
        return "vp9";
    case yt::Codec::av1:
        return "av1";
    case yt::Codec::aac:
        return "aac";
    case yt::Codec::opus:
        return "opus";
    default:
        return "?";
    }
}

// Bytes under a folder, and how many files.
long long folder_bytes(const std::string &path, int *files, int depth = 0)
{
    long long total = 0;
    DIR *dir = ::opendir(path.c_str());
    if (dir == nullptr)
        return 0;
    while (const dirent *entry = ::readdir(dir))
    {
        const std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        const std::string child = path + "/" + name;
        struct stat info
        {
        };
        if (::stat(child.c_str(), &info) != 0)
            continue;
        if (S_ISDIR(info.st_mode))
        {
            if (depth < 4)
                total += folder_bytes(child, files, depth + 1);
        }
        else
        {
            total += static_cast<long long>(info.st_size);
            ++*files;
        }
    }
    ::closedir(dir);
    return total;
}

std::string megabytes(long long bytes)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

void check_storage(Diagnostics::State &state, const DiagInput &input)
{
    const char *section = "Storage";
    // The app's storage is 256 MB in all (param.json downloadDataSize).
    int files = 0;
    const long long total = folder_bytes(input.data_root, &files);
    add(state, total > (224ll << 20) ? DiagStatus::warn : DiagStatus::info, section, "App folder",
        megabytes(total) + " in " + std::to_string(files) + " files, of 256 MB");
    for (const char *child : {"thumbs", "dev", "backup", "feeds"})
    {
        int count = 0;
        const long long bytes = folder_bytes(input.data_root + "/" + child, &count);
        if (count > 0)
            add(state, DiagStatus::info, section, child,
                megabytes(bytes) + " in " + std::to_string(count) + " files");
    }
}

void check_console(Diagnostics::State &state, const DiagInput &input)
{
    const char *section = "Console";
    add(state, DiagStatus::info, section, "App version", input.version);
    add(state, DiagStatus::info, section, "Language",
        "app " + input.language + ", console " + input.console_language + ", YouTube " + input.hl +
            "-" + input.gl);
    add(state, DiagStatus::info, section, "Account",
        input.signed_in ? "signed in" : "signed out (guest)");
    add(state, DiagStatus::info, section, "Settings", input.settings);

    // Memory: the heap a 4K picture queue needs, asked for and given back.
    constexpr std::size_t kMegabytes = 96;
    void *block = std::malloc(kMegabytes << 20);
    add(state, block != nullptr ? DiagStatus::ok : DiagStatus::fail, section, "Memory",
        block != nullptr ? std::to_string(kMegabytes) + " MB free for video"
                         : "could not get " + std::to_string(kMegabytes) + " MB");
    std::free(block);

    // The app's folder: settings, accounts and the log live there. Written
    // the way the settings are (open/write/rename), read back the same way.
    const std::string path = input.data_root + "/diag_test.tmp";
    const std::string sample = "diagnostics " + input.version;
    const std::string failure = save::write_atomic(path, sample);
    std::string back;
    const bool read = failure.empty() && save::read_file(path, &back, 4096);
    std::remove(path.c_str());
    if (!failure.empty())
        add(state, DiagStatus::fail, section, "App folder",
            input.data_root + " cannot be written: " + failure);
    else if (!read || back != sample)
        add(state, DiagStatus::fail, section, "App folder",
            input.data_root + " wrote, but read back " + (read ? "something else" : "nothing"));
    else
        add(state, DiagStatus::ok, section, "App folder", input.data_root + " writes and reads");
}

void check_network(Diagnostics::State &state)
{
    const char *section = "Network";
    struct Target
    {
        const char *name;
        const char *url;
        long expected; // an answer that means "working" besides 2xx (0: none)
    };
    static const Target kTargets[] = {
        {"Internet (plain HTTP)", "http://connectivitycheck.gstatic.com/generate_204", 0},
        {"Google", "https://www.google.com/generate_204", 0},
        {"YouTube", "https://www.youtube.com/generate_204", 0},
        {"YouTube API", "https://youtubei.googleapis.com/generate_204", 0},
        // Sign-in takes a POST: a GET answers 404 when the service is up.
        {"Google sign-in", "https://oauth2.googleapis.com/device/code", 404},
        // A video without segments answers 404.
        {"SponsorBlock", "https://sponsor.ajay.app/api/skipSegments?videoID=dQw4w9WgXcQ", 404},
        {"DeArrow", "https://sponsor.ajay.app/api/branding?videoID=dQw4w9WgXcQ", 0},
        {"Return YouTube Dislike", "https://returnyoutubedislikeapi.com/votes?videoId=dQw4w9WgXcQ",
         0},
        {"Updates (GitHub)",
         "https://api.github.com/repos/animatian112/ytps5-sponsorblock/releases/latest", 0},
        {"dpaste.com (log sharing)", "https://dpaste.com/", 0},
        {"paste.rs (log sharing)", "https://paste.rs/", 0},
        {"litterbox (log sharing)", "https://litterbox.catbox.moe/", 0},
        {"0x0.st (log sharing)", "https://0x0.st/", 0},
    };
    // GitHub refuses a request without a User-Agent (403).
    const std::vector<yt::http::Header> headers = {{"User-Agent", "TubeBlock-PS5"},
                                                   {"Accept", "*/*"}};
    for (const Target &target : kTargets)
    {
        std::string host = target.url;
        host = host.substr(host.find("//") + 2);
        host = host.substr(0, host.find('/'));
        const Clock::time_point start = Clock::now();
        const yt::http::Response response = yt::http::get(target.url, headers, 12);
        DiagStatus status = DiagStatus::ok;
        std::string detail;
        if (!response.ok())
        {
            status = DiagStatus::fail;
            detail = response.error +
                     (response.detail.empty() ? std::string() : " (" + response.detail + ")") +
                     " - " + yt::http::describe_lookup(host);
        }
        else
        {
            if (response.status >= 400 && response.status != target.expected)
                status = DiagStatus::warn;
            detail = "HTTP " + std::to_string(response.status) + ", " + ms_text(start);
        }
        add(state, status, section, target.name, detail);
    }

    // Whether the console lets the app listen (the log's local link).
    yt::LogShare share;
    std::string refused;
    if (share.start(&refused))
    {
        add(state, DiagStatus::ok, section, "Local server", "listening at " + share.url());
        share.stop();
    }
    else
    {
        add(state, DiagStatus::info, section, "Local server",
            "not allowed (" + refused + "); the log goes up to a paste site instead");
    }
}

std::string feed_detail(const yt::FeedPage &page, Clock::time_point start)
{
    if (!page.error.empty())
        return page.error;
    std::string text = std::to_string(page.items.size()) + " items, " + ms_text(start);
    if (!page.items.empty())
        text += ", first: \"" + page.items.front().title + "\" (" + page.items.front().detail + ")";
    return text;
}

void check_video(Diagnostics::State &state, yt::InnerTube &tube, const std::string &video_id,
                 const std::string &who)
{
    const char *section = "YouTube";
    const std::string label = " " + video_id + who;
    yt::VideoInfo info;
    Clock::time_point start = Clock::now();
    const bool played = tube.player(video_id, &info);
    if (!played || !info.ok())
    {
        add(state, DiagStatus::fail, section, "Player" + label,
            (info.error.empty() ? std::string("no answer") : info.error) +
                (info.census.empty() ? std::string() : " [" + info.census + "]"));
    }
    else
    {
        int video = 0;
        int audio = 0;
        int ciphered = 0;
        int best = 0;
        std::set<std::string> codecs;
        for (const yt::Format &format : info.formats)
        {
            if (format.height > 0)
                ++video;
            else
                ++audio;
            ciphered += format.ciphered ? 1 : 0;
            best = std::max(best, format.height);
            codecs.insert(codec_name(format.codec));
        }
        std::string list;
        for (const std::string &codec : codecs)
            list += (list.empty() ? "" : " ") + codec;
        add(state, video > 0 && audio > 0 ? DiagStatus::ok : DiagStatus::fail, section,
            "Player" + label,
            "\"" + info.title + "\" via " + (info.client.empty() ? "?" : info.client) + ": " +
                std::to_string(video) + " video / " + std::to_string(audio) +
                " audio formats, up to " + std::to_string(best) + "p, " + list +
                (ciphered > 0 ? ", " + std::to_string(ciphered) + " ciphered" : "") + ", " +
                ms_text(start));
    }

    // The upload date: the player's microformat, else the watch page's line.
    yt::FeedPage page;
    start = Clock::now();
    tube.related(video_id, &page);
    add(state, page.error.empty() ? DiagStatus::ok : DiagStatus::fail, section,
        "Watch page" + label, feed_detail(page, start));
    // What the player shows when neither has a day: the web client's.
    std::string iso;
    std::string day;
    start = Clock::now();
    const bool asked = tube.upload_date(video_id, &iso, &day);
    add(state, !info.publish_date.empty() || asked ? DiagStatus::ok : DiagStatus::warn, section,
        "Upload date" + label,
        "player '" + info.publish_date + "', watch page '" + page.date_text + "', web '" +
            (iso.empty() ? day : iso) + "', " + ms_text(start));
}

void check_feeds(Diagnostics::State &state, const DiagInput &input, const std::string &token)
{
    const char *section = "YouTube";
    const bool account = !token.empty();
    const std::string who = account ? " (account)" : " (signed out)";
    yt::InnerTube tube;
    tube.set_timeout(15);
    tube.set_region(input.gl, input.hl);
    tube.set_access_token(token);

    yt::FeedPage home;
    Clock::time_point start = Clock::now();
    tube.home(&home);
    add(state, !home.items.empty() ? DiagStatus::ok : DiagStatus::fail, section, "Home" + who,
        feed_detail(home, start));

    if (account)
    {
        yt::FeedPage subscriptions;
        start = Clock::now();
        tube.subscriptions(&subscriptions);
        add(state, !subscriptions.items.empty() ? DiagStatus::ok : DiagStatus::warn, section,
            "Subscriptions" + who, feed_detail(subscriptions, start));
    }
    else
    {
        yt::FeedPage search;
        start = Clock::now();
        tube.search("music", &search);
        add(state, !search.items.empty() ? DiagStatus::ok : DiagStatus::fail, section,
            "Search \"music\"" + who, feed_detail(search, start));
    }

    check_video(state, tube, kKnownVideo, who);
    if (!input.last_video.empty() && input.last_video != kKnownVideo)
        check_video(state, tube, input.last_video, who);
}

void check_youtube(Diagnostics::State &state, const DiagInput &input)
{
    check_feeds(state, input, {});
    // The signed-in path is the one the app uses for an account: its watch
    // page is where a missing date would come from.
    if (!input.access_token.empty())
        check_feeds(state, input, input.access_token);
    else if (input.signed_in)
        add(state, DiagStatus::warn, "YouTube", "Account", "signed in, but no access token yet");
}

void *main_entry(void *argument)
{
    std::unique_ptr<std::shared_ptr<Diagnostics::State>> holder(
        static_cast<std::shared_ptr<Diagnostics::State> *>(argument));
    Diagnostics::State &state = **holder;
    DiagInput input;
    {
        const std::lock_guard<std::mutex> lock(state.mutex);
        input = state.input;
    }
    yt::log::line("diag: start");
    const Clock::time_point start = Clock::now();
    check_console(state, input);
    check_storage(state, input);
    check_network(state);
    check_youtube(state, input);
    add(state, DiagStatus::info, "Done", "Time", ms_text(start));
    const std::lock_guard<std::mutex> lock(state.mutex);
    state.running = false;
    ++state.revision;
    return nullptr;
}

} // namespace

const char *diag_status_word(DiagStatus status)
{
    switch (status)
    {
    case DiagStatus::ok:
        return "OK";
    case DiagStatus::warn:
        return "WARN";
    case DiagStatus::fail:
        return "FAIL";
    default:
        return "INFO";
    }
}

void Diagnostics::start(DiagInput input)
{
    if (running())
        return;
    state_ = std::make_shared<State>();
    state_->input = std::move(input);
    state_->running = true;
    auto *argument = new std::shared_ptr<State>(state_);
    pthread_attr_t attributes;
    pthread_t thread;
    bool created = false;
    if (pthread_attr_init(&attributes) == 0)
    {
        // The player check runs the player JS (QuickJS): a deep stack.
        pthread_attr_setstacksize(&attributes, 2u << 20);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        created = pthread_create(&thread, &attributes, main_entry, argument) == 0;
        pthread_attr_destroy(&attributes);
    }
    if (!created)
    {
        delete argument;
        add(*state_, DiagStatus::fail, "Diagnostics", "Worker", "no thread");
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->running = false;
    }
}

bool Diagnostics::running() const
{
    if (state_ == nullptr)
        return false;
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->running;
}

std::vector<DiagLine> Diagnostics::lines() const
{
    if (state_ == nullptr)
        return {};
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->lines;
}

std::uint64_t Diagnostics::revision() const
{
    if (state_ == nullptr)
        return 0;
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->revision;
}

std::string Diagnostics::report() const
{
    std::string text = "TubeBlock diagnostics\n\n";
    for (const DiagLine &line : lines())
    {
        text += std::string("[") + diag_status_word(line.status) + "] " + line.section + " / " +
                line.name + ": " + line.detail + "\n";
    }
    // The end of the log: what happened before the pass.
    std::string log = yt::log::read_all(8u << 20);
    constexpr std::size_t kTail = 150u << 10;
    if (log.size() > kTail)
        log = log.substr(log.size() - kTail);
    text += "\n---- yt.log (end) ----\n" + log;
    return text;
}

} // namespace hui::app

// youtube-tv - Settings > Developer > Run diagnostics: every part of the app, checked.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/diagnostics.hpp"

#include "yt/http.hpp"
#include "yt/innertube.hpp"
#include "yt/log.hpp"
#include "yt/log_share.hpp"
#include "yt/types.hpp"

#include <pthread.h>

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

    // The app's folder: settings, accounts and the log live there.
    const std::string path = input.data_root + "/diag_test.tmp";
    const std::string sample = "diagnostics " + input.version;
    bool written = false;
    if (std::FILE *file = std::fopen(path.c_str(), "wb"))
    {
        written = std::fwrite(sample.data(), 1, sample.size(), file) == sample.size();
        written = std::fclose(file) == 0 && written;
    }
    std::string back;
    if (std::FILE *file = std::fopen(path.c_str(), "rb"))
    {
        char buffer[64] = {};
        back.assign(buffer, std::fread(buffer, 1, sizeof buffer, file));
        std::fclose(file);
    }
    std::remove(path.c_str());
    add(state, written && back == sample ? DiagStatus::ok : DiagStatus::fail, section, "App folder",
        written ? (back == sample ? input.data_root + " writes and reads"
                                  : input.data_root + " wrote, read back wrong")
                : input.data_root + " cannot be written");
}

void check_network(Diagnostics::State &state)
{
    const char *section = "Network";
    static const char *const kTargets[][2] = {
        {"Internet (plain HTTP)", "http://connectivitycheck.gstatic.com/generate_204"},
        {"Google", "https://www.google.com/generate_204"},
        {"YouTube", "https://www.youtube.com/generate_204"},
        {"YouTube API", "https://youtubei.googleapis.com/generate_204"},
        {"Google sign-in", "https://oauth2.googleapis.com/device/code"},
        {"SponsorBlock", "https://sponsor.ajay.app/api/skipSegments?videoID=dQw4w9WgXcQ"},
        {"DeArrow", "https://sponsor.ajay.app/api/branding?videoID=dQw4w9WgXcQ"},
        {"Return YouTube Dislike", "https://returnyoutubedislikeapi.com/votes?videoId=dQw4w9WgXcQ"},
        {"Updates (GitHub)", "https://api.github.com/repos/animatian112/ytps5-sponsorblock"},
        {"dpaste.com (log sharing)", "https://dpaste.com/"},
        {"paste.rs (log sharing)", "https://paste.rs/"},
    };
    for (const auto &target : kTargets)
    {
        std::string host = target[1];
        host = host.substr(host.find("//") + 2);
        host = host.substr(0, host.find('/'));
        const Clock::time_point start = Clock::now();
        const yt::http::Response response = yt::http::get(target[1], {}, 12);
        // Any HTTP answer means the service was reached; 4xx/5xx are noted.
        DiagStatus status = DiagStatus::ok;
        std::string detail;
        if (!response.ok())
        {
            status = DiagStatus::fail;
            detail = response.error + " - " + yt::http::describe_lookup(host);
        }
        else
        {
            if (response.status >= 400)
                status = DiagStatus::warn;
            detail = "HTTP " + std::to_string(response.status) + ", " + ms_text(start);
        }
        add(state, status, section, target[0], detail);
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

void check_video(Diagnostics::State &state, yt::InnerTube &tube, const std::string &video_id)
{
    const char *section = "YouTube";
    const std::string label = " " + video_id;
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
    const bool dated = !info.publish_date.empty() || !page.date_text.empty();
    add(state, dated ? DiagStatus::ok : DiagStatus::warn, section, "Upload date" + label,
        "player '" + info.publish_date + "', watch page '" + page.date_text + "'" +
            (dated ? std::string() : " - neither has it; the card's age is used"));
}

void check_youtube(Diagnostics::State &state, const DiagInput &input)
{
    const char *section = "YouTube";
    yt::InnerTube tube;
    tube.set_timeout(15);
    tube.set_region(input.gl, input.hl);

    yt::FeedPage home;
    Clock::time_point start = Clock::now();
    tube.home(&home);
    add(state, !home.items.empty() ? DiagStatus::ok : DiagStatus::fail, section,
        "Home (signed out)", feed_detail(home, start));

    yt::FeedPage search;
    start = Clock::now();
    tube.search("music", &search);
    add(state, !search.items.empty() ? DiagStatus::ok : DiagStatus::fail, section,
        "Search \"music\"", feed_detail(search, start));

    check_video(state, tube, kKnownVideo);
    if (!input.last_video.empty() && input.last_video != kKnownVideo)
        check_video(state, tube, input.last_video);
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

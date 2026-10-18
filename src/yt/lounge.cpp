// youtube-tv - Cast from the phone: the YouTube "Link with TV code" screen.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/lounge.hpp"

#include "core/save_file.hpp"
#include "yt/http.hpp"
#include "yt/log.hpp"

#include "third_party/yyjson/yyjson.h"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <utility>

namespace yt
{
namespace
{

constexpr const char *kBase = "https://www.youtube.com/api/lounge";
constexpr const char *kApp = "ytcr";
constexpr std::size_t kStack = 1u << 20;

std::string encode(std::string_view text)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : text)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
            byte == '~')
            out += ch;
        else
        {
            out += '%';
            out += hex[byte >> 4];
            out += hex[byte & 15];
        }
    }
    return out;
}

std::string str(yyjson_val *value, const char *key)
{
    yyjson_val *field = yyjson_obj_get(value, key);
    if (yyjson_is_str(field))
        return yyjson_get_str(field);
    if (yyjson_is_num(field))
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%g", yyjson_get_num(field));
        return text;
    }
    return {};
}

std::vector<http::Header> form()
{
    return {{"Content-Type", "application/x-www-form-urlencoded"},
            {"Origin", "https://www.youtube.com"}};
}

std::string random_letters(std::size_t count, std::uint64_t seed)
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string out;
    std::uint64_t x = seed | 1u;
    for (std::size_t i = 0; i < count; ++i)
    {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out += letters[x % 36];
    }
    return out;
}

std::uint64_t now_seed()
{
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

} // namespace

struct Lounge::Impl
{
    std::string data_root;
    std::string name;

    mutable std::mutex mutex;
    std::condition_variable wake; // the sender's queue
    bool running = false;
    bool stopping = false;
    std::uint64_t generation = 0; // a stop() abandons the workers of the old run
    bool want_code = true;
    Status status;
    std::vector<Command> commands;
    std::deque<std::string> outbox; // "req0__sc=...&req0_x=..." bodies, without count/ofs

    // Session (worker side, guarded by mutex where the sender reads it).
    std::string screen_id;
    std::string device_id;
    std::string token;
    std::string sid;
    std::string gsessionid;
    long aid = 0;
    long rid = 1;
    long ofs = 0;

    // Player state for nowPlaying / onStateChange.
    std::string video_id;
    double position = 0.0;
    double duration = 0.0;
    int state = -1;
    int sent_state = -2;
    std::string sent_video = "\x01"; // differs from any id: nowPlaying goes out first
    std::string list_id;
    std::string current_index = "0";
    std::string cpn; // a playback nonce per video, as the TV apps send

    // The phone's YouTube app reads every field of these; a missing one has
    // crashed it, so they always go out whole. (Called with mutex held.)
    std::string fields() const
    {
        char text[256];
        std::snprintf(text, sizeof(text),
                      "&req0_currentTime=%.3f&req0_state=%d&req0_duration=%.3f"
                      "&req0_loadedTime=%.3f&req0_seekableStartTime=0&req0_seekableEndTime=%.3f",
                      position, state, duration, duration > 0.0 ? duration : position, duration);
        return text + std::string("&req0_cpn=") + encode(cpn);
    }
    std::string now_playing() const
    {
        if (video_id.empty())
            return "req0__sc=nowPlaying"; // nothing open: the bare event
        return "req0__sc=nowPlaying&req0_videoId=" + encode(video_id) +
               "&req0_listId=" + encode(list_id) + "&req0_currentIndex=" + encode(current_index) +
               fields();
    }
    std::string state_change() const
    {
        return "req0__sc=onStateChange" + fields();
    }
    std::chrono::steady_clock::time_point last_sent{};

    std::string bind_query() const
    {
        std::string q = std::string(kBase) +
                        "/bc/bind?device=LOUNGE_SCREEN&id=" + encode(device_id) +
                        "&name=" + encode(name) + "&app=" + kApp +
                        "&theme=cl&capabilities=dsp%2Cmic%2Cdpa&cst=m&mdxVersion=2&loungeIdToken=" +
                        encode(token) + "&VER=8&v=2&t=1&CVER=1";
        return q;
    }

    // ---- setup ----
    bool load_ids()
    {
        std::string data;
        if (hui::save::read_file(data_root + "/lounge.txt", &data, 4096))
        {
            const std::size_t tab = data.find('\t');
            if (tab != std::string::npos)
            {
                screen_id = data.substr(0, tab);
                device_id = data.substr(tab + 1);
                while (!device_id.empty() && (device_id.back() == '\n' || device_id.back() == '\r'))
                    device_id.pop_back();
            }
        }
        if (device_id.empty())
            device_id = random_letters(8, now_seed()) + "-" + random_letters(4, now_seed() * 3) +
                        "-" + random_letters(4, now_seed() * 5) + "-" +
                        random_letters(12, now_seed() * 7);
        if (screen_id.empty())
        {
            http::Request request;
            request.url = std::string(kBase) + "/pairing/generate_screen_id";
            request.timeout_s = 15;
            const http::Response response = http::perform(request);
            if (!response.http_ok() || response.body.empty())
            {
                log::line("lounge: screen id -> %ld %s", response.status, response.error.c_str());
                return false;
            }
            screen_id = response.body;
            while (!screen_id.empty() && (screen_id.back() == '\n' || screen_id.back() == ' '))
                screen_id.pop_back();
        }
        (void)hui::save::write_atomic(data_root + "/lounge.txt",
                                      screen_id + '\t' + device_id + '\n');
        return true;
    }

    bool fetch_token()
    {
        const http::Response response =
            http::post_json(std::string(kBase) + "/pairing/get_lounge_token_batch", form(),
                            "screen_ids=" + encode(screen_id), 15);
        if (!response.http_ok())
        {
            log::line("lounge: token -> %ld %s", response.status, response.error.c_str());
            return false;
        }
        yyjson_doc *doc = yyjson_read(response.body.data(), response.body.size(), 0);
        yyjson_val *screen =
            yyjson_arr_get_first(yyjson_obj_get(yyjson_doc_get_root(doc), "screens"));
        token = str(screen, "loungeToken");
        if (doc != nullptr)
            yyjson_doc_free(doc);
        return !token.empty();
    }

    void fetch_code()
    {
        const std::string body = "access_type=permanent&app=" + std::string(kApp) +
                                 "&lounge_token=" + encode(token) +
                                 "&screen_id=" + encode(screen_id) +
                                 "&screen_name=" + encode(name) + "&device_id=" + encode(device_id);
        const http::Response response = http::post_json(
            std::string(kBase) + "/pairing/get_pairing_code?ctx=pair", form(), body, 15);
        std::string code;
        for (const char ch : response.body)
            if (ch >= '0' && ch <= '9')
                code += ch;
        std::lock_guard<std::mutex> lock(mutex);
        if (response.http_ok() && code.size() >= 12)
        {
            status.code = code.substr(0, 3) + " " + code.substr(3, 3) + " " + code.substr(6, 3) +
                          " " + code.substr(9, 3);
            status.error.clear();
        }
        else
            status.error = "No TV code (" +
                           (response.error.empty() ? "HTTP " + std::to_string(response.status)
                                                   : response.error) +
                           ")";
        log::line("lounge: code -> %ld %s", response.status, status.code.c_str());
    }

    // ---- the session ----
    void handle(const std::string &name_, yyjson_val *args)
    {
        Command command;
        bool push = true;
        if (name_ == "setPlaylist" || name_ == "setVideo" || name_ == "updatePlaylist")
        {
            command.kind = Command::Kind::play_video;
            command.video_id = str(args, "videoId");
            command.time_s = std::atof(str(args, "currentTime").c_str());
            const std::string ids = str(args, "videoIds");
            std::size_t at = 0;
            while (at < ids.size())
            {
                std::size_t end = ids.find(',', at);
                if (end == std::string::npos)
                    end = ids.size();
                command.videos.push_back(ids.substr(at, end - at));
                at = end + 1;
            }
            if (name_ == "updatePlaylist" || command.video_id.empty())
                push = false; // only the list changed
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (yyjson_obj_get(args, "listId") != nullptr)
                    list_id = str(args, "listId");
                if (yyjson_obj_get(args, "currentIndex") != nullptr)
                    current_index = str(args, "currentIndex");
                if (current_index.empty())
                    current_index = "0";
                if (!command.video_id.empty() && command.video_id != video_id)
                {
                    video_id = command.video_id;
                    cpn = random_letters(16, now_seed());
                    position = command.time_s;
                    duration = 0.0;
                    state = 3;
                    wake.notify_all(); // nowPlaying for the new video
                }
            }
        }
        else if (name_ == "play")
            command.kind = Command::Kind::play;
        else if (name_ == "pause")
            command.kind = Command::Kind::pause;
        else if (name_ == "seekTo")
        {
            command.kind = Command::Kind::seek;
            command.time_s = std::atof(str(args, "newTime").c_str());
        }
        else if (name_ == "stopVideo")
            command.kind = Command::Kind::stop;
        else if (name_ == "next")
            command.kind = Command::Kind::next;
        else if (name_ == "previous")
            command.kind = Command::Kind::previous;
        else if (name_ == "setVolume")
        {
            command.kind = Command::Kind::volume;
            command.volume = std::atoi(str(args, "volume").c_str());
        }
        else
        {
            push = false;
            std::lock_guard<std::mutex> lock(mutex);
            if (name_ == "remoteConnected" || name_ == "loungeStatus")
            {
                const std::string who = str(args, "name");
                if (name_ == "remoteConnected" || !who.empty())
                {
                    status.connected = true;
                    if (!who.empty())
                        status.remote = who;
                }
                if (name_ == "remoteConnected")
                {
                    // What the TV apps tell a phone that just joined.
                    outbox.push_back("req0__sc=onHasPreviousNextChanged&req0_hasPrevious=true"
                                     "&req0_hasNext=true");
                    outbox.push_back("req0__sc=onVolumeChanged&req0_volume=100&req0_muted=false");
                    outbox.push_back(now_playing());
                    wake.notify_all();
                }
            }
            else if (name_ == "remoteDisconnected")
            {
                status.connected = false;
                status.remote.clear();
            }
            else if (name_ == "getNowPlaying")
            {
                outbox.push_back(now_playing());
                wake.notify_all();
            }
            else if (name_ == "getVolume")
            {
                outbox.push_back("req0__sc=onVolumeChanged&req0_volume=100&req0_muted=false");
                wake.notify_all();
            }
        }
        if (push)
        {
            std::lock_guard<std::mutex> lock(mutex);
            status.connected = true;
            commands.push_back(std::move(command));
        }
    }

    // Reads "len\n[[id,[name,args]],...]" pieces of the stream.
    void feed_events(std::string &buffer)
    {
        for (const std::string &chunk : take_lounge_chunks(buffer))
        {
            yyjson_doc *doc = yyjson_read(chunk.data(), chunk.size(), 0);
            yyjson_val *list = yyjson_doc_get_root(doc);
            for (std::size_t i = 0, n = yyjson_arr_size(list); i < n; ++i)
            {
                yyjson_val *event = yyjson_arr_get(list, i);
                const long id = static_cast<long>(yyjson_get_num(yyjson_arr_get(event, 0)));
                yyjson_val *body = yyjson_arr_get(event, 1);
                const char *kind = yyjson_get_str(yyjson_arr_get(body, 0));
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    aid = std::max(aid, id);
                }
                if (kind == nullptr)
                    continue;
                const std::string name_ = kind;
                if (name_ == "c")
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    sid = yyjson_get_str(yyjson_arr_get(body, 1)) != nullptr
                              ? yyjson_get_str(yyjson_arr_get(body, 1))
                              : "";
                }
                else if (name_ == "S")
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    gsessionid = yyjson_get_str(yyjson_arr_get(body, 1)) != nullptr
                                     ? yyjson_get_str(yyjson_arr_get(body, 1))
                                     : "";
                }
                else if (name_ != "noop")
                {
                    log::line("lounge: event %s", name_.c_str());
                    handle(name_, yyjson_arr_get(body, 1));
                }
            }
            if (doc != nullptr)
                yyjson_doc_free(doc);
        }
    }

    bool bind()
    {
        long r = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            sid.clear();
            gsessionid.clear();
            aid = 0;
            ofs = 0;
            r = rid++;
        }
        const std::string url =
            bind_query() + "&RID=" + std::to_string(r) + "&zx=" + random_letters(12, now_seed());
        const http::Response response = http::post_json(url, form(), "count=0", 20);
        if (!response.http_ok())
        {
            log::line("lounge: bind -> %ld %s", response.status, response.error.c_str());
            return false;
        }
        std::string buffer = response.body;
        feed_events(buffer);
        std::lock_guard<std::mutex> lock(mutex);
        log::line("lounge: bound (sid %s)", sid.empty() ? "missing" : "ok");
        return !sid.empty() && !gsessionid.empty();
    }

    // One long poll; false when the session has to be bound again.
    bool poll(std::uint64_t run)
    {
        std::string url;
        {
            std::lock_guard<std::mutex> lock(mutex);
            url = bind_query() + "&SID=" + encode(sid) + "&gsessionid=" + encode(gsessionid) +
                  "&RID=rpc&CI=0&TYPE=xmlhttp&AID=" + std::to_string(aid) +
                  "&zx=" + random_letters(12, now_seed());
        }
        std::string buffer;
        http::Request request;
        request.url = url;
        request.timeout_s = 300;
        request.on_body = [&](std::string_view piece)
        {
            buffer.append(piece.data(), piece.size());
            feed_events(buffer);
            std::lock_guard<std::mutex> lock(mutex);
            return !stopping && generation == run && !want_code;
        };
        const http::Response response = http::perform(request);
        // Any refusal ends the session: answering it with an instant new
        // poll hammered YouTube and starved the video's own downloads.
        if (response.status != 0 && !response.http_ok())
        {
            log::line("lounge: session gone (%ld)", response.status);
            return false;
        }
        if (!response.error.empty() && response.error != "timeout")
        {
            log::line("lounge: poll %s", response.error.c_str());
            return false;
        }
        return true;
    }

    // ---- the sender ----
    void send(const std::string &message)
    {
        std::string url;
        std::string body;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (sid.empty())
                return;
            url = bind_query() + "&SID=" + encode(sid) + "&gsessionid=" + encode(gsessionid) +
                  "&RID=" + std::to_string(rid++) + "&AID=" + std::to_string(aid) +
                  "&zx=" + random_letters(12, now_seed());
            body = "count=1&ofs=" + std::to_string(ofs++) + "&" + message;
        }
        const http::Response response = http::post_json(url, form(), body, 15);
        if (!response.http_ok())
            log::line("lounge: send -> %ld %s", response.status, response.error.c_str());
    }

    void sender(std::uint64_t run)
    {
        for (;;)
        {
            std::string message;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock,
                          [&]
                          {
                              return stopping || generation != run || !outbox.empty() ||
                                     (!sid.empty() &&
                                      (state != sent_state || video_id != sent_video));
                          });
                if (stopping || generation != run)
                    return;
                if (!outbox.empty())
                {
                    message = std::move(outbox.front());
                    outbox.pop_front();
                }
                else
                {
                    // A new video is announced with nowPlaying, a change of
                    // the same one with onStateChange.
                    message = video_id != sent_video ? now_playing() : state_change();
                    sent_state = state;
                    sent_video = video_id;
                }
            }
            send(message);
        }
    }

    // Waits `seconds` (cut short by a new-code request); false when the run
    // is over.
    bool idle(std::uint64_t run, int seconds)
    {
        for (int i = 0; i < seconds * 10; ++i)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping || generation != run)
                    return false;
                if (want_code)
                    return true;
            }
            struct timespec pause = {0, 100000000};
            nanosleep(&pause, nullptr);
        }
        return true;
    }

    void main_loop(std::uint64_t run)
    {
        int failures = 0;
        while (true)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping || generation != run)
                    return;
            }
            if ((screen_id.empty() && !load_ids()) || !fetch_token())
            {
                std::lock_guard<std::mutex> lock(mutex);
                status.error = "Couldn't reach YouTube's cast service";
            }
            else
            {
                bool code = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    code = want_code;
                    want_code = false;
                }
                if (code)
                    fetch_code();
                if (bind())
                {
                    const auto bound = std::chrono::steady_clock::now();
                    auto asked = bound;
                    while (poll(run))
                    {
                        const bool quick =
                            std::chrono::steady_clock::now() - asked < std::chrono::seconds(3);
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            if (stopping || generation != run)
                                return;
                            if (want_code)
                                break; // a new code: fetch it, then bind again
                        }
                        // A poll that answers at once is never repeated at
                        // once: at most one every 3 s.
                        if (quick && !idle(run, 3))
                            return;
                        asked = std::chrono::steady_clock::now();
                    }
                    // A session that lasted a while was a good one; one that
                    // died at once backs off like a failure.
                    if (std::chrono::steady_clock::now() - bound > std::chrono::seconds(60))
                    {
                        failures = 0;
                        if (!idle(run, 2))
                            return;
                        continue;
                    }
                }
            }
            // Back off: 5 s, 10 s ... up to two minutes between tries.
            ++failures;
            const int wait = std::min(120, 5 * failures);
            for (int i = 0; i < wait * 10; ++i)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopping || generation != run || want_code)
                        break;
                }
                struct timespec pause = {0, 100000000};
                nanosleep(&pause, nullptr);
            }
        }
    }
};

namespace
{

struct ThreadArgs
{
    std::shared_ptr<Lounge::Impl> impl;
    std::uint64_t run;
    bool sender;
};

void *thread_main(void *argument)
{
    std::unique_ptr<ThreadArgs> args(static_cast<ThreadArgs *>(argument));
    if (args->sender)
        args->impl->sender(args->run);
    else
        args->impl->main_loop(args->run);
    return nullptr;
}

bool spawn(std::shared_ptr<Lounge::Impl> impl, std::uint64_t run, bool sender)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0)
        return false;
    pthread_attr_setstacksize(&attributes, kStack);
    auto *args = new ThreadArgs{std::move(impl), run, sender};
    pthread_t thread;
    const bool ok = pthread_create(&thread, &attributes, &thread_main, args) == 0;
    if (ok)
        pthread_detach(thread);
    else
        delete args;
    pthread_attr_destroy(&attributes);
    return ok;
}

} // namespace

Lounge::Lounge(std::string data_root, std::string screen_name) : impl_(std::make_shared<Impl>())
{
    impl_->data_root = std::move(data_root);
    impl_->name = std::move(screen_name);
}

Lounge::~Lounge()
{
    stop();
}

void Lounge::start()
{
    std::uint64_t run = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->running)
            return;
        impl_->running = true;
        impl_->stopping = false;
        run = ++impl_->generation;
        impl_->want_code = true;
    }
    if (!spawn(impl_, run, false) || !spawn(impl_, run, true))
        log::line("lounge: no worker thread");
}

void Lounge::stop()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->running)
        return;
    impl_->running = false;
    impl_->stopping = true;
    ++impl_->generation;
    impl_->status = Status{};
    impl_->wake.notify_all();
}

bool Lounge::running() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->running;
}

void Lounge::new_code()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->want_code = true;
    impl_->status.code.clear();
}

Lounge::Status Lounge::status() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->status;
}

std::vector<Lounge::Command> Lounge::take_commands()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<Command> out;
    out.swap(impl_->commands);
    return out;
}

void Lounge::report(const std::string &video_id, double position_s, double duration_s, int state)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->status.connected)
        return;
    const auto now = std::chrono::steady_clock::now();
    const bool changed = state != impl_->state || video_id != impl_->video_id;
    if (video_id != impl_->video_id)
        impl_->cpn = random_letters(16, now_seed());
    impl_->video_id = video_id;
    impl_->position = position_s;
    impl_->duration = duration_s;
    impl_->state = state;
    // A change at once; the position every 5 s while playing.
    if (changed || now - impl_->last_sent > std::chrono::seconds(5))
    {
        impl_->last_sent = now;
        impl_->sent_state = -2;
        impl_->wake.notify_all();
    }
}

} // namespace yt

// youtube-tv - DeArrow: community titles and thumbnails, as SmartTube shows.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/dearrow.hpp"

#include "third_party/yyjson/yyjson.h"
#include "yt/http.hpp"
#include "yt/log.hpp"

#include <pthread.h>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace yt
{

namespace
{

constexpr int kWorkers = 2;
// Cards scrolled past are not worth asking about any more: only the newest
// requests wait, the oldest are forgotten (and asked again if seen again).
constexpr std::size_t kMaxPending = 96;
constexpr std::size_t kMaxCached = 4000;

// A first entry is shown when it is trusted: locked, or not voted down.
bool trusted(yyjson_val *entry)
{
    yyjson_val *locked = yyjson_obj_get(entry, "locked");
    if (yyjson_is_bool(locked) && yyjson_get_bool(locked))
        return true;
    yyjson_val *votes = yyjson_obj_get(entry, "votes");
    return yyjson_is_num(votes) && yyjson_get_num(votes) >= 0.0;
}

bool is_original(yyjson_val *entry)
{
    yyjson_val *original = yyjson_obj_get(entry, "original");
    return yyjson_is_bool(original) && yyjson_get_bool(original);
}

// JavaScript's shortest round-trip spelling of a number, as the extension
// sends it: the thumbnail service caches by the exact time it was asked.
std::string number_text(double value)
{
    char buffer[40];
    for (int precision = 1; precision <= 17; ++precision)
    {
        std::snprintf(buffer, sizeof buffer, "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value)
            break;
    }
    return buffer;
}

} // namespace

std::string clean_dearrow_title(std::string title)
{
    std::size_t start = 0;
    while (start < title.size() && (title[start] == ' ' || title[start] == '\t'))
        ++start;
    if (start < title.size() && title[start] == '>')
        ++start;
    std::size_t end = title.size();
    while (end > start &&
           (title[end - 1] == ' ' || title[end - 1] == '\t' || title[end - 1] == '\n'))
        --end;
    return title.substr(start, end - start);
}

bool parse_branding(std::string_view json, Branding *out)
{
    if (out == nullptr)
        return false;
    *out = Branding{};
    yyjson_doc *doc = yyjson_read(json.data(), json.size(), 0);
    if (doc == nullptr)
        return false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root))
    {
        yyjson_doc_free(doc);
        return false;
    }
    yyjson_val *titles = yyjson_obj_get(root, "titles");
    if (yyjson_is_arr(titles) && yyjson_arr_size(titles) > 0)
    {
        yyjson_val *first = yyjson_arr_get_first(titles);
        yyjson_val *title = yyjson_obj_get(first, "title");
        if (trusted(first) && !is_original(first) && yyjson_is_str(title))
            out->title = clean_dearrow_title(yyjson_get_str(title));
    }
    yyjson_val *thumbnails = yyjson_obj_get(root, "thumbnails");
    if (yyjson_is_arr(thumbnails) && yyjson_arr_size(thumbnails) > 0)
    {
        yyjson_val *first = yyjson_arr_get_first(thumbnails);
        yyjson_val *timestamp = yyjson_obj_get(first, "timestamp");
        if (trusted(first) && !is_original(first) && yyjson_is_num(timestamp))
        {
            out->has_thumbnail = true;
            out->thumbnail_s = yyjson_get_num(timestamp);
        }
    }
    yyjson_doc_free(doc);
    return true;
}

std::string dearrow_thumbnail_url(const std::string &video_id, double time_s)
{
    return "https://dearrow-thumb.ajay.app/api/v1/getThumbnail?videoID=" + video_id +
           "&time=" + number_text(time_s);
}

struct DeArrow::Impl
{
    struct Entry
    {
        bool done = false;
        Branding branding;
    };

    mutable std::mutex mutex;
    std::condition_variable wake;
    bool quit = false;
    bool started = false;
    bool alt = false;
    std::deque<std::string> pending; // newest at the back
    std::unordered_map<std::string, Entry> cache;
    std::uint64_t revision = 0;

    void run()
    {
        for (;;)
        {
            std::string id;
            std::string url;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return quit || !pending.empty(); });
                if (quit)
                    return;
                id = std::move(pending.back());
                pending.pop_back();
                url =
                    std::string(alt ? "https://api.sponsor.ajay.app" : "https://sponsor.ajay.app") +
                    "/api/branding?videoID=" + id;
            }
            const http::Response response = http::get(url, {}, 8);
            Branding branding;
            if (response.http_ok())
            {
                if (!parse_branding(response.body, &branding))
                    log::line("dearrow: %s unreadable answer", id.c_str());
            }
            else if (response.status != 404)
            {
                // 404 is "nobody submitted anything": the common case, quiet.
                log::line("dearrow: %s HTTP %ld %s", id.c_str(), response.status,
                          response.error.c_str());
            }
            std::lock_guard<std::mutex> lock(mutex);
            Entry &entry = cache[id];
            entry.done = true;
            entry.branding = std::move(branding);
            ++revision;
        }
    }
};

namespace
{

void *dearrow_main(void *argument)
{
    std::unique_ptr<std::shared_ptr<DeArrow::Impl>> impl(
        static_cast<std::shared_ptr<DeArrow::Impl> *>(argument));
    (*impl)->run();
    return nullptr;
}

} // namespace

DeArrow::DeArrow() : impl_(std::make_shared<Impl>())
{
}

DeArrow::~DeArrow()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->quit = true;
    impl_->wake.notify_all();
}

void DeArrow::set_alt_server(bool alt)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->alt = alt;
}

bool DeArrow::lookup(const std::string &video_id, Branding *out)
{
    if (video_id.size() != 11)
        return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->cache.find(video_id);
    if (found != impl_->cache.end())
    {
        if (!found->second.done)
            return false;
        if (out != nullptr)
            *out = found->second.branding;
        return true;
    }
    if (impl_->cache.size() >= kMaxCached)
    {
        // Forget what is known; whatever is still waiting stays queued.
        for (auto it = impl_->cache.begin(); it != impl_->cache.end();)
            it = it->second.done ? impl_->cache.erase(it) : std::next(it);
    }
    impl_->cache[video_id] = Impl::Entry{};
    impl_->pending.push_back(video_id);
    while (impl_->pending.size() > kMaxPending)
    {
        impl_->cache.erase(impl_->pending.front());
        impl_->pending.pop_front();
    }
    if (!impl_->started)
    {
        // The threads start with the first card: none when DeArrow is off.
        impl_->started = true;
        for (int i = 0; i < kWorkers; ++i)
        {
            pthread_attr_t attributes;
            if (pthread_attr_init(&attributes) != 0)
                continue;
            pthread_attr_setstacksize(&attributes, 512u << 10);
            auto *argument = new std::shared_ptr<Impl>(impl_);
            pthread_t thread;
            if (pthread_create(&thread, &attributes, &dearrow_main, argument) == 0)
                pthread_detach(thread);
            else
                delete argument;
            pthread_attr_destroy(&attributes);
        }
    }
    impl_->wake.notify_all();
    return false;
}

std::uint64_t DeArrow::revision() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->revision;
}

} // namespace yt

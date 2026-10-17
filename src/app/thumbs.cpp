// youtube-tv - Thumbnail cache: download workers, frame-side upload.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/thumbs.hpp"

#include "gfx/gl_batch.hpp"
#include "yt/http.hpp"
#include "yt/image.hpp"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hui::app
{
namespace
{

constexpr std::size_t kWorkerStack = 1u << 20; // 1 MiB, like every worker
constexpr int kWorkers = 6;
// A failed download (a timeout in a burst, a dropped connection) is tried
// again after this many frames (~10 s) instead of staying grey for good.
constexpr std::uint64_t kRetryFrames = 600;
// Queued downloads nobody has asked for in this many frames (~2 s) are off
// screen: they are dropped and asked for again if their card comes back.
constexpr std::uint64_t kStaleFrames = 120;
// A thumbnail is small; refuse anything that looks like a video.
constexpr std::size_t kMaxBytes = 2u << 20;

bool spawn(pthread_t *thread, void *(*entry)(void *), void *argument)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0)
        return false;
    pthread_attr_setstacksize(&attributes, kWorkerStack);
    const int created = pthread_create(thread, &attributes, entry, argument);
    pthread_attr_destroy(&attributes);
    return created == 0;
}

struct Entry
{
    std::uint32_t texture = 0;
    std::uint64_t last_used = 0;
    std::uint64_t failed_at = 0;
    int width = 0;
    int height = 0;
    bool failed = false;
};

// A download waiting for a worker; `asked` is the frame it was last drawn.
struct Job
{
    std::string url;
    std::uint64_t asked = 0;
};

} // namespace

struct ThumbCache::Impl
{
    gfx::GlBatch &batch;
    Destroy destroy;
    std::size_t capacity;

    // ---- frame side ----
    std::unordered_map<std::string, Entry> entries;
    // Queue of keys waiting for a download slot, and the ones in flight.
    std::unordered_map<std::string, bool> in_flight;
    std::uint64_t clock = 0;
    std::uint64_t frame = 0;
    // A decoded image the workers produced; uploaded by pump().
    struct Ready
    {
        std::string key;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgba;
        bool failed = false;
    };
    std::vector<Ready> ready;

    // ---- worker side ----
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool quit = false;
    // Newest first: a worker takes the job drawn most recently, so the cards
    // on screen load before the ones a previous screen (or account) queued.
    std::deque<Job> work;
    bool quiet = false;
    int active = 0; // downloads running on the workers
    pthread_t threads[kWorkers] = {};

    Impl(gfx::GlBatch &batch_, Destroy destroy_, std::size_t capacity_)
        : batch(batch_), destroy(destroy_), capacity(capacity_)
    {
    }

    void worker_loop()
    {
        for (;;)
        {
            std::string key;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock,
                          [this] { return quit || (!work.empty() && (!quiet || active == 0)); });
                if (quit)
                    return;
                ++active;
                auto newest = work.begin();
                for (auto it = work.begin(); it != work.end(); ++it)
                    if (it->asked > newest->asked)
                        newest = it;
                key = std::move(newest->url);
                work.erase(newest);
            }

            Ready result;
            result.key = key;
            yt::http::Request request;
            request.url = key;
            request.timeout_s = 15;
            request.max_bytes = kMaxBytes;
            const yt::http::Response response = yt::http::perform(request);
            std::string error;
            if (response.ok() && response.status == 200 &&
                yt::decode_image(response.body, &result.width, &result.height, &result.rgba,
                                 &error))
            {
                const std::size_t cap = static_cast<std::size_t>(kMaxBytes);
                if (result.width <= 0 || result.height <= 0 ||
                    static_cast<std::size_t>(result.width) * result.height > cap)
                {
                    result.rgba.clear();
                    result.failed = true;
                }
            }
            else
            {
                result.failed = true;
            }

            std::lock_guard<std::mutex> lock(mutex);
            ready.push_back(std::move(result));
            --active;
            wake.notify_all();
        }
    }

    static void *worker_entry(void *argument)
    {
        static_cast<Impl *>(argument)->worker_loop();
        return nullptr;
    }

    void evict_if_needed()
    {
        while (entries.size() > capacity)
        {
            auto victim = entries.begin();
            for (auto it = entries.begin(); it != entries.end(); ++it)
            {
                if (it->second.last_used < victim->second.last_used)
                    victim = it;
            }
            if (victim->second.texture != 0 && destroy != nullptr)
                destroy(victim->second.texture);
            entries.erase(victim);
        }
    }
};

ThumbCache::ThumbCache(gfx::GlBatch &batch, Destroy destroy, std::size_t capacity)
    : impl_(std::make_unique<Impl>(batch, destroy, capacity))
{
    for (pthread_t &thread : impl_->threads)
    {
        if (!spawn(&thread, &Impl::worker_entry, impl_.get()))
            thread = 0;
    }
}

ThumbCache::~ThumbCache()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->quit = true;
    }
    impl_->wake.notify_all();
    for (pthread_t &thread : impl_->threads)
    {
        if (thread != 0)
            pthread_join(thread, nullptr);
    }
}

std::uint32_t ThumbCache::texture(const std::string &url)
{
    if (url.empty())
        return 0;
    auto it = impl_->entries.find(url);
    if (it != impl_->entries.end())
    {
        it->second.last_used = ++impl_->clock;
        if (!it->second.failed || impl_->frame - it->second.failed_at < kRetryFrames)
            return it->second.texture;
        impl_->entries.erase(it); // try a failed one again
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->in_flight.find(url) == impl_->in_flight.end())
    {
        impl_->in_flight[url] = true;
        impl_->work.push_back({url, impl_->frame});
        impl_->wake.notify_one();
    }
    else
    {
        // Still queued: drawn again this frame, so it moves up.
        for (Job &job : impl_->work)
            if (job.url == url)
            {
                job.asked = impl_->frame;
                break;
            }
    }
    return 0;
}

void ThumbCache::set_quiet(bool quiet)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->quiet == quiet)
        return;
    impl_->quiet = quiet;
    impl_->wake.notify_all();
}

float ThumbCache::aspect(const std::string &url) const
{
    const auto it = impl_->entries.find(url);
    if (it == impl_->entries.end() || it->second.height <= 0)
        return 0.0f;
    return static_cast<float>(it->second.width) / static_cast<float>(it->second.height);
}

std::uint32_t ThumbCache::upload(const std::string &key, const std::uint8_t *rgba, int width,
                                 int height)
{
    if (key.empty() || rgba == nullptr || width <= 0 || height <= 0)
        return 0;
    auto it = impl_->entries.find(key);
    if (it != impl_->entries.end() && it->second.texture != 0)
    {
        it->second.last_used = ++impl_->clock;
        return it->second.texture;
    }
    const std::uint32_t texture = impl_->batch.create_texture(width, height, rgba);
    Entry entry;
    entry.texture = texture;
    entry.last_used = ++impl_->clock;
    impl_->entries[key] = entry;
    impl_->evict_if_needed();
    return texture;
}

void ThumbCache::pump()
{
    std::vector<Impl::Ready> ready;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ++impl_->frame;
        // A few uploads a frame: a burst of finished downloads uploaded at
        // once made the UI hitch; the rest wait for the next frames.
        constexpr std::size_t kUploadsPerFrame = 3;
        const std::size_t take = std::min(kUploadsPerFrame, impl_->ready.size());
        ready.assign(std::make_move_iterator(impl_->ready.begin()),
                     std::make_move_iterator(impl_->ready.begin() + static_cast<long>(take)));
        impl_->ready.erase(impl_->ready.begin(), impl_->ready.begin() + static_cast<long>(take));
        for (const Impl::Ready &item : ready)
            impl_->in_flight.erase(item.key);
        // Drop queued downloads no card has asked for lately.
        const std::uint64_t now = impl_->frame;
        for (auto job = impl_->work.begin(); job != impl_->work.end();)
        {
            if (now - job->asked > kStaleFrames)
            {
                impl_->in_flight.erase(job->url);
                job = impl_->work.erase(job);
            }
            else
                ++job;
        }
    }
    for (Impl::Ready &item : ready)
    {
        if (item.failed)
        {
            Entry entry;
            entry.failed = true;
            entry.failed_at = impl_->frame;
            entry.last_used = ++impl_->clock;
            impl_->entries[item.key] = entry;
            continue;
        }
        const std::uint32_t texture =
            impl_->batch.create_texture(item.width, item.height, item.rgba.data());
        Entry entry;
        entry.texture = texture;
        entry.width = item.width;
        entry.height = item.height;
        entry.last_used = ++impl_->clock;
        impl_->entries[item.key] = entry;
        impl_->evict_if_needed();
    }
}

void ThumbCache::clear()
{
    for (auto &pair : impl_->entries)
    {
        if (pair.second.texture != 0 && impl_->destroy != nullptr)
            impl_->destroy(pair.second.texture);
    }
    impl_->entries.clear();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->work.clear();
    impl_->ready.clear();
    impl_->in_flight.clear();
}

} // namespace hui::app

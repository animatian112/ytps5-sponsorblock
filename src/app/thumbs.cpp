// youtube-tv - Thumbnail cache: download workers, frame-side upload.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/thumbs.hpp"

#include "core/save_file.hpp"
#include "gfx/gl_batch.hpp"
#include "yt/http.hpp"
#include "yt/image.hpp"
#include "yt/log.hpp"

#include <pthread.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <condition_variable>
#include <cstdio>
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
// Pictures kept on disk: the app's storage is 256 MB in all (param.json
// downloadDataSize), and a full disk keeps the settings from being saved.
// Past either limit the oldest go, down to three quarters of it.
constexpr std::size_t kDiskFiles = 800;
constexpr long long kDiskBytes = 64ll << 20;

// The file a picture is kept in: a hash of its address. A video picture on
// i.ytimg.com is keyed without its query (the signature part can change
// between answers while the picture stays the same).
std::string disk_name(const std::string &url)
{
    std::string key = url;
    if (key.find("ytimg.com/vi") != std::string::npos)
        key = key.substr(0, key.find('?'));
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char ch : key)
    {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    char name[24];
    std::snprintf(name, sizeof name, "%016llx.img", static_cast<unsigned long long>(hash));
    return name;
}

// Halves a picture (2x2 boxes) until it is at most kMaxWidth wide: DeArrow's
// thumbnails are full video frames, and a card never shows more.
constexpr int kMaxWidth = 720;

void shrink(std::vector<std::uint8_t> &rgba, int &width, int &height)
{
    while (width > kMaxWidth && height >= 2)
    {
        const int w = width / 2;
        const int h = height / 2;
        std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) *
                                      4u);
        for (int y = 0; y < h; ++y)
        {
            const std::uint8_t *row0 = rgba.data() + static_cast<std::size_t>(2 * y) * width * 4u;
            const std::uint8_t *row1 = row0 + static_cast<std::size_t>(width) * 4u;
            std::uint8_t *dst = out.data() + static_cast<std::size_t>(y) * w * 4u;
            for (int x = 0; x < w; ++x)
            {
                for (int c = 0; c < 4; ++c)
                {
                    const int sum = row0[8 * x + c] + row0[8 * x + 4 + c] + row1[8 * x + c] +
                                    row1[8 * x + 4 + c];
                    dst[4 * x + c] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
        }
        rgba = std::move(out);
        width = w;
        height = h;
    }
}

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
    // Queued or running downloads; the value is the frame the picture was
    // last asked for (its priority), looked up instead of searching the queue.
    std::unordered_map<std::string, std::uint64_t> in_flight;
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
    int active = 0;       // downloads running on the workers
    std::string disk_dir; // empty: no disk copies
    int disk_writes = 0;
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
                std::uint64_t newest_asked = 0;
                for (auto it = work.begin(); it != work.end(); ++it)
                {
                    const auto found = in_flight.find(it->url);
                    const std::uint64_t asked = found != in_flight.end() ? found->second : 0;
                    if (asked >= newest_asked)
                    {
                        newest = it;
                        newest_asked = asked;
                    }
                }
                key = std::move(newest->url);
                work.erase(newest);
            }

            Ready result;
            result.key = key;
            std::string directory;
            {
                std::lock_guard<std::mutex> lock(mutex);
                directory = disk_dir;
            }
            const std::string path =
                directory.empty() ? std::string() : directory + "/" + disk_name(key);
            // The copy kept from an earlier start first: no network wait.
            std::string kept;
            bool from_disk = false;
            if (!path.empty() && hui::save::read_file(path, &kept, kMaxBytes))
            {
                std::string error;
                from_disk =
                    yt::decode_image(kept, &result.width, &result.height, &result.rgba, &error);
                if (!from_disk)
                    std::remove(path.c_str());
            }
            yt::http::Response response;
            if (!from_disk)
            {
                yt::http::Request request;
                request.url = key;
                request.timeout_s = 15;
                request.max_bytes = kMaxBytes;
                response = yt::http::perform(request);
            }
            std::string error;
            if (from_disk || (response.ok() && response.status == 200 &&
                              yt::decode_image(response.body, &result.width, &result.height,
                                               &result.rgba, &error)))
            {
                if (!from_disk && !path.empty())
                    keep_on_disk(directory, path, response.body);
                const std::size_t cap = static_cast<std::size_t>(kMaxBytes);
                if (result.width <= 0 || result.height <= 0 ||
                    static_cast<std::size_t>(result.width) * result.height > cap)
                {
                    result.rgba.clear();
                    result.failed = true;
                }
                else if (key.find("dearrow-thumb") != std::string::npos)
                    shrink(result.rgba, result.width, result.height);
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

    void keep_on_disk(const std::string &directory, const std::string &path,
                      const std::string &body)
    {
        const std::string failure = hui::save::write_atomic(path, body);
        bool prune = !failure.empty(); // a full disk: make room now
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++disk_writes;
            // The first write of a run checks what earlier runs left.
            prune = prune || disk_writes == 1 || disk_writes % 50 == 0;
        }
        if (prune)
            prune_disk(directory);
    }

    // Oldest pictures first, until the cache is under both limits again
    // (half of them, with half: the disk was found full).
    static void prune_disk(const std::string &directory, bool half = false)
    {
        struct Entry
        {
            std::string path;
            long long bytes;
            long long when;
        };
        std::vector<Entry> entries;
        long long total = 0;
        for (const std::string &name : hui::save::list_files(directory))
        {
            const std::string path = directory + "/" + name;
            struct stat info
            {
            };
            if (::stat(path.c_str(), &info) != 0)
                continue;
            entries.push_back({path, static_cast<long long>(info.st_size),
                               static_cast<long long>(info.st_mtime)});
            total += static_cast<long long>(info.st_size);
        }
        if (!half && entries.size() <= kDiskFiles && total <= kDiskBytes)
            return;
        const std::size_t keep_files = half ? entries.size() / 2 : kDiskFiles * 3 / 4;
        const long long keep_bytes = half ? total / 2 : kDiskBytes * 3 / 4;
        std::sort(entries.begin(), entries.end(),
                  [](const Entry &a, const Entry &b) { return a.when < b.when; });
        std::size_t count = entries.size();
        std::size_t removed = 0;
        for (const Entry &entry : entries)
        {
            if (count <= keep_files && total <= keep_bytes)
                break;
            if (std::remove(entry.path.c_str()) == 0)
            {
                total -= entry.bytes;
                --count;
                ++removed;
            }
        }
        yt::log::line("thumbs: disk cache pruned %zu files, %lld KB left in %zu", removed,
                      total >> 10, count);
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
    const auto found = impl_->in_flight.find(url);
    if (found == impl_->in_flight.end())
    {
        impl_->in_flight.emplace(url, impl_->frame);
        impl_->work.push_back({url, impl_->frame});
        impl_->wake.notify_one();
    }
    else
        found->second = impl_->frame; // drawn again: it moves up the queue
    return 0;
}

void ThumbCache::set_disk_cache(const std::string &directory)
{
    if (!directory.empty() && !hui::save::ensure_directory(directory))
        return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->disk_dir = directory;
}

void ThumbCache::make_room()
{
    std::string directory;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        directory = impl_->disk_dir;
    }
    if (!directory.empty())
        Impl::prune_disk(directory, true);
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
            const auto found = impl_->in_flight.find(job->url);
            const std::uint64_t asked = found != impl_->in_flight.end() ? found->second : 0;
            if (now - asked > kStaleFrames)
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

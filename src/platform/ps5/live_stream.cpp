// youtube-tv - A live HLS broadcast read as one continuous byte stream.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/live_stream.hpp"

#include "yt/hls.hpp"
#include "yt/http.hpp"
#include "yt/log.hpp"

extern "C"
{
#include <libavutil/error.h>
}

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace hui::ps5
{

namespace
{

constexpr std::int64_t kClosed = std::numeric_limits<std::int64_t>::max();
// Unread bytes the worker may hold before it waits: several 1080p segments.
constexpr std::size_t kMaxBuffered = 24u << 20;
// Read bytes are dropped from the front in steps this big, not per read.
constexpr std::size_t kCompactStep = 4u << 20;
constexpr std::size_t kSegmentLimit = 32u << 20;
constexpr std::size_t kWorkerStack = 2u << 20; // console threads need an explicit stack

// A short sleep with the lock released (see range_stream.cpp: timed condition
// waits are not safe on the console).
void poll_pause(std::unique_lock<std::mutex> &lock, int ms = 5)
{
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    lock.lock();
}

} // namespace

struct LiveStream::State
{
    mutable std::mutex mutex;
    std::string url;
    std::string label;
    bool stop = false;
    bool ended = false;
    int error = 0;
    std::string error_text;
    // data holds the stream bytes [base, base + data.size()).
    std::vector<std::uint8_t> data;
    std::int64_t base = 0;
    std::vector<std::int64_t> positions; // per reader; kClosed once closed
    std::int64_t segments = 0;

    std::int64_t end() const
    {
        return base + static_cast<std::int64_t>(data.size());
    }

    std::int64_t slowest() const
    {
        std::int64_t lowest = kClosed;
        for (const std::int64_t position : positions)
            lowest = std::min(lowest, position);
        return lowest;
    }

    void compact()
    {
        const std::int64_t lowest = slowest();
        if (lowest == kClosed)
        {
            base = end();
            data.clear();
            return;
        }
        const std::size_t drop = static_cast<std::size_t>(lowest - base);
        if (drop >= kCompactStep || (drop > 0 && drop == data.size()))
        {
            data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(drop));
            base = lowest;
        }
    }
};

namespace
{

void *worker(void *argument)
{
    auto *handle = static_cast<std::shared_ptr<LiveStream::State> *>(argument);
    std::shared_ptr<LiveStream::State> owner = std::move(*handle);
    delete handle;
    LiveStream::State &state = *owner;

    std::string url;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        url = state.url;
    }
    auto fetch = [&state](const std::string &target)
    {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.stop)
            {
                yt::http::Response stopped;
                stopped.error = "stopped";
                return stopped;
            }
        }
        yt::http::Request request;
        request.url = target;
        request.timeout_s = 15;
        request.max_bytes = kSegmentLimit;
        return yt::http::perform(request);
    };
    auto emit = [&state](const std::string &bytes)
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        for (;;)
        {
            if (state.stop)
                return false;
            const std::int64_t lowest = state.slowest();
            if (lowest == kClosed)
                return false; // nobody reads any more
            if (static_cast<std::size_t>(state.end() - lowest) < kMaxBuffered)
                break;
            poll_pause(lock, 20);
        }
        state.data.insert(state.data.end(), bytes.begin(), bytes.end());
        ++state.segments;
        return true;
    };

    yt::hls::LiveFeed feed(url, fetch, 3);
    for (;;)
    {
        const double wait_s = feed.round(emit);
        if (wait_s < 0.0)
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.stop)
                break;
            if (feed.ended())
            {
                yt::log::line("live: %s broadcast ended after %lld segments", state.label.c_str(),
                              static_cast<long long>(state.segments));
                state.ended = true;
            }
            else
            {
                const long status = feed.error_status();
                state.error = status == 403   ? AVERROR_HTTP_FORBIDDEN
                              : status == 404 ? AVERROR_HTTP_NOT_FOUND
                                              : AVERROR(EIO);
                state.error_text = feed.error().empty() ? "live stream stopped" : feed.error();
                yt::log::line("live: %s %s", state.label.c_str(), state.error_text.c_str());
            }
            break;
        }
        // Sleep in short steps so a closed player is not kept waiting.
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(static_cast<int>(wait_s * 1000.0));
        std::unique_lock<std::mutex> lock(state.mutex);
        while (!state.stop && std::chrono::steady_clock::now() < until)
            poll_pause(lock, 50);
        if (state.stop)
            break;
    }
    return nullptr;
}

} // namespace

LiveStream::LiveStream(std::string playlist_url, const char *label, int readers,
                       Interrupt interrupt, void *opaque)
    : state_(std::make_shared<State>()), interrupt_(interrupt), opaque_(opaque)
{
    state_->url = std::move(playlist_url);
    state_->label = label != nullptr ? label : "live";
    reader_count_ = std::max(1, readers);
    state_->positions.assign(static_cast<std::size_t>(reader_count_), 0);
    readers_ = std::make_unique<Reader[]>(static_cast<std::size_t>(reader_count_));
    for (int i = 0; i < reader_count_; ++i)
    {
        readers_[i].owner = this;
        readers_[i].index = i;
    }
}

LiveStream::~LiveStream()
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stop = true;
}

int LiveStream::start(std::string *error)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0)
    {
        if (error != nullptr)
            *error = "no worker thread";
        return AVERROR(ENOMEM);
    }
    (void)pthread_attr_setstacksize(&attributes, kWorkerStack);
    auto *handle = new std::shared_ptr<State>(state_);
    pthread_t thread;
    const int created = pthread_create(&thread, &attributes, &worker, handle);
    pthread_attr_destroy(&attributes);
    if (created != 0)
    {
        delete handle;
        if (error != nullptr)
            *error = "no worker thread";
        return AVERROR(ENOMEM);
    }
    pthread_detach(thread);

    std::unique_lock<std::mutex> lock(state_->mutex);
    for (;;)
    {
        if (!state_->data.empty())
        {
            yt::log::line("live: %s first segment %zu bytes", state_->label.c_str(),
                          state_->data.size());
            return 0;
        }
        if (state_->error != 0)
        {
            if (error != nullptr)
                *error = state_->error_text;
            return state_->error;
        }
        if (state_->ended)
        {
            if (error != nullptr)
                *error = "the broadcast has ended";
            return AVERROR_EOF;
        }
        if (interrupt_ != nullptr && interrupt_(opaque_) != 0)
        {
            if (error != nullptr)
                *error = "interrupted";
            return AVERROR_EXIT;
        }
        poll_pause(lock);
    }
}

void *LiveStream::reader(int index)
{
    if (index < 0 || index >= reader_count_)
        return nullptr;
    return &readers_[index];
}

void LiveStream::close_reader(int index)
{
    if (index < 0 || index >= reader_count_)
        return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->positions[static_cast<std::size_t>(index)] = kClosed;
    state_->compact();
}

std::int64_t LiveStream::available(int index) const
{
    if (index < 0 || index >= reader_count_)
        return 0;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const std::int64_t position = state_->positions[static_cast<std::size_t>(index)];
    return position == kClosed ? 0 : state_->end() - position;
}

bool LiveStream::finished() const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->ended || state_->error != 0;
}

int LiveStream::read(int index, std::uint8_t *buffer, int size)
{
    if (size <= 0)
        return 0;
    State &state = *state_;
    std::unique_lock<std::mutex> lock(state.mutex);
    std::int64_t &position = state.positions[static_cast<std::size_t>(index)];
    if (position == kClosed)
        return AVERROR_EOF;
    for (;;)
    {
        const std::int64_t ready = state.end() - position;
        if (ready > 0)
        {
            const std::size_t take = static_cast<std::size_t>(
                std::min<std::int64_t>(ready, static_cast<std::int64_t>(size)));
            std::memcpy(buffer, state.data.data() + (position - state.base), take);
            position += static_cast<std::int64_t>(take);
            state.compact();
            return static_cast<int>(take);
        }
        if (state.error != 0)
            return state.error;
        if (state.ended)
            return AVERROR_EOF;
        if (interrupt_ != nullptr && interrupt_(opaque_) != 0)
            return AVERROR_EXIT;
        poll_pause(lock);
    }
}

int LiveStream::read_cb(void *opaque, std::uint8_t *buffer, int size)
{
    auto *reader = static_cast<Reader *>(opaque);
    return reader->owner->read(reader->index, buffer, size);
}

} // namespace hui::ps5

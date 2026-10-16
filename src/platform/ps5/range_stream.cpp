// youtube-tv - A googlevideo stream read in bounded HTTP range requests.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/range_stream.hpp"

#include "console_curl.h"
#include "yt/log.hpp"

extern "C"
{
#include <libavformat/avio.h>
#include <libavutil/error.h>
}

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace hui::ps5
{
namespace
{

constexpr std::size_t kRingBytes = 3u << 20; // per stream: the app heap is shared with the frames
constexpr std::int64_t kFirstChunk = 256 << 10; // a quick first answer
constexpr std::int64_t kChunk = 1 << 20;        // well under yt-dlp's 10 MiB cap
constexpr int kForbiddenRetries = 3;            // a 403 can be the ad wait ending late
constexpr int kTransientRetries = 6;
constexpr std::size_t kWorkerStack = 1u << 20; // console threads need an explicit stack
constexpr const char *kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/140.0.0.0 Safari/537.36";

// googlevideo URLs name the file length as clen=<bytes>.
std::int64_t clen_of(const std::string &url)
{
    for (const char *key : {"&clen=", "?clen="})
    {
        const std::size_t at = url.find(key);
        if (at != std::string::npos)
        {
            const long long value = std::strtoll(url.c_str() + at + std::strlen(key), nullptr, 10);
            if (value > 0)
                return value;
        }
    }
    return -1;
}

} // namespace

struct RangeStream::State
{
    std::mutex mutex;
    std::condition_variable changed;
    std::string url;
    std::string label;
    bool stop = false;
    std::uint64_t generation = 0;
    // The ring holds the file bytes [position, position + count).
    std::vector<std::uint8_t> ring = std::vector<std::uint8_t>(kRingBytes);
    std::size_t head = 0;
    std::size_t count = 0;
    std::int64_t position = 0;
    std::int64_t size = -1;
    // A terminal failure at the fetch offset, reported once the ring is read.
    int error = 0;
    std::string error_text;

    std::int64_t fetch_offset() const
    {
        return position + static_cast<std::int64_t>(count);
    }

    void append(const std::uint8_t *data, std::size_t length)
    {
        std::size_t tail = (head + count) % ring.size();
        while (length > 0)
        {
            const std::size_t run = std::min(length, ring.size() - tail);
            std::memcpy(ring.data() + tail, data, run);
            data += run;
            length -= run;
            count += run;
            tail = (tail + run) % ring.size();
        }
    }

    void drop(std::size_t length)
    {
        head = (head + length) % ring.size();
        count -= length;
        position += static_cast<std::int64_t>(length);
    }
};

namespace
{

// One range request in flight.
struct Transfer
{
    RangeStream::State *state = nullptr;
    CURL *easy = nullptr;
    std::uint64_t generation = 0;
    std::int64_t from = 0;
    std::size_t received = 0;
    long status = 0;
};

bool cancelled(const Transfer &transfer)
{
    return transfer.state->stop || transfer.generation != transfer.state->generation;
}

std::size_t on_write(char *data, std::size_t size, std::size_t items, void *opaque)
{
    auto &transfer = *static_cast<Transfer *>(opaque);
    const std::size_t length = size * items;
    if (transfer.status == 0)
        (void)curl_easy_getinfo(transfer.easy, CURLINFO_RESPONSE_CODE, &transfer.status);
    if (transfer.status >= 300 || (transfer.status != 206 && transfer.from > 0))
        return length; // an error page, or the whole file where a range was asked
    RangeStream::State &state = *transfer.state;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (cancelled(transfer))
        return 0;
    const std::size_t room = state.ring.size() - state.count;
    const std::size_t take = std::min(length, room);
    state.append(reinterpret_cast<const std::uint8_t *>(data), take);
    transfer.received += take;
    state.changed.notify_all();
    return take == length ? length : 0; // a full ring ends this request early
}

// A short sleep with the lock released. condition_variable::wait_for is not
// used: on the console its pthread_cond_timedwait answers with a code libc++
// takes for a failure, and without exceptions that aborts the app ("privileged
// instruction fault"). Plain wait() and sleeps are safe.
void poll_pause(std::unique_lock<std::mutex> &lock)
{
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    lock.lock();
}

// Header names compare without strncasecmp, one libc import fewer for a
// sandboxed title to resolve.
bool starts_with_nocase(const char *text, const char *prefix)
{
    for (; *prefix != '\0'; ++text, ++prefix)
    {
        char c = *text;
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != *prefix)
            return false;
    }
    return true;
}

// "Content-Range: bytes 0-524287/12345678" names the file length.
std::size_t on_header(char *data, std::size_t size, std::size_t items, void *opaque)
{
    auto &transfer = *static_cast<Transfer *>(opaque);
    const std::size_t length = size * items;
    static constexpr char kName[] = "content-range:";
    if (length > sizeof(kName) && starts_with_nocase(data, kName))
    {
        const std::string line(data, length);
        const std::size_t slash = line.find('/');
        if (slash != std::string::npos)
        {
            const long long total = std::strtoll(line.c_str() + slash + 1, nullptr, 10);
            if (total > 0)
            {
                std::lock_guard<std::mutex> lock(transfer.state->mutex);
                if (!cancelled(transfer))
                    transfer.state->size = total;
            }
        }
    }
    return length;
}

int on_progress(void *opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto &transfer = *static_cast<Transfer *>(opaque);
    std::lock_guard<std::mutex> lock(transfer.state->mutex);
    return cancelled(transfer) ? 1 : 0;
}

void fail(RangeStream::State &state, int error, std::string text)
{
    yt::log::line("range: %s %s at %lld", state.label.c_str(), text.c_str(),
                  static_cast<long long>(state.fetch_offset()));
    state.error = error;
    state.error_text = std::move(text);
    state.changed.notify_all();
}

void *worker(void *argument)
{
    auto *handle = static_cast<std::shared_ptr<RangeStream::State> *>(argument);
    std::shared_ptr<RangeStream::State> owner = std::move(*handle);
    delete handle;
    RangeStream::State &state = *owner;

    CURL *easy = curl_easy_init();
    if (easy == nullptr)
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        fail(state, AVERROR(ENOMEM), "curl_easy_init failed");
        return nullptr;
    }
    console_curl_setup(easy);
    (void)curl_easy_setopt(easy, CURLOPT_URL, state.url.c_str());
    (void)curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    (void)curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
    (void)curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
    (void)curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    (void)curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 5L); // a stuck transfer resumes sooner
    (void)curl_easy_setopt(easy, CURLOPT_USERAGENT, kUserAgent);
    (void)curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_write);
    (void)curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, on_header);
    (void)curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, on_progress);
    (void)curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);

    std::int64_t chunk = kFirstChunk;
    int forbidden = 0;
    int transient = 0;
    bool logged_first = false;
    bool failed = false;
    std::uint64_t last_generation = 0;

    for (;;)
    {
        Transfer transfer;
        transfer.state = &state;
        transfer.easy = easy;
        std::int64_t from = 0;
        std::int64_t to = 0;
        {
            std::unique_lock<std::mutex> lock(state.mutex);
            state.changed.wait(lock,
                               [&]
                               {
                                   if (state.stop)
                                       return true;
                                   if (state.error != 0)
                                       return false;
                                   const std::int64_t offset = state.fetch_offset();
                                   if (state.size >= 0 && offset >= state.size)
                                       return false;
                                   const std::size_t room = state.ring.size() - state.count;
                                   return static_cast<std::int64_t>(room) >=
                                          std::min<std::int64_t>(
                                              chunk, state.size >= 0 ? state.size - offset : chunk);
                               });
            if (state.stop)
                break;
            if (failed || state.generation != last_generation)
            {
                // A seek cleared the failure or moved the stream: a fresh
                // budget of retries for the new attempt.
                forbidden = 0;
                transient = 0;
                failed = false;
                last_generation = state.generation;
            }
            transfer.generation = state.generation;
            from = state.fetch_offset();
            std::int64_t want = chunk;
            if (state.size >= 0)
                want = std::min(want, state.size - from);
            to = from + want - 1;
            transfer.from = from;
        }

        char range[64];
        std::snprintf(range, sizeof(range), "%lld-%lld", static_cast<long long>(from),
                      static_cast<long long>(to));
        (void)curl_easy_setopt(easy, CURLOPT_RANGE, range);
        (void)curl_easy_setopt(easy, CURLOPT_WRITEDATA, &transfer);
        (void)curl_easy_setopt(easy, CURLOPT_HEADERDATA, &transfer);
        (void)curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &transfer);
        const CURLcode result = curl_easy_perform(easy);
        long status = 0;
        (void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);

        std::unique_lock<std::mutex> lock(state.mutex);
        if (state.stop)
            break;
        if (transfer.generation != state.generation)
            continue; // a seek moved the stream: start over at the new offset
        if (!logged_first)
        {
            yt::log::line("range: %s first request %s -> HTTP %ld, %zu bytes, size %lld",
                          state.label.c_str(), range, status, transfer.received,
                          static_cast<long long>(state.size));
            logged_first = true;
        }

        if (status == 403 || status == 401)
        {
            if (++forbidden > kForbiddenRetries)
            {
                fail(state, AVERROR_HTTP_FORBIDDEN,
                     "Server returned 403 Forbidden (access denied)");
                failed = true;
                continue;
            }
            yt::log::line("range: %s HTTP %ld for %s, retry %d", state.label.c_str(), status, range,
                          forbidden);
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            continue;
        }
        if (status == 416)
        {
            // Asked past the end: the file is as long as what was read.
            state.size = from;
            state.changed.notify_all();
            continue;
        }
        if (status == 404 || status == 410)
        {
            fail(state, status == 404 ? AVERROR_HTTP_NOT_FOUND : AVERROR_HTTP_OTHER_4XX,
                 "Server returned HTTP " + std::to_string(status));
            continue;
        }
        if (status == 200 && from > 0 && transfer.received == 0)
        {
            fail(state, AVERROR(ENOSYS), "server ignored the range request");
            continue;
        }
        if (status == 200 && state.size < 0 && result == CURLE_OK)
            state.size = from + static_cast<std::int64_t>(transfer.received);

        if (transfer.received > 0)
        {
            forbidden = 0;
            transient = 0;
            chunk = kChunk;
            continue; // a short or cut transfer resumes where it stopped
        }
        if (result != CURLE_OK || status >= 400)
        {
            if (++transient > kTransientRetries)
            {
                fail(state, status >= 500 ? AVERROR_HTTP_SERVER_ERROR : AVERROR(EIO),
                     status >= 400 ? "Server returned HTTP " + std::to_string(status)
                                   : std::string(curl_easy_strerror(result)));
                failed = true;
                continue;
            }
            yt::log::line("range: %s %s (HTTP %ld), retry %d", state.label.c_str(),
                          curl_easy_strerror(result), status, transient);
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(300 * transient));
        }
    }
    curl_easy_cleanup(easy);
    return nullptr;
}

} // namespace

RangeStream::RangeStream(std::string url, const char *label, Interrupt interrupt, void *opaque)
    : state_(std::make_shared<State>()), interrupt_(interrupt), opaque_(opaque)
{
    state_->url = std::move(url);
    state_->label = label != nullptr ? label : "stream";
    state_->size = clen_of(state_->url);
}

RangeStream::~RangeStream()
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stop = true;
    state_->changed.notify_all();
}

int RangeStream::start(std::string *error)
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
        if (state_->count > 0 || (state_->size >= 0 && state_->fetch_offset() >= state_->size))
            return 0;
        if (state_->error != 0)
        {
            if (error != nullptr)
                *error = state_->error_text;
            return state_->error;
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

int RangeStream::read(std::uint8_t *buffer, int size)
{
    if (size <= 0)
        return 0;
    State &state = *state_;
    std::unique_lock<std::mutex> lock(state.mutex);
    for (;;)
    {
        if (state.count > 0)
        {
            const std::size_t take = std::min(static_cast<std::size_t>(size), state.count);
            const std::size_t first = std::min(take, state.ring.size() - state.head);
            std::memcpy(buffer, state.ring.data() + state.head, first);
            if (take > first)
                std::memcpy(buffer + first, state.ring.data(), take - first);
            state.drop(take);
            state.changed.notify_all();
            return static_cast<int>(take);
        }
        if (state.error != 0)
            return state.error;
        if (state.size >= 0 && state.position >= state.size)
            return AVERROR_EOF;
        if (interrupt_ != nullptr && interrupt_(opaque_) != 0)
            return AVERROR_EXIT;
        poll_pause(lock);
    }
}

std::int64_t RangeStream::seek(std::int64_t offset, int whence)
{
    State &state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    if ((whence & AVSEEK_SIZE) != 0)
        return state.size >= 0 ? state.size : AVERROR(ENOSYS);
    whence &= ~AVSEEK_FORCE;
    std::int64_t target = offset;
    if (whence == SEEK_CUR)
        target = state.position + offset;
    else if (whence == SEEK_END)
    {
        if (state.size < 0)
            return AVERROR(ENOSYS);
        target = state.size + offset;
    }
    else if (whence != SEEK_SET)
        return AVERROR(EINVAL);
    if (target < 0)
        return AVERROR(EINVAL);

    if (target >= state.position && target <= state.fetch_offset())
    {
        state.drop(static_cast<std::size_t>(target - state.position)); // already fetched
        // A seek is how the player asks for the sound again after it stopped:
        // a failure recorded at the fetch offset must not outlive it, or a
        // stream that once gave up (three 403s, a dead connection) stays
        // silent for the rest of the video.
        state.error = 0;
        state.error_text.clear();
    }
    else
    {
        ++state.generation;
        state.head = 0;
        state.count = 0;
        state.position = target;
        state.error = 0;
        state.error_text.clear();
    }
    state.changed.notify_all();
    return target;
}

int RangeStream::read_cb(void *opaque, std::uint8_t *buffer, int size)
{
    return static_cast<RangeStream *>(opaque)->read(buffer, size);
}

std::int64_t RangeStream::seek_cb(void *opaque, std::int64_t offset, int whence)
{
    return static_cast<RangeStream *>(opaque)->seek(offset, whence);
}

} // namespace hui::ps5

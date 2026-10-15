// youtube-tv - A googlevideo stream read in bounded HTTP range requests.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ffmpeg's own http reader asks for "bytes=N-" and keeps one connection open
// for the whole file. googlevideo cuts such a transfer after a few MiB and
// answers the reconnect with 403, so a video played for a second and stopped.
// yt-dlp reads YouTube streams in chunks of at most 10 MiB for that reason;
// this does the same: a worker thread fetches consecutive ranges with libcurl
// into a ring buffer, and ffmpeg reads from it through a custom AVIOContext.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace hui::ps5
{

class RangeStream
{
  public:
    // `interrupt(opaque)` returning non-zero cuts a waiting read (AVERROR_EXIT),
    // like ffmpeg's AVIOInterruptCB.
    using Interrupt = int (*)(void *opaque);

    RangeStream(std::string url, const char *label, Interrupt interrupt, void *opaque);
    ~RangeStream();
    RangeStream(const RangeStream &) = delete;
    RangeStream &operator=(const RangeStream &) = delete;

    // Starts the worker and waits for the first bytes. Returns 0, or an
    // AVERROR (AVERROR_HTTP_FORBIDDEN for a 403) with `error` describing it.
    int start(std::string *error);

    // AVIOContext callbacks.
    int read(std::uint8_t *buffer, int size);
    std::int64_t seek(std::int64_t offset, int whence);

    static int read_cb(void *opaque, std::uint8_t *buffer, int size);
    static std::int64_t seek_cb(void *opaque, std::int64_t offset, int whence);

    struct State;

  private:
    std::shared_ptr<State> state_;
    Interrupt interrupt_;
    void *opaque_;
};

} // namespace hui::ps5

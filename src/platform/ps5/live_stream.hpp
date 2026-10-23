// youtube-tv - A live HLS broadcast read as one continuous byte stream.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace hui::ps5
{

// A worker thread follows one HLS media playlist (yt::hls::LiveFeed) and
// appends each segment's bytes; MPEG-TS segments in a row are one valid
// stream, so ffmpeg demuxes it as plain mpegts behind a custom AVIOContext.
// Several readers (the video and the audio demuxer of a muxed variant) read
// the same download, each at its own position.
class LiveStream
{
  public:
    using Interrupt = int (*)(void *opaque);

    LiveStream(std::string playlist_url, const char *label, int readers, Interrupt interrupt,
               void *opaque);
    ~LiveStream();
    LiveStream(const LiveStream &) = delete;
    LiveStream &operator=(const LiveStream &) = delete;

    // Starts the worker and waits for the first segment. Returns 0, or an
    // AVERROR with `error` describing it.
    int start(std::string *error);

    // The opaque for read_cb of reader `index` (0 .. readers-1).
    void *reader(int index);
    // A reader that will not read again (its demuxer never opened or closed)
    // stops holding the bytes it has not read.
    void close_reader(int index);
    // Bytes reader `index` can take without waiting, and whether the feed is
    // over (ended or failed): a read then never waits either.
    std::int64_t available(int index) const;
    bool finished() const;

    static int read_cb(void *opaque, std::uint8_t *buffer, int size);

    struct State;

  private:
    struct Reader
    {
        LiveStream *owner = nullptr;
        int index = 0;
    };
    int read(int index, std::uint8_t *buffer, int size);

    std::shared_ptr<State> state_;
    std::unique_ptr<Reader[]> readers_;
    int reader_count_ = 0;
    Interrupt interrupt_;
    void *opaque_;
};

} // namespace hui::ps5

// youtube-tv - Console player: PacBrew ffmpeg 7.x demux, decode and resample.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console build links static libavformat/libavcodec/libavutil/libswscale/
// libswresample built with OpenSSL, so the signed googlevideo.com URLs open
// directly over https. Video and audio each get their own AVFormatContext (the
// DASH streams are separate URLs); there is no synchronising container.
//
// Everything runs on the caller's frame thread: update() demuxes and decodes a
// bounded number of packets, converts whichever frames came out into a bounded
// queue, and poll_frame() presents the picture the audio clock has reached
// (holding while the queue is ahead, dropping pictures the clock has passed).
// Audio goes straight to the AudioSink, which owns the audio thread. No threads
// are created here; every stage logs one "ffmpeg: ..." line so yt.log shows
// which step failed or is too slow, and open()/update() failures are also
// readable through error().

#include "yt/log.hpp"
#include "yt/player.hpp"
#include "yt/tuning.hpp"

// Console-only: the Videodec2 hardware path and its reachability probe live
// under src/platform/ps5/, which the host builds do not compile.
#include "platform/ps5/live_stream.hpp"
#include "platform/ps5/range_stream.hpp"
#include "platform/ps5/videodec.hpp"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

extern "C"
{
    void hui_heap_stats(size_t *live_bytes, size_t *peak_bytes, size_t *blocks, size_t *failures);
    size_t hui_heap_capacity(void);
}

namespace yt
{

namespace
{

// Output format of the audio path: 48 kHz stereo float, the sink's contract.
constexpr int kOutputRate = 48000;
constexpr int kDecoderThreads = 4;
// Above 1080p the decoder gets more of the CPU: 1440p and 4K VP9 are several
// times the work of a 1080p picture.
constexpr int kDecoderThreadsLarge = 6;
// The largest picture handed to the screen (its own size); bigger ones are
// scaled down while they are converted.
constexpr int kMaxOutputWidth = 1920;
// Threads that turn each decoded picture into RGBA (and scale it down).
constexpr int kConvertThreads = 4;
constexpr int kMaxOutputHeight = 1080;
constexpr int kVideoPacketsPerUpdate = 4;
constexpr int kAudioPacketsPerUpdate = 8;
// Stop feeding the sink once it holds half a second; keeps latency low.
// One second: a slow frame (a heavy stream, a busy screen) must not empty
// the ring, or the sound gaps and the picture, which follows the sound's
// clock, stalls and then rushes. The ring holds 1.36 s.
constexpr int kAudioCushionFrames = kOutputRate;
// A decoder that will not write must not grow the carry buffer without end.
constexpr std::size_t kMaxCarryFrames = kOutputRate;
// Blocking network reads (http "timeout" / "rw_timeout", microseconds).
constexpr const char *kNetworkTimeout = "15000000";
// open() must never pin the worker on a dead host, and update() must never pin
// the frame thread on a stalled read: the network interrupt fires once the
// matching deadline passes. Microseconds.
constexpr std::int64_t kOpenTimeoutUs = 20'000'000;
constexpr std::int64_t kUpdateBudgetUs = 8'000;
// The update budget only stops a read that is producing nothing. A transfer
// that is making progress (a large keyframe packet from a slow host) must be
// allowed to finish, so the interrupt ignores the update deadline until it is
// exceeded by this margin: a healthy read finishes in a few milliseconds, and
// even a stalled one is cut here instead of freezing the frame thread for
// ffmpeg's full rw_timeout (kNetworkTimeout). A read cut this way is retried,
// but a stream whose reads are only ever cut is not slow, it is dead, and ends
// after kMaxStallReads of them.
constexpr std::int64_t kInterruptGraceUs = 1'000'000;
// Consecutive read/decode failures a stream survives before it is declared
// dead. A single EAGAIN, timeout or interrupted read never ends playback.
constexpr int kMaxConsecutiveErrors = 3;
// Interrupt/budget-cut reads a stream survives without producing a single
// packet before it is declared dead. Each one costs kInterruptGraceUs (the
// read blocks until the grace has passed) and the pumps count them, not
// kMaxConsecutiveErrors, because such a read is classified as retryable: a
// dead host would otherwise hold the frame thread at ~1 fps forever.
constexpr int kMaxStallReads = 4;
// How often one video's sound is restarted after it stopped before the end.
constexpr int kMaxAudioRecoveries = 12;
// Software AV1 on this CPU is far slower than real time, so a handful of
// packets proves nothing: during open() the decoder gets this many access units
// and this long before "no frames" is a verdict. The console has no AV1
// hardware path, so software is the only one there is.
constexpr int kAv1ProbeFrames = 24;
constexpr std::int64_t kAv1ProbeWindowUs = 2'000'000;
// Decoded pictures waiting for their presentation time. The queue bounds how
// far ahead of the clock the decoder may run (12 frames is ~0.4 s at 30 fps);
// when it is full the decoder holds, when it is empty the picture holds.
constexpr std::size_t kVideoQueueDepth = 12;
// The pool of decoded RGBA pictures is bounded by bytes, not by count: the
// queue plus the picture on screen and the one being converted is ~110 MiB at
// 1080p but ~460 MB at 2160p, which the settings allow. kVideoPoolBudgetBytes
// covers every live picture, so the effective queue depth is what it leaves
// after the two headroom slots, and it never drops below the floor.
constexpr std::size_t kVideoPoolBudgetBytes = 192u * 1024u * 1024u;
constexpr std::size_t kVideoPoolHeadroom = 2;
constexpr std::size_t kVideoMinQueueDepth = 4;
// The pool table itself is fixed: the queue never exceeds kVideoQueueDepth
// pictures and only the picture on screen and the one being converted live
// outside it, so kVideoQueueDepth + kVideoPoolHeadroom slots always suffice.
constexpr std::size_t kVideoPoolSlots = kVideoQueueDepth + kVideoPoolHeadroom;
// A video frame this far behind the audio clock is stale: converting and
// queueing it cannot catch up, so it is dropped.
constexpr double kLateFrameToleranceS = 0.10;
// Drift between the presented picture and the audio clock worth a log line.
constexpr double kDriftLogS = 0.20;
// How many stale frames in a row may be dropped before the picture is advanced
// again. A decoder that is simply slower than real time (software VP9/AV1 on
// this CPU) would otherwise settle on one still picture; the cap keeps the
// video moving and the "decode slow" line explains the stutter.
constexpr int kMaxLateDropRun = 10;
// Sentinel "nothing has been handed out / no pts" value.
constexpr double kNoPts = -1.0e18;

std::int64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string av_error(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(code, buffer, sizeof(buffer)) < 0)
    {
        return "error " + std::to_string(code);
    }
    return std::string(buffer);
}

const char *video_codec_label(AVCodecID id)
{
    switch (id)
    {
    case AV_CODEC_ID_AV1:
        return "AV1";
    case AV_CODEC_ID_VP9:
        return "VP9";
    case AV_CODEC_ID_H264:
        return "AVC";
    default:
        return "?";
    }
}

// The player only ever sees AV codec ids; the hardware path and
// videodec_supports() speak yt::Codec.
Codec codec_from_av_id(AVCodecID id)
{
    switch (id)
    {
    case AV_CODEC_ID_H264:
        return Codec::avc;
    case AV_CODEC_ID_VP9:
        return Codec::vp9;
    case AV_CODEC_ID_AV1:
        return Codec::av1;
    default:
        return Codec::unknown;
    }
}

// One demuxer/decoder pair. Video and audio each own one of these.
struct Demuxer
{
    // An http(s) URL is read through `range` (bounded range requests) behind
    // the custom `io`; both outlive `format`, which reads from them.
    std::unique_ptr<hui::ps5::RangeStream> range;
    // A live broadcast is read from a LiveStream instead (owned by the
    // player: the video and audio demuxer of a muxed variant share one).
    hui::ps5::LiveStream *live = nullptr;
    int live_reader = -1;
    AVIOContext *io = nullptr;
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVStream *stream = nullptr;
    int stream_index = -1;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    bool eof = false;     // av_read_frame reported the end
    bool drained = false; // the decoder has emitted everything it will
    // Consecutive read/decode failures; only repeated hard failures end the
    // stream, and the first one is logged with its exact av_strerror text.
    int consecutive_errors = 0;
    // Consecutive interrupt/budget-cut reads that produced no packet; any read
    // that returns data clears it. kMaxStallReads in a row are a dead stream,
    // not a slow one (see note_stalled_read).
    int stalled_reads = 0;
    std::chrono::steady_clock::time_point last_error_log{};
};

// The interrupt callback cuts a blocking read when the open deadline, the
// update budget or close() says so; ffmpeg reports that as AVERROR_EXIT. That
// is not a stream error: the packet is read again on the next update().
bool is_interrupt_error(int code)
{
    return code == AVERROR_EXIT;
}

// One read/decode failure. Logs the exact av_strerror text with its code (the
// first failure at once, then at most one line per second) and answers whether
// the stream may still be retried: kMaxConsecutiveErrors in a row end it.
bool note_stream_error(Demuxer &demuxer, const char *stream, const char *what, int code)
{
    ++demuxer.consecutive_errors;
    const auto now = std::chrono::steady_clock::now();
    if (demuxer.consecutive_errors == 1 || now - demuxer.last_error_log >= std::chrono::seconds(1))
    {
        demuxer.last_error_log = now;
        log::line("ffmpeg: %s %s failed: %s (%d), failure %d/%d", stream, what,
                  av_error(code).c_str(), code, demuxer.consecutive_errors, kMaxConsecutiveErrors);
    }
    return demuxer.consecutive_errors < kMaxConsecutiveErrors;
}

// One read the update budget or the interrupt cut before it produced a packet.
// Such a read is normally retried silently, but when it keeps happening the
// host is dead rather than slow: answers whether the stream may be retried,
// false once kMaxStallReads in a row were cut (roughly one second apart).
bool note_stalled_read(Demuxer &demuxer)
{
    return ++demuxer.stalled_reads < kMaxStallReads;
}

// A live (HLS) URL: YouTube's media playlists, or any .m3u8.
bool is_live_url(const std::string &url)
{
    return url.find("/hls_playlist/") != std::string::npos ||
           url.find(".m3u8") != std::string::npos;
}

// A live demuxer is only read while a whole segment's worth of bytes waits
// ahead of it: an MPEG-TS packet ends where the next one starts, so the
// demuxer finishing a segment's last packet needs the first bytes of the
// next segment, and must not wait for them on the frame thread. Each stream
// is measured by its own segments: a separate sound stream's are tiny
// (16 KB a second), a 1080p picture's can be megabytes. The cap keeps a
// high-bitrate stream from holding back more than a keyframe needs.
constexpr std::int64_t kLiveReadAheadCap = 1 << 20;

bool live_starved(const Demuxer &demuxer)
{
    if (demuxer.live == nullptr || demuxer.live->finished())
        return false;
    const std::int64_t wanted =
        std::min(kLiveReadAheadCap, std::max<std::int64_t>(demuxer.live->largest_segment(), 4096));
    return demuxer.live->available(demuxer.live_reader) < wanted;
}

// The hardware path's decode and convert, off the frame thread. A depth-one
// Videodec2 session blocks in Decode until its picture is done (~12 ms at 4K);
// on the frame thread that, plus the conversion, held a 4K60 stream at ~45 fps.
// The worker takes the split packets the frame thread feeds it, decodes each,
// and converts every picture (NV12 in the decoder's slot, valid until the next
// Decode) into an RGBA buffer of its own, at most 1920x1080. Up to kReady
// pictures wait for the frame thread, which swaps each buffer into its pool:
// no copy on either side.
class HardwareWorker
{
  public:
    static constexpr std::size_t kPackets = 8; // fed ahead of the decoder
    static constexpr std::size_t kReady = 3;   // converted ahead of the queue

    struct Picture
    {
        bool ok = true;     // false: a failed Decode, `error` says why
        bool shown = false; // false: a hidden VP9 frame, nothing to show
        std::string error;
        std::unique_ptr<std::uint8_t[]> bytes; // RGBA, width x height (or NV12)
        std::size_t size = 0;
        bool nv12 = false; // the planes as decoded: Y, then interleaved UV
        int width = 0;
        int height = 0;
        int source_height = 0;
        std::int64_t pts = AV_NOPTS_VALUE;
        // The first picture's geometry, for the log.
        int pitch = 0;
        std::size_t slot_size = 0;
    };

    HardwareWorker(hui::ps5::HardwareDecoder *decoder, int shown_height, int convert_threads,
                   bool nv12)
        : decoder_(decoder), shown_height_(shown_height), convert_threads_(convert_threads),
          nv12_(nv12)
    {
    }
    ~HardwareWorker()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        changed_.notify_all();
        if (decode_started_)
            pthread_join(decode_thread_, nullptr);
        if (convert_started_)
            pthread_join(convert_thread_, nullptr);
        for (AVPacket *&packet : packets_)
            av_packet_free(&packet);
        if (sws_ != nullptr)
            sws_freeContext(sws_);
        av_frame_free(&source_);
        av_frame_free(&target_);
    }
    HardwareWorker(const HardwareWorker &) = delete;
    HardwareWorker &operator=(const HardwareWorker &) = delete;

    bool start()
    {
        source_ = av_frame_alloc();
        target_ = av_frame_alloc();
        if (source_ == nullptr || target_ == nullptr)
            return false;
        pthread_attr_t attributes;
        if (pthread_attr_init(&attributes) != 0)
            return false;
        pthread_attr_setstacksize(&attributes, 1u << 20); // console threads need a stack
        // Decode and convert on threads of their own: the next picture decodes
        // into another slot while this one converts.
        // At depth 2 one slot is inside the decoder and each picture comes
        // out one call late, so two fewer may wait for the converter.
        slots_ = std::max(1, decoder_->frame_slots() - 2 * (decoder_->depth() - 1));
        decode_started_ =
            pthread_create(&decode_thread_, &attributes, &HardwareWorker::decode_main, this) == 0;
        convert_started_ =
            decode_started_ &&
            pthread_create(&convert_thread_, &attributes, &HardwareWorker::convert_main, this) == 0;
        pthread_attr_destroy(&attributes);
        return decode_started_ && convert_started_;
    }

    // Room for another packet.
    bool wants_packet() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return packets_.size() < kPackets;
    }
    // Takes the packet's data (the packet is left empty).
    void submit(AVPacket *packet)
    {
        AVPacket *copy = av_packet_alloc();
        if (copy == nullptr)
            return;
        av_packet_move_ref(copy, packet);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            packets_.push_back(copy);
        }
        changed_.notify_all();
    }
    // Decode and convert time since the last call, for the 5-second line.
    struct Timing
    {
        int decodes = 0;
        std::int64_t decode_us = 0;
        std::int64_t decode_max_us = 0;
        int converts = 0;
        std::int64_t convert_us = 0;
        std::int64_t convert_max_us = 0;
        int waits_for_room = 0; // a picture ready but the ready list full
        int no_picture = 0;     // decodes that returned nothing (hidden frame, or held back)
        int several = 0;        // decodes reporting more than one picture
    };
    Timing take_timing()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Timing out = timing_;
        timing_ = Timing{};
        return out;
    }
    bool take(Picture *out)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ready_.empty())
                return false;
            *out = std::move(ready_.front());
            ready_.erase(ready_.begin());
        }
        changed_.notify_all(); // room for the next conversion
        return true;
    }
    // A buffer the pool gave back in a swap, for the next conversion.
    void recycle(std::unique_ptr<std::uint8_t[]> bytes, std::size_t size)
    {
        if (bytes == nullptr || size == 0)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (spare_.size() < kReady + 1)
            spare_.push_back(Spare{std::move(bytes), size});
    }
    // Nothing fed, decoding, converting or waiting: the stream's end can be
    // declared.
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return packets_.empty() && !decoding_ && decoded_.empty() && ready_.empty();
    }
    // Drops what is queued and waits for the pictures in flight (dropped too):
    // before a reset, a seek or the end of the session.
    void settle()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (AVPacket *&packet : packets_)
            av_packet_free(&packet);
        packets_.clear();
        changed_.wait(lock, [this] { return !decoding_ && !converting_; });
        decoded_.clear();
        shown_pts_.clear();
        for (Picture &picture : ready_)
            if (picture.bytes != nullptr && spare_.size() < kReady + 1)
                spare_.push_back(Spare{std::move(picture.bytes), picture.size});
        ready_.clear();
        changed_.notify_all();
    }

  private:
    struct Spare
    {
        std::unique_ptr<std::uint8_t[]> bytes;
        std::size_t size = 0;
    };

    // A Decode's outcome, waiting for the converter. Its picture stays in the
    // decoder's slot until slots_ more Decode calls have run.
    struct Decoded
    {
        bool ok = true;
        std::string error;
        hui::ps5::DecodedPicture picture;
        std::int64_t pts = AV_NOPTS_VALUE;
    };

    // The last decodes, for the log when a picture comes out of order.
    struct Event
    {
        std::int64_t packet_pts = 0;
        bool shows = true;
        int bytes = 0;
        bool ok = true;
        std::uintptr_t address = 0;
        std::int64_t tag = 0;
        std::int64_t given = 0;
        int waiting = 0;
    };
    static constexpr int kEvents = 10;
    Event events_[kEvents];
    int event_next_ = 0;

  public:
    void log_recent(double time_base)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < kEvents; ++i)
        {
            const Event &e = events_[(event_next_ + i) % kEvents];
            log::line("ffmpeg:   decode unit %.3f %s %d B -> %s picture %llx tag %.3f, given "
                      "%.3f (%d waiting)",
                      static_cast<double>(e.packet_pts) * time_base, e.shows ? "shows" : "hidden",
                      e.bytes, e.ok ? "ok" : "FAILED",
                      static_cast<unsigned long long>(e.address & 0xffffffffull),
                      static_cast<double>(e.tag) * time_base,
                      static_cast<double>(e.given) * time_base, e.waiting);
        }
    }

  private:
    // A VP9 frame's uncompressed header: shown when show_existing_frame or
    // show_frame is set (hidden frames are references only).
    static bool vp9_shows_picture(const std::uint8_t *data, int size)
    {
        if (data == nullptr || size < 1)
            return true;
        const std::uint8_t b = data[0];
        if ((b >> 6) != 2)
            return true; // not a frame header we know: count it
        const int profile = ((b >> 5) & 1) | (((b >> 4) & 1) << 1);
        int bit = profile == 3 ? 2 : 3; // a reserved zero bit after profile 3
        if ((b >> bit) & 1)
            return true; // show_existing_frame
        bit -= 2;        // past frame_type
        if (bit >= 0)
            return ((b >> bit) & 1) != 0;
        return size < 2 || (data[1] & 0x80) != 0;
    }

    static void *decode_main(void *argument)
    {
        static_cast<HardwareWorker *>(argument)->decode_loop();
        return nullptr;
    }
    static void *convert_main(void *argument)
    {
        static_cast<HardwareWorker *>(argument)->convert_loop();
        return nullptr;
    }

    void decode_loop()
    {
        for (;;)
        {
            AVPacket *packet = nullptr;
            std::int64_t packet_pts = AV_NOPTS_VALUE;
            int packet_bytes = 0;
            bool packet_shows = true;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // A packet, and a slot whose picture has been converted: the
                // outcomes not yet converted hold the other slots.
                changed_.wait(lock,
                              [this] {
                                  return quit_ ||
                                         (!packets_.empty() &&
                                          decoded_.size() < static_cast<std::size_t>(slots_));
                              });
                if (quit_)
                    return;
                packet = packets_.front();
                packets_.erase(packets_.begin());
                decoding_ = true;
                // Pictures leave the decoder in the order they are shown,
                // which VP9 makes the order of the units that show one.
                packet_pts = packet->pts;
                packet_bytes = packet->size;
                packet_shows = vp9_shows_picture(packet->data, packet->size);
                if (packet_shows)
                {
                    shown_pts_.push_back(packet->pts);
                    if (shown_pts_.size() > 16) // out of step: start over
                        shown_pts_.pop_front();
                }
            }
            Decoded decoded;
            decoded.pts = packet->pts;
            const auto start = std::chrono::steady_clock::now();
            decoded.ok = decoder_->decode(packet->data, static_cast<std::size_t>(packet->size),
                                          packet->pts, &decoded.picture, &decoded.error);
            // The picture's time: the oldest unit that shows one and has
            // not had its picture yet (at depth 2, often the one before; a
            // "show existing frame" unit shows a picture decoded earlier).
            {
                std::lock_guard<std::mutex> lock(mutex_);
                Event event;
                event.packet_pts = packet_pts;
                event.shows = packet_shows;
                event.bytes = packet_bytes;
                event.ok = decoded.ok;
                event.address = reinterpret_cast<std::uintptr_t>(decoded.picture.address);
                event.tag = decoded.picture.tag;
                event.waiting = static_cast<int>(shown_pts_.size());
                if (decoded.ok && decoded.picture.address != nullptr)
                {
                    if (!shown_pts_.empty())
                    {
                        decoded.pts = shown_pts_.front();
                        shown_pts_.pop_front();
                    }
                    else
                        decoded.pts = decoded.picture.tag;
                }
                event.given = decoded.pts;
                events_[event_next_] = event;
                event_next_ = (event_next_ + 1) % kEvents;
            }
            const std::int64_t took = std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count();
            av_packet_free(&packet);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++timing_.decodes;
                if (decoded.ok && decoded.picture.address == nullptr)
                    ++timing_.no_picture;
                if (decoded.picture.count > 1)
                    ++timing_.several;
                timing_.decode_us += took;
                timing_.decode_max_us = std::max(timing_.decode_max_us, took);
                decoding_ = false;
                decoded_.push_back(std::move(decoded));
            }
            changed_.notify_all();
        }
    }

    void convert_loop()
    {
        for (;;)
        {
            Decoded decoded;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock,
                              [this]
                              {
                                  if (!quit_ && !decoded_.empty() && ready_.size() >= kReady)
                                      ++timing_.waits_for_room; // counted once per wake, roughly
                                  return quit_ || (!decoded_.empty() && ready_.size() < kReady);
                              });
                if (quit_)
                    return;
                // A copy: the entry stays (and keeps its slot) until converted.
                decoded = decoded_.front();
                converting_ = true;
            }
            Picture picture;
            picture.ok = decoded.ok;
            picture.error = decoded.error;
            picture.pts = decoded.pts;
            const bool has_picture = decoded.ok && decoded.picture.address != nullptr &&
                                     decoded.picture.width > 0 && decoded.picture.height > 0;
            const auto start = std::chrono::steady_clock::now();
            if (has_picture)
                convert(decoded.picture, &picture);
            const std::int64_t took = std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (has_picture)
                {
                    ++timing_.converts;
                    timing_.convert_us += took;
                    timing_.convert_max_us = std::max(timing_.convert_max_us, took);
                }
                converting_ = false;
                // settle() may have emptied the list meanwhile: then this one
                // belongs to the old position and is dropped.
                if (!decoded_.empty())
                {
                    decoded_.pop_front();
                    // A hidden frame (no picture) needs no place in the ready
                    // list, but a failure does: the frame thread counts them.
                    if (!picture.ok || picture.shown)
                        ready_.push_back(std::move(picture));
                }
            }
            changed_.notify_all();
        }
    }

    // NV12 in the decoder's slot -> RGBA in a buffer of our own, scaled down to
    // fit 1920x1080. The chroma plane starts right after the reported height,
    // as the Nuvio and Stremio PS5 ports read it (planes[1] = data + pitch x
    // height); rows past the stream's own height (1088 for 1080p) are padding.
    void convert(const hui::ps5::DecodedPicture &decoded, Picture *picture)
    {
        const int pitch = decoded.pitch > 0 ? decoded.pitch : decoded.width;
        const int source_width = decoded.width;
        const int source_height =
            shown_height_ > 0 ? std::min(decoded.height, shown_height_) : decoded.height;
        if (nv12_)
        {
            convert_nv12(decoded, pitch, source_width & ~1, source_height & ~1, picture);
            return;
        }
        int width = source_width;
        int height = source_height;
        if (width > kMaxOutputWidth || height > kMaxOutputHeight)
        {
            const double scale = std::min(static_cast<double>(kMaxOutputWidth) / width,
                                          static_cast<double>(kMaxOutputHeight) / height);
            width = std::max(2, static_cast<int>(width * scale) & ~1);
            height = std::max(2, static_cast<int>(height * scale) & ~1);
        }
        const std::size_t size =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;

        std::unique_ptr<std::uint8_t[]> bytes;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = spare_.begin(); it != spare_.end(); ++it)
            {
                if (it->size == size)
                {
                    bytes = std::move(it->bytes);
                    spare_.erase(it);
                    break;
                }
            }
        }
        if (bytes == nullptr)
            bytes.reset(new (std::nothrow) std::uint8_t[size]);
        if (bytes == nullptr)
            return; // out of memory: this picture is dropped (shown stays false)

        if (sws_ == nullptr || sws_src_w_ != source_width || sws_src_h_ != source_height ||
            sws_dst_w_ != width || sws_dst_h_ != height)
        {
            if (sws_ != nullptr)
                sws_freeContext(sws_);
            sws_ = sws_alloc_context();
            if (sws_ != nullptr)
            {
                av_opt_set_int(sws_, "srcw", source_width, 0);
                av_opt_set_int(sws_, "srch", source_height, 0);
                av_opt_set_int(sws_, "src_format", AV_PIX_FMT_NV12, 0);
                av_opt_set_int(sws_, "dstw", width, 0);
                av_opt_set_int(sws_, "dsth", height, 0);
                av_opt_set_int(sws_, "dst_format", AV_PIX_FMT_RGBA, 0);
                av_opt_set_int(sws_, "sws_flags", SWS_BILINEAR, 0);
                av_opt_set_int(sws_, "threads", convert_threads_, 0);
                if (sws_init_context(sws_, nullptr, nullptr) < 0)
                {
                    sws_freeContext(sws_);
                    sws_ = nullptr;
                }
            }
            if (sws_ == nullptr)
                return;
            sws_src_w_ = source_width;
            sws_src_h_ = source_height;
            sws_dst_w_ = width;
            sws_dst_h_ = height;
        }

        auto *base = static_cast<std::uint8_t *>(const_cast<void *>(decoded.address));
        const std::size_t slot = decoded.size > 0
                                     ? decoded.size
                                     : static_cast<std::size_t>(pitch) * decoded.height * 3 / 2;
        av_frame_unref(source_);
        source_->buf[0] = av_buffer_create(base, slot, [](void *, std::uint8_t *) {}, nullptr, 0);
        av_frame_unref(target_);
        target_->buf[0] =
            av_buffer_create(bytes.get(), size, [](void *, std::uint8_t *) {}, nullptr, 0);
        if (source_->buf[0] == nullptr || target_->buf[0] == nullptr)
            return;
        source_->format = AV_PIX_FMT_NV12;
        source_->width = source_width;
        source_->height = source_height;
        source_->data[0] = base;
        source_->linesize[0] = pitch;
        source_->data[1] = base + static_cast<std::size_t>(pitch) * decoded.height;
        source_->linesize[1] = pitch;
        target_->format = AV_PIX_FMT_RGBA;
        target_->width = width;
        target_->height = height;
        target_->data[0] = bytes.get();
        target_->linesize[0] = width * 4;
        const bool converted = sws_scale_frame(sws_, target_, source_) >= 0;
        av_frame_unref(source_);
        av_frame_unref(target_);
        if (!converted)
            return;
        picture->bytes = std::move(bytes);
        picture->size = size;
        picture->width = width;
        picture->height = height;
        picture->source_height = source_height;
        picture->pitch = pitch;
        picture->slot_size = decoded.size;
        picture->shown = true;
    }

    // The decoder's planes copied tight (no pitch padding, no rows past the
    // picture) for the GPU to convert and scale.
    void convert_nv12(const hui::ps5::DecodedPicture &decoded, int pitch, int width, int height,
                      Picture *picture)
    {
        if (width <= 0 || height <= 0)
            return;
        const std::size_t luma = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        const std::size_t size = luma + luma / 2;
        std::unique_ptr<std::uint8_t[]> bytes = take_spare(size);
        if (bytes == nullptr)
            return;
        const auto *base = static_cast<const std::uint8_t *>(decoded.address);
        const std::size_t row = static_cast<std::size_t>(pitch);
        std::uint8_t *out = bytes.get();
        if (row == static_cast<std::size_t>(width))
            std::memcpy(out, base, luma);
        else
            for (int y = 0; y < height; ++y)
                std::memcpy(out + static_cast<std::size_t>(y) * width, base + row * y,
                            static_cast<std::size_t>(width));
        const std::uint8_t *chroma = base + row * static_cast<std::size_t>(decoded.height);
        out += luma;
        for (int y = 0; y < height / 2; ++y)
            std::memcpy(out + static_cast<std::size_t>(y) * width, chroma + row * y,
                        static_cast<std::size_t>(width));
        picture->bytes = std::move(bytes);
        picture->size = size;
        picture->width = width;
        picture->height = height;
        picture->source_height = height;
        picture->pitch = pitch;
        picture->slot_size = decoded.size;
        picture->nv12 = true;
        picture->shown = true;
    }

    std::unique_ptr<std::uint8_t[]> take_spare(std::size_t size)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = spare_.begin(); it != spare_.end(); ++it)
            {
                if (it->size == size)
                {
                    std::unique_ptr<std::uint8_t[]> bytes = std::move(it->bytes);
                    spare_.erase(it);
                    return bytes;
                }
            }
        }
        return std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[size]);
    }

    hui::ps5::HardwareDecoder *decoder_;
    const int shown_height_;
    const int convert_threads_;
    const bool nv12_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    pthread_t decode_thread_{};
    pthread_t convert_thread_{};
    bool decode_started_ = false;
    bool convert_started_ = false;
    int slots_ = 1;
    bool quit_ = false;
    bool decoding_ = false;
    bool converting_ = false;
    std::vector<AVPacket *> packets_;
    std::deque<Decoded> decoded_;
    std::deque<std::int64_t> shown_pts_; // units that show a picture, not yet out
    std::vector<Picture> ready_;
    std::vector<Spare> spare_;
    Timing timing_;
    // Used by the worker only.
    SwsContext *sws_ = nullptr;
    int sws_src_w_ = 0;
    int sws_src_h_ = 0;
    int sws_dst_w_ = 0;
    int sws_dst_h_ = 0;
    AVFrame *source_ = nullptr;
    AVFrame *target_ = nullptr;
};

class FfmpegPlayer final : public Player
{
  public:
    FfmpegPlayer() = default;
    FfmpegPlayer(const FfmpegPlayer &) = delete;
    FfmpegPlayer &operator=(const FfmpegPlayer &) = delete;
    ~FfmpegPlayer() override
    {
        release();
    }

    bool open(const std::string &video_url, const std::string &audio_url, AudioSink *sink) override;
    void close() override;
    void set_decode_mode(DecodeMode mode) override
    {
        // Read by the next open(); changing it mid-playback does nothing until
        // open() is called again.
        decode_mode_ = mode;
    }

    bool running() const override
    {
        return running_;
    }
    double duration_s() const override;
    std::string quality_label() const override
    {
        return quality_;
    }

    void update() override;
    VideoFrameView poll_frame() override;

    double clock_s() const override
    {
        return clock_;
    }
    void seek(double seconds) override;
    void set_paused(bool paused) override;
    bool paused() const override
    {
        return paused_;
    }
    void set_speed(double speed) override;
    double speed() const override
    {
        return speed_;
    }
    double buffered_s() const override;
    std::uint64_t dropped_frames() const override
    {
        return video_frames_dropped_.load();
    }
    int source_height() const override
    {
        return source_height_.load();
    }
    std::string stats() const override
    {
        char text[512];
        std::snprintf(text, sizeof(text),
                      "decode  %s %s, %d threads, deblocking %s\n"
                      "picture %dp -> %dx%d RGBA, convert %s (%d threads)\n"
                      "queue   %zu of %zu pictures, %.1f s buffered, %llu dropped late",
                      quality_.c_str(),
                      hardware_active_ ? "HARDWARE (Videodec2, experimental)" : "software",
                      decoder_threads_, deblock_skipped_ ? "off" : "on", source_height_.load(),
                      video_width_, video_height_, sws_threaded_ ? "threaded" : "single",
                      convert_threads_, video_queue_.size(), video_queue_depth_, buffered_s(),
                      static_cast<unsigned long long>(video_frames_dropped_.load()));
        return text;
    }
    bool ended() const override
    {
        return ended_;
    }
    const std::string &error() const override
    {
        return error_;
    }

  private:
    static int interrupt_cb(void *opaque);
    static void release_io(Demuxer &demuxer);

    bool open_demuxer(Demuxer &demuxer, const std::string &url, bool video);
    bool init_audio_resampler();
    std::string make_quality_label() const;

    void pump_video(int budget);
    // The experimental hardware path (Decoding: Hardware, VP9): packets go
    // through vp9_superframe_split to Videodec2, each decoded picture is read
    // back from the CPU-readable frame slot and converted like a software one.
    void pump_video_hardware(int budget);
    bool start_hardware();
    void restart_hardware();
    bool wants_hardware() const;
    void queue_hardware_picture(HardwareWorker::Picture &picture);
    void fall_back_to_software(const std::string &why);
    void pump_audio(int budget);
    void emit_video(AVFrame *frame);
    void emit_audio(AVFrame *frame);
    double frame_pts(const AVFrame *frame) const;

    // Presentation gate driven by the audio clock (the wall clock when there is
    // no audio): drops queued pictures the clock has passed, presents the one
    // that is due and holds otherwise. Returns true when a new picture went on
    // screen during this call.
    bool present_due_picture();
    // RGBA buffers are reused through a pool, never reallocated per frame.
    int acquire_video_buffer(std::size_t size);
    void release_video_buffer(int index);
    // Logs "a/v drift" at most once a second when the presented picture and the
    // audio clock are more than kDriftLogS apart.
    void note_drift();

    void push_audio(const float *stereo, std::size_t frames);
    bool flush_carry();
    void update_clock();

    // Decodes up to `packets` of the video stream (or `window_us` of wall time)
    // during open() to prove the decoder really produces pictures; stops at the
    // first frame it emits.
    void probe_video_frames(int packets, std::int64_t window_us);
    // Frame interval of the video stream in microseconds, 33 ms when unknown.
    std::int64_t frame_interval_us() const;
    // Logs "decode slow" at most once a second while the decoder cannot keep
    // up with the stream's frame interval.
    void note_decode_slow(std::int64_t spent_us);
    // Logs how many late frames were dropped, at most once a second.
    void note_video_drop();
    // A silent audio path must name its reason in yt.log: the first line at
    // once, then at most one per second, and only when the reason changes.
    void note_audio_silence(const char *reason);
    // The buffer pool's transitions are logged the first time and then at most
    // once a second each: yt.log's last lines must name the pool step a crash
    // happened in without logging every frame.
    void note_buffer_acquire(int index, std::size_t size);
    void note_buffer_release(int index, std::size_t size);
    // One line a second when a picture was dropped for lack of memory.
    void note_no_memory(int width, int height, std::size_t size);
    // An allocation failure, not a resolution, sets the real picture budget:
    // the queue target drops to the slots already in use (at least one).
    void lower_queue_depth();
    // Frees every pool slot and forgets the free list.
    void clear_video_pool();

    void release_demuxer(Demuxer &demuxer);
    void release();

    // True once the current update() budget has passed (the network interrupt
    // stops a blocking read and the pumps bail instead of mistaking it for EOF).
    bool out_of_time() const;

    std::atomic<bool> abort_{false};
    // Steady-clock microseconds the network interrupt consults; 0 = no limit.
    // open_deadline_us_ covers the whole open() sequence, update_deadline_us_
    // the single update() call so the frame thread always gets the frame back.
    std::atomic<std::int64_t> open_deadline_us_{0};
    std::atomic<std::int64_t> update_deadline_us_{0};
    std::chrono::steady_clock::time_point last_budget_log_{};
    Demuxer video_;
    Demuxer audio_;
    AudioSink *sink_ = nullptr;

    bool running_ = false;
    bool paused_ = false;
    bool ended_ = false;
    bool audio_active_ = false;
    // Priming: after an open or a seek the sound is held (and with it the
    // clock) until both paths have a cushion, so the first seconds do not
    // stutter while the decoders and the range readers warm up.
    bool priming_ = false;
    std::chrono::steady_clock::time_point priming_since_{};
    void start_priming()
    {
        if (sink_ == nullptr || !audio_active_)
            return;
        priming_ = true;
        priming_since_ = std::chrono::steady_clock::now();
        sink_->set_paused(true);
    }
    std::string error_;
    std::string quality_;

    SwsContext *sws_ = nullptr;
    // The converter's shape (sws_ is rebuilt when it changes) and whether it
    // runs on several threads (sws_scale_frame) or the single-threaded way.
    int sws_src_w_ = 0;
    int sws_src_h_ = 0;
    int sws_src_format_ = -1;
    int sws_dst_w_ = 0;
    int sws_dst_h_ = 0;
    bool sws_threaded_ = false;
    AVFrame *sws_dst_ = nullptr;
    SwrContext *swr_ = nullptr;
    // Playback speed and the atempo graph that stretches the 48 kHz output
    // to it (none at 1x).
    double speed_ = 1.0;
    // Times this video's sound was brought back after it stopped early.
    int audio_recoveries_ = 0;
    AVFilterGraph *tempo_graph_ = nullptr;
    AVFilterContext *tempo_src_ = nullptr;
    AVFilterContext *tempo_sink_ = nullptr;
    std::int64_t tempo_pts_ = 0;
    bool build_tempo();
    void free_tempo();
    void push_tempo(const float *stereo, std::size_t frames);

    // Decoded pictures waiting for their presentation time. The RGBA buffers
    // live in a pool (a 1080p frame is ~8 MiB, so they are reused, never
    // reallocated per frame) and the queue pairs a buffer with its pts; the
    // presentation gate scans it against the clock. exposed_buffer_ is the pool
    // slot currently on screen, valid until the next picture is presented.
    //
    // The whole frame path is allocation-safe in this -fno-exceptions build:
    // the slot table and the queue are fixed-size and the picture bytes come
    // from nothrow new, so a heap that cannot produce a width*height*4 slot
    // drops the picture (see emit_video) instead of calling std::terminate.
    struct QueuedPicture
    {
        int buffer = -1;
        double pts = 0.0;
    };
    // Fixed-capacity FIFO of decoded pictures; never allocates.
    struct PictureQueue
    {
        std::array<QueuedPicture, kVideoQueueDepth> slots{};
        std::size_t head = 0;
        std::size_t count = 0;

        bool empty() const
        {
            return count == 0;
        }
        std::size_t size() const
        {
            return count;
        }
        void clear()
        {
            head = 0;
            count = 0;
        }
        QueuedPicture &front()
        {
            return slots[head];
        }
        void pop()
        {
            head = (head + 1) % kVideoQueueDepth;
            --count;
        }
        bool push(const QueuedPicture &picture)
        {
            if (count >= kVideoQueueDepth)
            {
                return false;
            }
            slots[(head + count) % kVideoQueueDepth] = picture;
            ++count;
            return true;
        }
    };
    // One reusable RGBA picture; the bytes come from nothrow new.
    struct VideoBuffer
    {
        std::unique_ptr<std::uint8_t[]> bytes;
        std::size_t size = 0;
    };
    std::array<VideoBuffer, kVideoPoolSlots> video_buffers_{};
    std::size_t video_buffer_count_ = 0; // slots ever handed out
    std::array<int, kVideoPoolSlots> video_free_buffers_{};
    std::size_t video_free_count_ = 0;
    PictureQueue video_queue_;
    // Effective queue bound, recomputed from the pool budget when the
    // resolution is known (see emit_video); kVideoQueueDepth until then.
    std::size_t video_queue_depth_ = kVideoQueueDepth;
    int exposed_buffer_ = -1;
    double exposed_pts_ = kNoPts;
    bool picture_presented_ = false;
    double last_video_pts_ = 0.0;
    int video_width_ = 0;
    bool video_nv12_ = false; // the pool holds NV12 pictures (GPU conversion)
    int video_height_ = 0;
    bool frame_seen_ = false;

    // Resampler output and the sink-overflow carry, both grown with nothrow
    // new for the same reason as the picture slots: out of memory drops that
    // audio, it does not abort the app.
    std::unique_ptr<std::uint8_t[]> audio_scratch_;
    std::size_t audio_scratch_size_ = 0;
    std::unique_ptr<float[]> carry_;
    std::size_t carry_capacity_ = 0; // floats
    std::size_t carry_frames_ = 0;
    std::uint64_t audio_frames_written_ = 0;
    std::uint64_t audio_frames_decoded_ = 0;
    double audio_base_s_ = 0.0;
    // Live (HLS) playback: the broadcast's own timestamps start anywhere, so
    // the first video timestamp is the player's zero.
    bool live_ = false;
    double pts_origin_ = 0.0;
    std::unique_ptr<hui::ps5::LiveStream> live_video_;
    std::unique_ptr<hui::ps5::LiveStream> live_audio_;
    // After seek(): the demuxers restart at the keyframe before the target, so
    // audio and video decoded before it are discarded until each reaches it
    // (negative: no seek pending for that stream).
    double audio_seek_target_ = -1.0;
    double video_seek_target_ = -1.0;
    int video_seek_skipped_ = 0; // frames dropped on the way to the target
    bool audio_first_write_logged_ = false;
    std::string audio_silence_reason_;
    std::chrono::steady_clock::time_point last_audio_log_{};

    double video_interval_s_ = 1.0 / 30.0;
    std::atomic<std::uint64_t> video_frames_dropped_{0};
    std::atomic<int> source_height_{0};
    int decoder_threads_ = 0;
    int convert_threads_ = kConvertThreads;
    bool deblock_skipped_ = false;
    double av_offset_s_ = 0.0;
    int late_drop_run_ = 0;
    // The decode-slow and drop lines are throttled separately: they describe
    // different things and must be able to appear in the same second.
    std::chrono::steady_clock::time_point last_decode_log_{};
    std::chrono::steady_clock::time_point last_drop_log_{};
    std::chrono::steady_clock::time_point last_drift_log_{};
    std::chrono::steady_clock::time_point last_acquire_log_{};
    std::chrono::steady_clock::time_point last_release_log_{};
    std::chrono::steady_clock::time_point last_nomem_log_{};

    // Software by default; set_decode_mode() overrides it before open(). The
    // session is created only when a mode other than software asked for it, so
    // software playback never constructs or touches the hardware decoder.
    DecodeMode decode_mode_ = DecodeMode::auto_detect;
    std::unique_ptr<hui::ps5::HardwareDecoder> hardware_;
    // Runs the decoder's Decode calls; destroyed before hardware_.
    std::unique_ptr<HardwareWorker> hardware_worker_;
    bool hardware_active_ = false; // pictures come from Videodec2
    AVBSFContext *hardware_bsf_ = nullptr;
    AVPacket *hardware_packet_ = nullptr;
    AVFrame *hardware_frame_ = nullptr; // wraps the decoder's picture for the converter
    int hardware_failures_ = 0;
    std::uint64_t hardware_pictures_ = 0;
    int hardware_restarts_ = 0;      // decoder restarts after failures, this video
    int hardware_order_dumps_ = 0;   // out-of-order reports logged, this video
    int hardware_missing_ = 0;       // frames skipped between pictures (diagnostics)
    int hardware_out_of_order_ = 0;  // pictures earlier than the one before
    int hardware_queue_empty_ = 0;   // presents that found no picture queued (5-second line)
    bool hardware_wait_key_ = false; // after a seek: packets until a keyframe are skipped
    int hardware_skipped_to_key_ = 0;
    std::chrono::steady_clock::time_point hardware_log_{};

    double clock_ = 0.0;
    bool wall_anchor_ = false;
    std::chrono::steady_clock::time_point wall_last_{};
};

int FfmpegPlayer::interrupt_cb(void *opaque)
{
    const FfmpegPlayer *player = static_cast<const FfmpegPlayer *>(opaque);
    if (player->abort_.load())
    {
        return 1;
    }
    const std::int64_t now = now_us();
    const std::int64_t open_deadline = player->open_deadline_us_.load();
    if (open_deadline != 0 && now >= open_deadline)
    {
        return 1;
    }
    const std::int64_t update_deadline = player->update_deadline_us_.load();
    // The update budget keeps the frame thread responsive but must not cut a
    // read that is still transferring: it only fires once the deadline has been
    // exceeded by kInterruptGraceUs. out_of_time() alone paces the pumps.
    if (update_deadline != 0 && now >= update_deadline + kInterruptGraceUs)
    {
        return 1;
    }
    return 0;
}

bool FfmpegPlayer::out_of_time() const
{
    const std::int64_t deadline = update_deadline_us_.load();
    return deadline != 0 && now_us() >= deadline;
}

bool FfmpegPlayer::open(const std::string &video_url, const std::string &audio_url, AudioSink *sink)
{
    release();
    error_.clear();
    sink_ = sink;
    audio_recoveries_ = 0;
    if (sink_ != nullptr)
        sink_->set_paused(false);

    if (video_url.empty())
    {
        error_ = "open: no video url";
        sink_ = nullptr;
        return false;
    }

    // Hard wall-clock limit for the whole open sequence: a dead host cannot
    // pin the worker thread, the interrupt stops the network call instead.
    open_deadline_us_.store(now_us() + kOpenTimeoutUs);
    const auto fail = [this](std::string message) -> bool
    {
        if (now_us() >= open_deadline_us_.load())
        {
            log::line("ffmpeg: open deadline reached");
            message = "open timed out";
        }
        release();
        error_ = std::move(message);
        return false;
    };

    {
        size_t live = 0;
        size_t peak = 0;
        size_t failures = 0;
        hui_heap_stats(&live, &peak, nullptr, &failures);
        log::line("heap: %zu MiB used, peak %zu, of %zu MiB, %zu failed", live >> 20, peak >> 20,
                  hui_heap_capacity() >> 20, failures);
    }
    std::string audio_source = audio_url;
    live_ = is_live_url(video_url);
    if (live_)
    {
        // One download serves both demuxers when the variant carries its own
        // sound; a separate sound playlist gets a stream of its own.
        const bool shared = sink != nullptr && audio_source == video_url;
        live_video_ = std::make_unique<hui::ps5::LiveStream>(video_url, "video", shared ? 2 : 1,
                                                             &FfmpegPlayer::interrupt_cb, this);
        std::string live_error;
        if (live_video_->start(&live_error) < 0)
            return fail("live: " + live_error);
        if (!shared && !audio_source.empty() && sink != nullptr)
        {
            live_audio_ = std::make_unique<hui::ps5::LiveStream>(audio_source, "audio", 1,
                                                                 &FfmpegPlayer::interrupt_cb, this);
            if (live_audio_->start(&live_error) < 0)
            {
                log::line("ffmpeg: live audio failed (%s): picture only", live_error.c_str());
                live_audio_.reset();
                audio_source.clear();
            }
        }
        else if (!shared)
            audio_source.clear();
    }
    if (!open_demuxer(video_, video_url, true))
        return fail(error_);
    if (live_ && video_.format->start_time != AV_NOPTS_VALUE)
    {
        pts_origin_ = static_cast<double>(video_.format->start_time) / AV_TIME_BASE;
        log::line("ffmpeg: live stream, timeline starts at %.3f s", pts_origin_);
    }
    quality_ = make_quality_label();

    // Decode mode. Hardware is attempted only when the app asked for it and the
    // console's Videodec2 has a proven path for this codec (AVC and VP9; AV1
    // has none), so software mode never touches the hardware API at all. Every
    // failure here is logged and software takes over: open() never fails
    // because the hardware path was unavailable.
    if (wants_hardware())
    {
        const Codec codec = codec_from_av_id(video_.stream->codecpar->codec_id);
        if (codec != Codec::unknown && hui::ps5::videodec_supports(codec))
        {
            std::string hardware_error;
            hardware_ = std::make_unique<hui::ps5::HardwareDecoder>();
            const int depth = player_tuning().hardware_depth >= 2 ? 2 : 1;
            bool opened = hardware_->open(codec, video_.stream->codecpar->width,
                                          video_.stream->codecpar->height, &hardware_error, depth);
            if (!opened && depth > 1)
            {
                // The experimental depth refused: the proven one.
                log::line("ffmpeg: hardware depth 2 refused (%s); depth 1", hardware_error.c_str());
                hardware_error.clear();
                opened = hardware_->open(codec, video_.stream->codecpar->width,
                                         video_.stream->codecpar->height, &hardware_error, 1);
            }
            if (!opened || !hardware_->ready())
            {
                if (hardware_error.empty())
                {
                    hardware_error = "decoder session not ready";
                }
                log::line("ffmpeg: hardware decode unavailable: %s, using software",
                          hardware_error.c_str());
                hardware_.reset();
            }
            else if (!start_hardware())
            {
                // The session is up but this stream cannot take the
                // experimental read-back path (see start_hardware's line).
                hardware_->close();
                hardware_.reset();
            }
        }
    }

    if (video_.stream->codecpar->codec_id == AV_CODEC_ID_AV1)
    {
        // A decoder that opens but never emits a picture is a black screen the
        // viewer cannot recover from; probing turns that into a precise open
        // failure so the app falls back to the next format. Software AV1 on
        // this CPU is much slower than real time, so it gets a real window (2 s
        // / 24 access units) before a verdict instead of a handful of packets.
        probe_video_frames(kAv1ProbeFrames, kAv1ProbeWindowUs);
        if (!frame_seen_)
        {
            return fail(error_.empty()
                            ? "av1: the software decoder produced no frames in 2 s (this "
                              "console has no AV1 hardware path)"
                            : error_);
        }
    }

    if (audio_source.empty())
    {
        // Nothing about this is an error, but the reason for silence belongs in
        // the log rather than being inferred from the picture.
        log::line("ffmpeg: audio inactive: no audio url");
    }
    else if (sink == nullptr)
    {
        // The app must hand over the sink it started; without it nothing can be
        // heard and the video looks like a decoder bug. Loud on purpose.
        log::line("ffmpeg: audio disabled: audio url present but sink=null (defect)");
    }
    else
    {
        if (!open_demuxer(audio_, audio_source, false))
            return fail(error_);
        if (live_ && audio_.format->start_time != AV_NOPTS_VALUE)
        {
            // The sound's first sample sits where it sits on the shared
            // timeline: the audio clock starts there, not at zero.
            audio_base_s_ =
                static_cast<double>(audio_.format->start_time) / AV_TIME_BASE - pts_origin_;
        }
        if (!init_audio_resampler())
            return fail(error_);
        audio_active_ = true;
        log::line("ffmpeg: audio active sink=%p", static_cast<const void *>(sink));
    }

    open_deadline_us_.store(0);
    running_ = true;
    start_priming();
    return true;
}

void FfmpegPlayer::close()
{
    release();
    error_.clear();
}

bool FfmpegPlayer::open_demuxer(Demuxer &demuxer, const std::string &url, bool video)
{
    AVFormatContext *format = avformat_alloc_context();
    if (format == nullptr)
    {
        error_ = "avformat_alloc_context failed";
        return false;
    }
    format->interrupt_callback.callback = &FfmpegPlayer::interrupt_cb;
    format->interrupt_callback.opaque = this;

    log::line("ffmpeg: open %s url=%s", video ? "video" : "audio", url.substr(0, 64).c_str());
    if (live_)
    {
        // A live broadcast: the LiveStream joins the segments into one stream.
        const bool own_audio = !video && live_audio_ != nullptr;
        hui::ps5::LiveStream *stream = own_audio ? live_audio_.get() : live_video_.get();
        demuxer.live = stream;
        demuxer.live_reader = video || own_audio ? 0 : 1;
        constexpr int kIoBuffer = 64 << 10;
        auto *buffer = static_cast<unsigned char *>(av_malloc(kIoBuffer));
        demuxer.io =
            buffer != nullptr
                ? avio_alloc_context(buffer, kIoBuffer, 0, stream->reader(demuxer.live_reader),
                                     &hui::ps5::LiveStream::read_cb, nullptr, nullptr)
                : nullptr;
        if (demuxer.io == nullptr)
        {
            av_free(buffer);
            error_ = "avio_alloc_context failed";
            avformat_free_context(format);
            release_io(demuxer);
            return false;
        }
        format->pb = demuxer.io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    // googlevideo cuts ffmpeg's single open-ended request after a few MiB and
    // refuses the reconnect (403): read https streams in bounded ranges.
    else if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)
    {
        demuxer.range = std::make_unique<hui::ps5::RangeStream>(url, video ? "video" : "audio",
                                                                &FfmpegPlayer::interrupt_cb, this);
        std::string range_error;
        const int started = demuxer.range->start(&range_error);
        if (started < 0)
        {
            error_ =
                "avformat_open_input: " + (range_error.empty() ? av_error(started) : range_error);
            avformat_free_context(format);
            release_io(demuxer);
            return false;
        }
        constexpr int kIoBuffer = 64 << 10;
        auto *buffer = static_cast<unsigned char *>(av_malloc(kIoBuffer));
        demuxer.io = buffer != nullptr
                         ? avio_alloc_context(buffer, kIoBuffer, 0, demuxer.range.get(),
                                              &hui::ps5::RangeStream::read_cb, nullptr,
                                              &hui::ps5::RangeStream::seek_cb)
                         : nullptr;
        if (demuxer.io == nullptr)
        {
            av_free(buffer);
            error_ = "avio_alloc_context failed";
            avformat_free_context(format);
            release_io(demuxer);
            return false;
        }
        format->pb = demuxer.io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
    }

    AVDictionary *options = nullptr;
    if (demuxer.io == nullptr)
    {
        av_dict_set(&options, "timeout", kNetworkTimeout, 0);
        av_dict_set(&options, "rw_timeout", kNetworkTimeout, 0);
        av_dict_set(&options, "reconnect", "1", 0);
    }
    // A live stream's bytes are MPEG-TS whatever its playlist URL says: no
    // name, so the probe goes by the content alone.
    int result = avformat_open_input(&format, live_ ? "" : url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (result < 0)
    {
        error_ = "avformat_open_input: " + av_error(result);
        avformat_close_input(&format); // frees nothing of the custom io
        release_io(demuxer);
        return false;
    }
    demuxer.format = format;

    result = avformat_find_stream_info(demuxer.format, nullptr);
    log::line("ffmpeg: find_stream_info %s -> %d", video ? "video" : "audio", result);
    if (result < 0)
    {
        error_ = "avformat_find_stream_info: " + av_error(result);
        return false;
    }

    const AVCodec *decoder = nullptr;
    const AVMediaType type = video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    const int index = av_find_best_stream(demuxer.format, type, -1, -1, &decoder, 0);
    if (index < 0)
    {
        error_ = std::string(video ? "no video stream: " : "no audio stream: ") + av_error(index);
        return false;
    }
    demuxer.stream = demuxer.format->streams[index];
    demuxer.stream_index = index;

    {
        const AVCodecParameters *parameters = demuxer.stream->codecpar;
        const int first = video ? parameters->width : parameters->sample_rate;
        const int second = video ? parameters->height : parameters->ch_layout.nb_channels;
        log::line("ffmpeg: %s stream %s %dx%d", video ? "video" : "audio",
                  avcodec_get_name(parameters->codec_id), first, second);
    }

    const AVCodec *codec =
        decoder != nullptr ? decoder : avcodec_find_decoder(demuxer.stream->codecpar->codec_id);
    if (codec == nullptr)
    {
        // AV1 is named so the app can skip it: its absence is a property of this
        // ffmpeg build, not of the stream.
        error_ = video && demuxer.stream->codecpar->codec_id == AV_CODEC_ID_AV1
                     ? "av1: no decoder in this build"
                     : "no decoder for stream";
        return false;
    }

    demuxer.codec = avcodec_alloc_context3(codec);
    if (demuxer.codec == nullptr)
    {
        error_ = "avcodec_alloc_context3 failed";
        return false;
    }
    result = avcodec_parameters_to_context(demuxer.codec, demuxer.stream->codecpar);
    if (result < 0)
    {
        error_ = "avcodec_parameters_to_context: " + av_error(result);
        return false;
    }
    demuxer.codec->pkt_timebase = demuxer.stream->time_base;
    const bool large = video && demuxer.stream->codecpar->height > 1080;
    const PlayerTuning tuning = player_tuning();
    demuxer.codec->thread_count = tuning.decoder_threads > 0
                                      ? tuning.decoder_threads
                                      : (large ? kDecoderThreadsLarge : kDecoderThreads);
    if (video)
    {
        decoder_threads_ = demuxer.codec->thread_count;
        convert_threads_ = tuning.convert_threads > 0 ? tuning.convert_threads : kConvertThreads;
        deblock_skipped_ = tuning.skip_deblock == 2 || (tuning.skip_deblock == 1 && large);
    }
    if (video && deblock_skipped_)
    {
        // Above 1080p the picture is scaled down to the screen anyway, which
        // hides what the in-loop deblocking filter would smooth; skipping it
        // and taking the decoder's faster paths saves a large share of the
        // decoding work.
        demuxer.codec->skip_loop_filter = AVDISCARD_ALL;
        demuxer.codec->flags2 |= AV_CODEC_FLAG2_FAST;
    }
    result = avcodec_open2(demuxer.codec, codec, nullptr);
    if (result < 0)
    {
        error_ = "avcodec_open2: " + av_error(result);
        return false;
    }

    const char *const id_name = avcodec_get_name(demuxer.stream->codecpar->codec_id);
    if (video)
    {
        // Frame rate sets what "the decoder keeps up" means. The stored value
        // is the interval, not the rate. A wrong or absent value (0/0 in some
        // fragmented answers) falls back to 30 fps.
        AVRational rate = demuxer.stream->avg_frame_rate;
        if (rate.num <= 0 || rate.den <= 0)
        {
            rate = demuxer.stream->r_frame_rate;
        }
        double fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0.0;
        if (!(fps > 0.0) || fps > 1000.0)
        {
            fps = 30.0;
        }
        video_interval_s_ = 1.0 / fps;
        log::line("ffmpeg: video codec %s (sw) decoder=%s threads=%d fps=%.2f", id_name,
                  codec->name, demuxer.codec->thread_count, fps);
    }
    else
    {
        char layout[64] = {};
        if (av_channel_layout_describe(&demuxer.codec->ch_layout, layout, sizeof(layout)) < 0)
        {
            std::strcpy(layout, "unknown");
        }
        const char *const sample_format = av_get_sample_fmt_name(demuxer.codec->sample_fmt);
        log::line("ffmpeg: audio codec %s (sw) decoder=%s rate=%d layout=%s fmt=%s", id_name,
                  codec->name, demuxer.codec->sample_rate, layout,
                  sample_format != nullptr ? sample_format : "none");
    }

    demuxer.packet = av_packet_alloc();
    demuxer.frame = av_frame_alloc();
    if (demuxer.packet == nullptr || demuxer.frame == nullptr)
    {
        error_ = "av_packet_alloc/av_frame_alloc failed";
        return false;
    }
    return true;
}

bool FfmpegPlayer::init_audio_resampler()
{
    if (audio_.codec == nullptr)
    {
        error_ = "no audio codec";
        return false;
    }
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout in_layout = audio_.codec->ch_layout;
    if (in_layout.nb_channels <= 0)
    {
        av_channel_layout_default(&in_layout, 2);
    }
    const int in_rate = audio_.codec->sample_rate > 0 ? audio_.codec->sample_rate : kOutputRate;
    char layout[64] = {};
    if (av_channel_layout_describe(&in_layout, layout, sizeof(layout)) < 0)
    {
        std::strcpy(layout, "unknown");
    }
    const char *const sample_format = av_get_sample_fmt_name(audio_.codec->sample_fmt);
    int result = swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_FLT, kOutputRate, &in_layout,
                                     audio_.codec->sample_fmt, in_rate, 0, nullptr);
    if (result < 0 || swr_ == nullptr)
    {
        error_ = "swr_alloc_set_opts2: " + av_error(result < 0 ? result : AVERROR(ENOMEM));
        log::line("ffmpeg: audio resampler failed: %s (in=%d Hz %s %s)", error_.c_str(), in_rate,
                  layout, sample_format != nullptr ? sample_format : "none");
        return false;
    }
    result = swr_init(swr_);
    if (result < 0)
    {
        error_ = "swr_init: " + av_error(result);
        log::line("ffmpeg: audio resampler failed: %s (in=%d Hz %s %s)", error_.c_str(), in_rate,
                  layout, sample_format != nullptr ? sample_format : "none");
        return false;
    }
    log::line("ffmpeg: audio resampler %d Hz %s %s -> %d Hz stereo flt", in_rate, layout,
              sample_format != nullptr ? sample_format : "none", kOutputRate);
    return true;
}

std::string FfmpegPlayer::make_quality_label() const
{
    if (video_.stream == nullptr)
    {
        return std::string();
    }
    int height = video_.stream->codecpar->height;
    if (height <= 0 && video_.codec != nullptr)
    {
        height = video_.codec->height;
    }
    return std::to_string(height) + "p " + video_codec_label(video_.stream->codecpar->codec_id);
}

void FfmpegPlayer::update()
{
    if (!running_)
    {
        return;
    }
    if (audio_.live != nullptr && audio_.drained)
    {
        // The sound path ended: its reader must stop holding the bytes the
        // picture's reader shares with it.
        audio_.live->close_reader(audio_.live_reader);
        audio_.live = nullptr;
    }
    av_offset_s_ = static_cast<double>(player_tuning().av_offset_ms) / 1000.0;
    // A per-call budget: a stalled read is cut off by the interrupt instead of
    // freezing the frame thread that drives the watchdog and the whole UI.
    const std::int64_t start_us = now_us();
    update_deadline_us_.store(start_us + kUpdateBudgetUs);
    if (!paused_)
    {
        pump_video(kVideoPacketsPerUpdate);
        if (audio_active_)
        {
            pump_audio(kAudioPacketsPerUpdate);
        }
        if (audio_active_ && audio_.drained && audio_recoveries_ < kMaxAudioRecoveries &&
            duration_s() > 0.0 && clock_ < duration_s() - 5.0)
        {
            // The sound stopped well before the end: a stalled or refused
            // read, not the end of the stream. Start both streams over at the
            // clock (the range reader asks again, the decoders restart) rather
            // than playing the rest of the video silent.
            ++audio_recoveries_;
            log::line("ffmpeg: audio stopped at %.1fs of %.1fs, restarting it (%d)", clock_,
                      duration_s(), audio_recoveries_);
            audio_.drained = false;
            seek(clock_);
        }
        if (live_ && audio_active_ && !audio_.drained && !priming_ && audio_frames_written_ == 0 &&
            std::chrono::steady_clock::now() - priming_since_ > std::chrono::seconds(8))
        {
            // A live stream whose sound never started must not hold the
            // picture on its first frame: the wall clock takes over below.
            log::line("ffmpeg: live sound never started, playing the picture alone");
            audio_.drained = true;
        }
        if (audio_active_ && audio_.drained)
        {
            // The audio path is spent (EOF, a failed decoder or a dead read):
            // an audio clock that will never advance again must not hold the
            // picture. The wall clock takes over from the position the audio
            // clock had reached, exactly as for video-only playback, so the
            // queued pictures keep presenting instead of being held forever.
            // Clearing audio_active_ also makes this fire once.
            audio_active_ = false;
            if (!wall_anchor_)
            {
                wall_anchor_ = true;
                wall_last_ = std::chrono::steady_clock::now();
            }
            log::line("ffmpeg: audio ended, clock on the wall clock");
        }
        if (priming_)
        {
            const bool ready =
                sink_ != nullptr &&
                sink_->buffered() >= static_cast<std::size_t>(kOutputRate * 6 / 10) &&
                (video_queue_.size() >= 4 || video_.drained);
            const double waited =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - priming_since_)
                    .count();
            if (ready || waited > 2.5 || !audio_active_)
            {
                priming_ = false;
                if (sink_ != nullptr)
                    sink_->set_paused(paused_);
                log::line("ffmpeg: primed in %.2fs (%s)", waited, ready ? "ready" : "timeout");
            }
        }
        update_clock();
    }
    const std::int64_t elapsed_us = now_us() - start_us;
    const bool budget_reached = out_of_time();
    update_deadline_us_.store(0);
    if (budget_reached)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_budget_log_ >= std::chrono::seconds(1))
        {
            last_budget_log_ = now;
            log::line("ffmpeg: read budget reached (%d ms)", static_cast<int>(elapsed_us / 1000));
        }
    }
    if (video_.drained)
    {
        ended_ = true;
    }
}

// open() runs this before playback. The packets it consumes are decoded for
// real: their pictures go through emit_video, so the probe costs nothing beyond
// the frames it needs to prove that the decoder works at all. It gives up after
// `packets` access units or `window_us` of wall time, whichever comes first.
void FfmpegPlayer::probe_video_frames(int packets, std::int64_t window_us)
{
    int sent = 0;
    const std::int64_t window_deadline = now_us() + window_us;
    while (sent < packets && !video_.drained && !frame_seen_)
    {
        if (abort_.load() || now_us() >= open_deadline_us_.load() || now_us() >= window_deadline)
        {
            return;
        }
        const int received = avcodec_receive_frame(video_.codec, video_.frame);
        if (received == 0)
        {
            emit_video(video_.frame);
            av_frame_unref(video_.frame);
            continue;
        }
        if (received != AVERROR(EAGAIN))
        {
            return; // end of stream, or a decoder that refuses the stream
        }
        const int read = av_read_frame(video_.format, video_.packet);
        if (read < 0)
        {
            // A failed probe read (403, timeout, a cut) is a stream problem,
            // not a decoder one: keep its exact reason so open() reports that
            // and the caller's format fallback reacts to it, instead of the
            // misleading "produced no frames". End of stream before a picture
            // keeps that wording, because there the decoder really produced
            // nothing, and a cut by the open deadline is reported as the open
            // timeout it is.
            if (read != AVERROR_EOF && !is_interrupt_error(read))
            {
                log::line("ffmpeg: video probe read failed: %s (%d)", av_error(read).c_str(), read);
                if (error_.empty())
                {
                    error_ = "av1: probe read failed: " + av_error(read);
                }
            }
            return;
        }
        if (video_.packet->stream_index != video_.stream_index)
        {
            av_packet_unref(video_.packet);
            continue;
        }
        ++sent;
        int result = avcodec_send_packet(video_.codec, video_.packet);
        while (result == AVERROR(EAGAIN) && !frame_seen_)
        {
            const int drain = avcodec_receive_frame(video_.codec, video_.frame);
            if (drain != 0)
            {
                break;
            }
            emit_video(video_.frame);
            av_frame_unref(video_.frame);
            result = avcodec_send_packet(video_.codec, video_.packet);
        }
        av_packet_unref(video_.packet);
    }
}

std::int64_t FfmpegPlayer::frame_interval_us() const
{
    const double interval = video_interval_s_ > 0.0 ? video_interval_s_ : 1.0 / 30.0;
    return static_cast<std::int64_t>(interval * 1.0e6);
}

void FfmpegPlayer::note_decode_slow(std::int64_t spent_us)
{
    if (spent_us <= frame_interval_us())
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_decode_log_ < std::chrono::seconds(1))
    {
        return;
    }
    last_decode_log_ = now;
    log::line("ffmpeg: decode slow (%d ms/frame, %s)", static_cast<int>(spent_us / 1000),
              video_.stream != nullptr ? video_codec_label(video_.stream->codecpar->codec_id)
                                       : "?");
}

void FfmpegPlayer::note_video_drop()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - last_drop_log_ < std::chrono::seconds(1))
    {
        return;
    }
    last_drop_log_ = now;
    log::line("ffmpeg: video dropped %llu late frames (picture behind the audio clock)",
              static_cast<unsigned long long>(video_frames_dropped_.load()));
}

void FfmpegPlayer::note_audio_silence(const char *reason)
{
    const auto now = std::chrono::steady_clock::now();
    if (audio_silence_reason_ == reason && now - last_audio_log_ < std::chrono::seconds(1))
    {
        return;
    }
    audio_silence_reason_ = reason;
    last_audio_log_ = now;
    log::line("ffmpeg: audio silent: %s", reason);
}

void FfmpegPlayer::note_buffer_acquire(int index, std::size_t size)
{
    const auto now = std::chrono::steady_clock::now();
    if (now - last_acquire_log_ < std::chrono::seconds(1))
    {
        return;
    }
    last_acquire_log_ = now;
    log::line("ffmpeg: video buffer acquired index=%d size=%zu pool=%zu free=%zu", index, size,
              video_buffer_count_, video_free_count_);
}

void FfmpegPlayer::note_buffer_release(int index, std::size_t size)
{
    const auto now = std::chrono::steady_clock::now();
    if (now - last_release_log_ < std::chrono::seconds(1))
    {
        return;
    }
    last_release_log_ = now;
    log::line("ffmpeg: video buffer released index=%d size=%zu pool=%zu free=%zu", index, size,
              video_buffer_count_, video_free_count_);
}

void FfmpegPlayer::note_no_memory(int width, int height, std::size_t size)
{
    const auto now = std::chrono::steady_clock::now();
    if (now - last_nomem_log_ < std::chrono::seconds(1))
    {
        return;
    }
    last_nomem_log_ = now;
    log::line("ffmpeg: picture dropped, no memory for a %dx%d slot (%zu MB)", width, height,
              size / (1024u * 1024u));
}

void FfmpegPlayer::lower_queue_depth()
{
    // The queue holds at most kVideoQueueDepth entries, so the depth must stay
    // within it: in_use can reach the queue plus the exposed picture (13), and
    // a depth above the capacity would make the drop-oldest guard unreachable.
    const std::size_t in_use = video_buffer_count_ - video_free_count_;
    video_queue_depth_ = std::clamp<std::size_t>(in_use, 1u, kVideoQueueDepth);
}

void FfmpegPlayer::clear_video_pool()
{
    for (VideoBuffer &slot : video_buffers_)
    {
        slot.bytes.reset();
        slot.size = 0;
    }
    video_buffer_count_ = 0;
    video_free_count_ = 0;
}

bool FfmpegPlayer::wants_hardware() const
{
    // Hardware on request, and on Auto above 1080p: there the CPU cannot keep
    // up (a 4K VP9 stream runs it out of memory and frames), while the
    // console's decoder plays it.
    if (decode_mode_ == DecodeMode::hardware)
        return true;
    return decode_mode_ == DecodeMode::auto_detect && video_.stream != nullptr &&
           video_.stream->codecpar->height > 1080;
}

bool FfmpegPlayer::start_hardware()
{
    // Experimental, and only on request: Decoding set to Hardware, a VP9
    // stream of 8-bit 4:2:0 (profile 0). AVC reorders pictures (B-frames),
    // which a depth-one session cannot be trusted with; HDR VP9 is profile 2.
    const AVCodecParameters *par = video_.stream->codecpar;
    const char *refusal = nullptr;
    if (!wants_hardware())
        refusal = decode_mode_ == DecodeMode::software ? "Decoding is set to Software"
                                                       : "Auto decodes 1080p and below on the CPU";
    else if (par->codec_id != AV_CODEC_ID_VP9)
        refusal = "only VP9 takes the hardware path";
    else if (par->profile > 0 || par->bits_per_raw_sample > 8 ||
             par->color_trc == AVCOL_TRC_SMPTE2084 || par->color_trc == AVCOL_TRC_ARIB_STD_B67)
        refusal = "a 10-bit / HDR VP9 stream (profile 2)";
    const AVBitStreamFilter *split =
        refusal == nullptr ? av_bsf_get_by_name("vp9_superframe_split") : nullptr;
    if (refusal == nullptr && split == nullptr)
        refusal = "no vp9_superframe_split filter in this build";
    if (refusal == nullptr &&
        (av_bsf_alloc(split, &hardware_bsf_) < 0 ||
         avcodec_parameters_copy(hardware_bsf_->par_in, par) < 0 || av_bsf_init(hardware_bsf_) < 0))
        refusal = "the superframe filter did not start";
    if (refusal == nullptr)
    {
        hardware_packet_ = av_packet_alloc();
        hardware_frame_ = av_frame_alloc();
        if (hardware_packet_ == nullptr || hardware_frame_ == nullptr)
            refusal = "out of memory";
    }
    if (refusal != nullptr)
    {
        log::line("ffmpeg: hardware decode not used: %s", refusal);
        if (hardware_bsf_ != nullptr)
            av_bsf_free(&hardware_bsf_);
        av_packet_free(&hardware_packet_);
        av_frame_free(&hardware_frame_);
        return false;
    }
    const bool nv12 = player_tuning().gpu_convert && par->width <= 4096 && par->height <= 2304;
    hardware_worker_ =
        std::make_unique<HardwareWorker>(hardware_.get(), par->height, convert_threads_, nv12);
    if (!hardware_worker_->start())
    {
        log::line("ffmpeg: hardware decode not used: no decode thread");
        hardware_worker_.reset();
        av_bsf_free(&hardware_bsf_);
        av_packet_free(&hardware_packet_);
        av_frame_free(&hardware_frame_);
        return false;
    }
    hardware_active_ = true;
    hardware_failures_ = 0;
    hardware_restarts_ = 0;
    hardware_order_dumps_ = 0;
    hardware_pictures_ = 0;
    log::line("ffmpeg: hardware decode ON (experimental): vp9 %dx%d via Videodec2 on its own "
              "thread, depth %d, %d free slots, %s",
              par->width, par->height, hardware_->depth(), hardware_->frame_slots(),
              nv12 ? "NV12 to the GPU" : "RGBA converted on the CPU");
    return true;
}

void FfmpegPlayer::restart_hardware()
{
    // As a seek does it, without moving: settle, reset, skip to a keyframe.
    if (hardware_worker_ != nullptr)
        hardware_worker_->settle();
    std::string ignored;
    if (hardware_ != nullptr)
        (void)hardware_->reset(&ignored);
    av_bsf_flush(hardware_bsf_);
    hardware_wait_key_ = true;
    hardware_skipped_to_key_ = 0;
    hardware_failures_ = 0;
}

void FfmpegPlayer::fall_back_to_software(const std::string &why)
{
    log::line("ffmpeg: hardware decode OFF after %llu pictures: %s; software from here",
              static_cast<unsigned long long>(hardware_pictures_), why.c_str());
    hardware_active_ = false;
    hardware_worker_.reset(); // waits for the decode in flight
    if (hardware_ != nullptr)
        hardware_->close();
    hardware_.reset();
    if (hardware_bsf_ != nullptr)
        av_bsf_free(&hardware_bsf_);
    av_packet_free(&hardware_packet_);
    av_frame_free(&hardware_frame_);
    // The software decoder starts from a keyframe: back to where the picture is.
    const double at = frame_seen_ ? last_video_pts_ : clock_;
    seek(at > 0.0 ? at : 0.0);
}

void FfmpegPlayer::pump_video_hardware(int budget)
{
    int work = 0;
    while (work < budget && hardware_active_ && !video_.drained)
    {
        if (out_of_time())
            return;
        // Converted pictures first: each one is a buffer swap into the pool.
        if (video_queue_.size() < video_queue_depth_)
        {
            HardwareWorker::Picture picture;
            if (hardware_worker_->take(&picture))
            {
                ++work;
                if (!picture.ok)
                {
                    log::line("ffmpeg: hardware decode failed: %s", picture.error.c_str());
                    if (++hardware_failures_ >= 3 || hardware_pictures_ == 0)
                    {
                        // Above 1080p software cannot keep up: the decoder
                        // starts over at the next keyframe instead, a few
                        // times, before giving up on it.
                        if (hardware_pictures_ > 0 && hardware_restarts_ < 5)
                        {
                            ++hardware_restarts_;
                            log::line("ffmpeg: hardware decoder restarted at the next keyframe "
                                      "(%d)",
                                      hardware_restarts_);
                            restart_hardware();
                            return;
                        }
                        fall_back_to_software(picture.error);
                        return;
                    }
                    continue;
                }
                hardware_failures_ = 0;
                queue_hardware_picture(picture);
                continue;
            }
        }
        // Then keep the worker fed; it decodes while this thread draws.
        if (!hardware_worker_->wants_packet())
            return;
        const int filtered = av_bsf_receive_packet(hardware_bsf_, hardware_packet_);
        if (filtered == 0)
        {
            hardware_worker_->submit(hardware_packet_);
            ++work;
            continue;
        }
        if (filtered != AVERROR(EAGAIN))
        {
            if (filtered == AVERROR_EOF)
            {
                // The end: once the worker has handed over its last picture.
                if (!hardware_worker_->empty())
                    return;
                video_.drained = true;
                log::line("ffmpeg: video stream ended (eof, hardware)");
                return;
            }
            fall_back_to_software("superframe filter: " + av_error(filtered));
            return;
        }
        if (video_.eof)
        {
            av_bsf_send_packet(hardware_bsf_, nullptr); // drains, then reports EOF
            continue;
        }
        if (live_starved(video_))
            return;
        const int read = av_read_frame(video_.format, video_.packet);
        if (read >= 0)
        {
            video_.consecutive_errors = 0;
            video_.stalled_reads = 0;
            const bool ours = video_.packet->stream_index == video_.stream_index;
            if (ours && hardware_wait_key_ && (video_.packet->flags & AV_PKT_FLAG_KEY) == 0)
            {
                // After a seek the decoder starts over: a picture that refers
                // to frames it never saw decodes as a smear of the old scene.
                // Everything up to the next keyframe is left out.
                ++hardware_skipped_to_key_;
                av_packet_unref(video_.packet);
                ++work;
                continue;
            }
            if (ours && hardware_wait_key_)
            {
                hardware_wait_key_ = false;
                if (hardware_skipped_to_key_ > 0)
                    log::line("ffmpeg: hardware seek: %d packets before the keyframe left out",
                              hardware_skipped_to_key_);
                hardware_skipped_to_key_ = 0;
            }
            if (ours)
                av_bsf_send_packet(hardware_bsf_, video_.packet); // takes the packet
            av_packet_unref(video_.packet);
            ++work;
            continue;
        }
        av_packet_unref(video_.packet);
        if (read == AVERROR(EAGAIN) || abort_.load())
            return;
        if (is_interrupt_error(read) || out_of_time())
        {
            if (note_stalled_read(video_))
                return;
            error_ = "read stalled: no data for " + std::to_string(video_.stalled_reads) + " s";
            log::line("ffmpeg: video %s", error_.c_str());
            video_.drained = true;
            return;
        }
        if (read == AVERROR_EOF || !note_stream_error(video_, "video", "read", read))
        {
            video_.eof = true;
            continue;
        }
        return;
    }
}

void FfmpegPlayer::queue_hardware_picture(HardwareWorker::Picture &picture)
{
    if (hardware_pictures_ == 0)
        log::line("ffmpeg: hardware picture (%d) pitch=%d slot=%zu -> %dx%d %s",
                  picture.source_height, picture.pitch, picture.slot_size, picture.width,
                  picture.height, picture.nv12 ? "NV12" : "RGBA");
    ++hardware_pictures_;
    const double pts =
        picture.pts == AV_NOPTS_VALUE || video_.stream == nullptr
            ? (frame_seen_ ? last_video_pts_ + 1.0 / 30.0 : 0.0)
            : static_cast<double>(picture.pts) * av_q2d(video_.stream->time_base) - pts_origin_;
    // Diagnostics: pictures that never came out (a jump past one frame's
    // step) and pictures out of order.
    if (frame_seen_ && video_.stream != nullptr && video_seek_target_ < 0.0)
    {
        const AVRational rate = video_.stream->avg_frame_rate;
        const double step = rate.num > 0 && rate.den > 0 ? av_q2d(av_inv_q(rate)) : 1.0 / 30.0;
        const double gap = pts - last_video_pts_;
        if (gap < 0.0)
        {
            ++hardware_out_of_order_;
            // The first few: what the decoder was given and gave back.
            if (hardware_order_dumps_ < 4)
            {
                ++hardware_order_dumps_;
                log::line("ffmpeg: picture at %.3f after %.3f; the last decodes:", pts,
                          last_video_pts_);
                hardware_worker_->log_recent(av_q2d(video_.stream->time_base));
            }
        }
        else if (gap > step * 1.5 && gap < 2.0)
            hardware_missing_ += static_cast<int>(gap / step + 0.5) - 1;
    }
    if (video_seek_target_ >= 0.0)
    {
        // Decoded only to reach the seek target (capped as in software).
        if (pts + kLateFrameToleranceS < video_seek_target_ && ++video_seek_skipped_ < 600)
        {
            hardware_worker_->recycle(std::move(picture.bytes), picture.size);
            return;
        }
        video_seek_target_ = -1.0;
    }
    source_height_.store(picture.source_height);
    const int width = picture.width;
    const int height = picture.height;
    const std::size_t size = picture.size;
    if (video_width_ != width || video_height_ != height || video_nv12_ != picture.nv12)
    {
        video_nv12_ = picture.nv12;
        log::line("ffmpeg: video resolution %dx%d -> %dx%d", video_width_, video_height_, width,
                  height);
        video_queue_.clear();
        clear_video_pool();
        exposed_buffer_ = -1;
        exposed_pts_ = kNoPts;
        picture_presented_ = false;
        video_width_ = width;
        video_height_ = height;
        const std::size_t headroom = size * kVideoPoolHeadroom;
        const std::size_t queue_budget =
            kVideoPoolBudgetBytes > headroom ? kVideoPoolBudgetBytes - headroom : 0u;
        video_queue_depth_ = std::clamp(queue_budget / size, kVideoMinQueueDepth, kVideoQueueDepth);
        log::line("ffmpeg: video queue depth %zu (%zu MB)", video_queue_depth_,
                  size * (video_queue_depth_ + kVideoPoolHeadroom) / (1024u * 1024u));
    }
    const int target = acquire_video_buffer(size);
    if (target < 0 || static_cast<std::size_t>(target) >= video_buffer_count_ ||
        video_buffers_[target].size != size)
    {
        if (target >= 0)
            release_video_buffer(target);
        note_no_memory(width, height, size);
        hardware_worker_->recycle(std::move(picture.bytes), picture.size);
        return;
    }
    // The converted picture becomes the pool slot; the slot's old memory
    // goes back to the worker for a later conversion. No copy.
    std::swap(video_buffers_[target].bytes, picture.bytes);
    hardware_worker_->recycle(std::move(picture.bytes), size);
    if (video_queue_.size() >= video_queue_depth_)
    {
        release_video_buffer(video_queue_.front().buffer);
        video_queue_.pop();
    }
    if (!video_queue_.push(QueuedPicture{target, pts}))
    {
        release_video_buffer(target);
        return;
    }
    last_video_pts_ = pts;
    if (!frame_seen_)
        log::line("ffmpeg: first frame %dx%d pts=%.2f", width, height, pts);
    frame_seen_ = true;
    const auto wall = std::chrono::steady_clock::now();
    if (wall - hardware_log_ >= std::chrono::seconds(5))
    {
        hardware_log_ = wall;
        const HardwareWorker::Timing t = hardware_worker_->take_timing();
        log::line("ffmpeg: hardware %llu pictures; last 5 s: %d decodes %.1f ms avg %.1f max, "
                  "%d converts %.1f ms avg %.1f max, ahead-full %d, queue empty %d times; "
                  "no picture %d, several %d, missing %d, out of order %d",
                  static_cast<unsigned long long>(hardware_pictures_), t.decodes,
                  t.decodes > 0 ? static_cast<double>(t.decode_us) / 1000.0 / t.decodes : 0.0,
                  static_cast<double>(t.decode_max_us) / 1000.0, t.converts,
                  t.converts > 0 ? static_cast<double>(t.convert_us) / 1000.0 / t.converts : 0.0,
                  static_cast<double>(t.convert_max_us) / 1000.0, t.waits_for_room,
                  hardware_queue_empty_, t.no_picture, t.several, hardware_missing_,
                  hardware_out_of_order_);
        hardware_queue_empty_ = 0;
        hardware_missing_ = 0;
        hardware_out_of_order_ = 0;
    }
}

void FfmpegPlayer::pump_video(int budget)
{
    if (hardware_active_)
    {
        pump_video_hardware(budget);
        return;
    }
    if (video_.format == nullptr || video_.drained)
    {
        return;
    }
    int work = 0;
    while (work < budget && !video_.drained)
    {
        if (out_of_time())
        {
            return;
        }
        // The queue is full: the decoder is ahead of the clock, so it holds.
        // The frames are already decoded and converted; running further would
        // only grow memory and the distance to the picture on screen.
        if (video_queue_.size() >= video_queue_depth_)
        {
            return;
        }
        const std::int64_t step_start_us = now_us();
        const int received = avcodec_receive_frame(video_.codec, video_.frame);
        if (received == 0)
        {
            video_.consecutive_errors = 0;
            // A picture the audio clock has already passed cannot be caught up
            // with: converting and queueing it would only add latency, so it is
            // dropped and the decoder goes straight back for the next packet.
            // The drop counter is shared with the presentation gate and is only
            // cleared when a picture actually goes on screen, so after
            // kMaxLateDropRun dropped frames the queued picture is shown even
            // though it is late: a black screen and a frozen picture are both
            // ruled out.
            const double pts = frame_pts(video_.frame);
            if (video_seek_target_ >= 0.0)
            {
                // Still before the seek target: decoded only to reach it.
                // Capped (10 s at 60 fps): odd timestamps must never leave the
                // picture black.
                if (pts + kLateFrameToleranceS < video_seek_target_ && ++video_seek_skipped_ < 600)
                {
                    av_frame_unref(video_.frame);
                    ++work;
                    continue;
                }
                video_seek_target_ = -1.0;
            }
            if (frame_seen_ && late_drop_run_ < kMaxLateDropRun && audio_active_ && clock_ > 0.0 &&
                pts + kLateFrameToleranceS < clock_)
            {
                ++video_frames_dropped_;
                ++late_drop_run_;
                note_video_drop();
                av_frame_unref(video_.frame);
                ++work;
                continue;
            }
            emit_video(video_.frame);
            av_frame_unref(video_.frame);
            note_decode_slow(now_us() - step_start_us);
            ++work;
            continue;
        }
        if (received == AVERROR_EOF)
        {
            video_.drained = true;
            log::line("ffmpeg: video stream ended (eof)");
            break;
        }
        if (received != AVERROR(EAGAIN))
        {
            // A cut short by close(), the interrupt or the budget is not a
            // decoder failure: the frame is received again next update().
            if (abort_.load() || is_interrupt_error(received) || out_of_time())
            {
                return;
            }
            if (!note_stream_error(video_, "video", "decode", received))
            {
                video_.drained = true;
                log::line("ffmpeg: video stream ended (decode failed)");
                break;
            }
            ++work;
            continue;
        }
        if (video_.eof)
        {
            // Flushed already and still no frame: the stream is spent.
            video_.drained = true;
            break;
        }
        if (live_starved(video_))
            break;
        const int read = av_read_frame(video_.format, video_.packet);
        if (read >= 0)
        {
            video_.consecutive_errors = 0;
            video_.stalled_reads = 0; // a packet arrived: the host is alive
            if (video_.packet->stream_index == video_.stream_index)
            {
                const int sent = avcodec_send_packet(video_.codec, video_.packet);
                if (sent < 0 && sent != AVERROR(EAGAIN))
                {
                    if (abort_.load() || is_interrupt_error(sent) || out_of_time())
                    {
                        av_packet_unref(video_.packet);
                        return;
                    }
                    if (!note_stream_error(video_, "video", "send_packet", sent))
                    {
                        video_.eof = true;
                        avcodec_send_packet(video_.codec, nullptr);
                    }
                }
            }
            av_packet_unref(video_.packet);
            ++work;
            continue;
        }
        // The read failed. Only a real end of stream or a repeated hard failure
        // ends playback: a read cut by the interrupt (the update budget, the
        // open deadline or close()) is retried on the next update(). A cut read
        // that repeats without ever producing a packet is a dead stream, not a
        // slow one: it pins the frame thread for kInterruptGraceUs on every
        // update, so after kMaxStallReads in a row playback ends with the exact
        // reason instead of hanging at ~1 fps.
        if (read == AVERROR(EAGAIN))
        {
            av_packet_unref(video_.packet);
            break;
        }
        if (abort_.load())
        {
            av_packet_unref(video_.packet);
            return;
        }
        if (is_interrupt_error(read) || out_of_time())
        {
            av_packet_unref(video_.packet);
            if (note_stalled_read(video_))
            {
                return;
            }
            error_ = "read stalled: no data for " + std::to_string(video_.stalled_reads) + " s";
            log::line("ffmpeg: video %s", error_.c_str());
            video_.drained = true; // update() turns this into ended_
            return;
        }
        if (read == AVERROR_EOF)
        {
            video_.eof = true;
            avcodec_send_packet(video_.codec, nullptr);
            ++work;
            continue;
        }
        if (!note_stream_error(video_, "video", "read", read))
        {
            video_.eof = true;
            avcodec_send_packet(video_.codec, nullptr);
            log::line("ffmpeg: video stream ended (read failed)");
            ++work;
            continue;
        }
        av_packet_unref(video_.packet);
        break;
    }
}

void FfmpegPlayer::pump_audio(int budget)
{
    if (audio_.format == nullptr || audio_.drained)
    {
        return;
    }
    int work = 0;
    while (work < budget)
    {
        if (out_of_time())
        {
            return;
        }
        if (carry_frames_ > 0 && !flush_carry())
        {
            break;
        }
        if (sink_ != nullptr && sink_->buffered() >= static_cast<std::size_t>(kAudioCushionFrames))
        {
            break;
        }
        const int received = avcodec_receive_frame(audio_.codec, audio_.frame);
        if (received == 0)
        {
            audio_.consecutive_errors = 0;
            ++audio_frames_decoded_;
            emit_audio(audio_.frame);
            av_frame_unref(audio_.frame);
            ++work;
            continue;
        }
        if (received == AVERROR_EOF)
        {
            audio_.drained = true;
            log::line("ffmpeg: audio stream ended (eof) decoded=%llu written=%llu",
                      static_cast<unsigned long long>(audio_frames_decoded_),
                      static_cast<unsigned long long>(audio_frames_written_));
            break;
        }
        if (received != AVERROR(EAGAIN))
        {
            // The interrupt, the budget and close() are not decoder failures.
            if (abort_.load() || is_interrupt_error(received) || out_of_time())
            {
                return;
            }
            if (!note_stream_error(audio_, "audio", "decode", received))
            {
                audio_.drained = true;
                log::line("ffmpeg: audio stream ended (decode failed)");
                break;
            }
            ++work;
            continue;
        }
        if (audio_.eof)
        {
            audio_.drained = true;
            break;
        }
        if (live_starved(audio_))
            break;
        const int read = av_read_frame(audio_.format, audio_.packet);
        if (read >= 0)
        {
            audio_.consecutive_errors = 0;
            audio_.stalled_reads = 0; // a packet arrived: the host is alive
            if (audio_.packet->stream_index == audio_.stream_index)
            {
                const int sent = avcodec_send_packet(audio_.codec, audio_.packet);
                if (sent < 0 && sent != AVERROR(EAGAIN))
                {
                    if (abort_.load() || is_interrupt_error(sent) || out_of_time())
                    {
                        av_packet_unref(audio_.packet);
                        return;
                    }
                    if (!note_stream_error(audio_, "audio", "send_packet", sent))
                    {
                        audio_.eof = true;
                        avcodec_send_packet(audio_.codec, nullptr);
                    }
                }
            }
            av_packet_unref(audio_.packet);
            ++work;
            continue;
        }
        // The read failed. Only a real end of stream or a repeated hard failure
        // ends playback: a read cut by the interrupt (the update budget, the
        // open deadline or close()) is retried on the next update(). A cut read
        // that keeps producing nothing is a dead audio URL: it is dropped
        // instead of pinning the frame thread for kInterruptGraceUs on every
        // update. The picture can still play without sound.
        if (read == AVERROR(EAGAIN))
        {
            av_packet_unref(audio_.packet);
            break;
        }
        if (abort_.load())
        {
            av_packet_unref(audio_.packet);
            return;
        }
        if (is_interrupt_error(read) || out_of_time())
        {
            av_packet_unref(audio_.packet);
            if (note_stalled_read(audio_))
            {
                return;
            }
            log::line("ffmpeg: audio read stalled: no data for %d s", audio_.stalled_reads);
            // The audio clock cannot be the master clock of a path that is
            // gone. The end of the audio path is turned into the wall-clock
            // fallback that video-only playback already uses by update(), one
            // place for every way the path can end; it anchors at the position
            // the audio clock had reached.
            audio_.drained = true;
            return;
        }
        if (read == AVERROR_EOF)
        {
            audio_.eof = true;
            avcodec_send_packet(audio_.codec, nullptr);
            ++work;
            continue;
        }
        if (!note_stream_error(audio_, "audio", "read", read))
        {
            audio_.eof = true;
            avcodec_send_packet(audio_.codec, nullptr);
            log::line("ffmpeg: audio stream ended (read failed)");
            ++work;
            continue;
        }
        av_packet_unref(audio_.packet);
        break;
    }
}

double FfmpegPlayer::frame_pts(const AVFrame *frame) const
{
    std::int64_t timestamp = frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE)
    {
        timestamp = frame->pts;
    }
    if (timestamp == AV_NOPTS_VALUE || video_.stream == nullptr)
    {
        return frame_seen_ ? last_video_pts_ + 1.0 / 30.0 : 0.0;
    }
    return static_cast<double>(timestamp) * av_q2d(video_.stream->time_base) - pts_origin_;
}

void FfmpegPlayer::emit_video(AVFrame *frame)
{
    const int source_width = frame->width;
    const int source_height = frame->height;
    if (source_width <= 0 || source_height <= 0)
    {
        return;
    }
    // The screen is 1920x1080: a bigger picture (1440p, 4K, 8K) is scaled
    // down to it in the same pass that turns it into RGBA. The conversion,
    // the queue's memory and the texture upload then cost what a 1080p
    // stream costs, and the picture keeps the sharper source's detail.
    int width = source_width;
    int height = source_height;
    if (width > kMaxOutputWidth || height > kMaxOutputHeight)
    {
        const double scale = std::min(static_cast<double>(kMaxOutputWidth) / width,
                                      static_cast<double>(kMaxOutputHeight) / height);
        width = std::max(2, static_cast<int>(width * scale) & ~1);
        height = std::max(2, static_cast<int>(height * scale) & ~1);
    }

    source_height_.store(source_height);
    const std::size_t size =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;

    if (video_width_ != width || video_height_ != height || video_nv12_)
    {
        // A resolution change invalidates everything decoded so far (and the
        // pool: the buffers are reallocated at the new size on demand).
        video_nv12_ = false;
        log::line("ffmpeg: video resolution %dx%d -> %dx%d", video_width_, video_height_, width,
                  height);
        video_queue_.clear();
        clear_video_pool();
        exposed_buffer_ = -1;
        exposed_pts_ = kNoPts;
        picture_presented_ = false;
        video_width_ = width;
        video_height_ = height;

        // The pool is bounded by bytes, so the queue depth follows the picture
        // size: the picture on screen and the one being converted live outside
        // the queue, and the rest of the budget is this many queued pictures.
        // One line per resolution says how many and how much they take.
        const std::size_t headroom = size * kVideoPoolHeadroom;
        const std::size_t queue_budget =
            kVideoPoolBudgetBytes > headroom ? kVideoPoolBudgetBytes - headroom : 0u;
        video_queue_depth_ = std::clamp(queue_budget / size, kVideoMinQueueDepth, kVideoQueueDepth);
        log::line("ffmpeg: video queue depth %zu (%zu MB)", video_queue_depth_,
                  size * (video_queue_depth_ + kVideoPoolHeadroom) / (1024u * 1024u));
    }

    const int target = acquire_video_buffer(size);
    if (target < 0)
    {
        // The pool could not produce a width*height*4 slot (see
        // acquire_video_buffer): the picture is dropped and playback continues
        // on what is already queued. One line a second names the drop.
        note_no_memory(width, height, size);
        return;
    }

    // Failure scenario: a pool slot smaller than the frame's width*height*4
    // would let sws_scale write past it (height rows at stride width*4), so
    // the convert refuses to run unless the slot is provably big enough. The
    // pool sizes every slot from the frame itself (see acquire_video_buffer),
    // never from the stream's advertised dimensions.
    if (static_cast<std::size_t>(target) >= video_buffer_count_ ||
        video_buffers_[target].size < size)
    {
        release_video_buffer(target);
        log::line("ffmpeg: video buffer %d too small for %dx%d (defect)", target, width, height);
        error_ = "video buffer pool returned an undersized slot";
        return;
    }

    if (sws_ == nullptr || sws_src_w_ != source_width || sws_src_h_ != source_height ||
        sws_src_format_ != frame->format || sws_dst_w_ != width || sws_dst_h_ != height)
    {
        // The conversion (and the scaling down of a big picture) runs on
        // kConvertThreads threads: on one it was the frame thread's biggest
        // cost above 1080p. A build without slice threads gets one thread.
        if (sws_ != nullptr)
            sws_freeContext(sws_);
        sws_threaded_ = false;
        sws_ = sws_alloc_context();
        if (sws_ != nullptr)
        {
            av_opt_set_int(sws_, "srcw", source_width, 0);
            av_opt_set_int(sws_, "srch", source_height, 0);
            av_opt_set_int(sws_, "src_format", frame->format, 0);
            av_opt_set_int(sws_, "dstw", width, 0);
            av_opt_set_int(sws_, "dsth", height, 0);
            av_opt_set_int(sws_, "dst_format", AV_PIX_FMT_RGBA, 0);
            av_opt_set_int(sws_, "sws_flags", SWS_BILINEAR, 0);
            av_opt_set_int(sws_, "threads", convert_threads_, 0);
            if (sws_init_context(sws_, nullptr, nullptr) < 0)
            {
                sws_freeContext(sws_);
                sws_ = nullptr;
            }
            else
                sws_threaded_ = true;
        }
        if (sws_ == nullptr)
            sws_ = sws_getContext(source_width, source_height,
                                  static_cast<AVPixelFormat>(frame->format), width, height,
                                  AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws_ == nullptr)
        {
            release_video_buffer(target);
            error_ = "sws context failed";
            return;
        }
        sws_src_w_ = source_width;
        sws_src_h_ = source_height;
        sws_src_format_ = frame->format;
        sws_dst_w_ = width;
        sws_dst_h_ = height;
        log::line("ffmpeg: convert %dx%d -> %dx%d RGBA (%s)", source_width, source_height, width,
                  height, sws_threaded_ ? "threaded" : "one thread");
    }

    std::uint8_t *destination[4] = {video_buffers_[target].bytes.get(), nullptr, nullptr, nullptr};
    const int destination_stride[4] = {width * 4, 0, 0, 0};
    bool converted = false;
    if (sws_threaded_)
    {
        // sws_scale_frame is the threaded entry: the pool slot is lent to it
        // as the destination frame's buffer (freed by nobody: the pool owns it).
        if (sws_dst_ == nullptr)
            sws_dst_ = av_frame_alloc();
        AVBufferRef *slot = sws_dst_ != nullptr ? av_buffer_create(
                                                      destination[0], size,
                                                      [](void *, std::uint8_t *) {}, nullptr, 0)
                                                : nullptr;
        if (slot != nullptr)
        {
            sws_dst_->buf[0] = slot;
            sws_dst_->data[0] = destination[0];
            sws_dst_->linesize[0] = destination_stride[0];
            sws_dst_->width = width;
            sws_dst_->height = height;
            sws_dst_->format = AV_PIX_FMT_RGBA;
            converted = sws_scale_frame(sws_, sws_dst_, frame) >= 0;
            av_frame_unref(sws_dst_);
        }
    }
    if (!converted)
        sws_scale(sws_, frame->data, frame->linesize, 0, source_height, destination,
                  destination_stride);

    const double pts = frame_pts(frame);
    if (video_queue_.size() >= video_queue_depth_)
    {
        // Defensive: pump_video holds before the queue is full, but the open()
        // probe converts frames without that guard. Never grow past the bound.
        release_video_buffer(video_queue_.front().buffer);
        video_queue_.pop();
    }
    if (!video_queue_.push(QueuedPicture{target, pts}))
    {
        // Proven unreachable: the guard above leaves room in the fixed queue
        // and video_queue_depth_ never exceeds its capacity.
        release_video_buffer(target);
        return;
    }
    last_video_pts_ = pts;
    if (!frame_seen_)
        log::line("ffmpeg: first frame %dx%d pts=%.2f", width, height, pts);
    frame_seen_ = true;
}

int FfmpegPlayer::acquire_video_buffer(std::size_t size)
{
    while (video_free_count_ > 0)
    {
        const int index = video_free_buffers_[--video_free_count_];
        // Failure scenario: an index left in the free list from a pool that
        // has since shrunk (the resolution-change clear) would index past
        // video_buffers_; stale entries are dropped instead of used.
        if (index < 0 || static_cast<std::size_t>(index) >= video_buffer_count_)
        {
            continue;
        }
        VideoBuffer &slot = video_buffers_[static_cast<std::size_t>(index)];
        if (slot.size != size)
        {
            // nothrow new: an allocation that throws would std::terminate this
            // -fno-exceptions build, so a failed slot drops the picture instead.
            std::unique_ptr<std::uint8_t[]> fresh(new (std::nothrow) std::uint8_t[size]);
            if (fresh == nullptr)
            {
                video_free_buffers_[video_free_count_++] = index;
                lower_queue_depth();
                return -1;
            }
            slot.bytes = std::move(fresh);
            slot.size = size;
        }
        note_buffer_acquire(index, size);
        return index;
    }
    if (video_buffer_count_ >= kVideoPoolSlots)
    {
        // Proven unreachable: the queue holds at most video_queue_depth_
        // pictures and only two live outside it. The frame is dropped rather
        // than handed a slot past the table.
        lower_queue_depth();
        return -1;
    }
    VideoBuffer &slot = video_buffers_[video_buffer_count_];
    slot.bytes.reset(new (std::nothrow) std::uint8_t[size]);
    if (slot.bytes == nullptr)
    {
        lower_queue_depth();
        return -1;
    }
    slot.size = size;
    const int index = static_cast<int>(video_buffer_count_++);
    note_buffer_acquire(index, size);
    return index;
}

void FfmpegPlayer::release_video_buffer(int index)
{
    if (index >= 0 && static_cast<std::size_t>(index) < video_buffer_count_ &&
        video_free_count_ < kVideoPoolSlots)
    {
        video_free_buffers_[video_free_count_++] = index;
        note_buffer_release(index, video_buffers_[index].size);
    }
}

bool FfmpegPlayer::present_due_picture()
{
    // Pictures the clock has already passed are stale: one that was queued
    // before its time and is still waiting when the time has gone is dropped,
    // not shown late. The run is capped so a decoder slower than real time
    // keeps advancing the picture instead of settling on a still frame.
    // Settings > Developer > A/V sync: the picture follows the sound's clock
    // shifted by the offset (positive: the picture later).
    const double picture_clock = clock_ - av_offset_s_;
    while (!paused_ && !video_queue_.empty() && audio_active_ && clock_ > 0.0 &&
           late_drop_run_ < kMaxLateDropRun &&
           video_queue_.front().pts + kLateFrameToleranceS < picture_clock)
    {
        ++video_frames_dropped_;
        ++late_drop_run_;
        release_video_buffer(video_queue_.front().buffer);
        video_queue_.pop();
        note_video_drop();
    }
    if (video_queue_.empty())
    {
        if (hardware_active_ && !paused_ && picture_presented_)
            ++hardware_queue_empty_;
        return false;
    }

    const double pts = video_queue_.front().pts;
    // The first picture starts playback at once: there is no clock for it to
    // wait on yet. Afterwards the audio clock (or, video-only, the wall-clock
    // anchor taken below) decides when the next picture is due; while it is
    // ahead the picture is held, which is what keeps the video behind the
    // sound instead of racing ahead of it.
    const bool due = !picture_presented_ || pts <= picture_clock;
    if (!due)
    {
        return false;
    }

    if (exposed_buffer_ >= 0)
    {
        release_video_buffer(exposed_buffer_);
    }
    exposed_buffer_ = video_queue_.front().buffer;
    exposed_pts_ = pts;
    video_queue_.pop();
    picture_presented_ = true;
    late_drop_run_ = 0;

    if (!audio_active_ && !wall_anchor_)
    {
        // Video-only: the picture itself drives the clock from now on. The
        // anchor is taken when the picture is shown, not when it is decoded, so
        // a queue filled ahead of playback does not fast-forward the timeline.
        wall_anchor_ = true;
        wall_last_ = std::chrono::steady_clock::now();
        clock_ = pts;
    }
    return true;
}

void FfmpegPlayer::note_drift()
{
    if (!audio_active_ || !picture_presented_)
    {
        return;
    }
    // Positive means the picture on screen is ahead of the sound.
    const double drift = exposed_pts_ - clock_;
    if (drift > -kDriftLogS && drift < kDriftLogS)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_drift_log_ < std::chrono::seconds(1))
    {
        return;
    }
    last_drift_log_ = now;
    log::line("ffmpeg: a/v drift %d ms", static_cast<int>(drift * 1000.0));
}

void FfmpegPlayer::emit_audio(AVFrame *frame)
{
    if (swr_ == nullptr || sink_ == nullptr || frame->nb_samples <= 0)
    {
        note_audio_silence("swr, sink or decoded frame missing");
        return;
    }
    int in_rate = frame->sample_rate;
    if (in_rate <= 0 && audio_.codec != nullptr)
    {
        in_rate = audio_.codec->sample_rate;
    }
    if (in_rate <= 0)
    {
        in_rate = kOutputRate;
    }

    const std::int64_t delay = swr_get_delay(swr_, in_rate);
    const int capacity = static_cast<int>(
        av_rescale_rnd(delay + frame->nb_samples, kOutputRate, in_rate, AV_ROUND_UP));
    if (capacity <= 0)
    {
        note_audio_silence("resampler capacity is zero");
        return;
    }
    const std::size_t needed = static_cast<std::size_t>(capacity) * 2u * sizeof(float);
    if (audio_scratch_size_ < needed)
    {
        // Grown with nothrow new like the picture slots: a heap that cannot
        // spare the resampler buffer drops this batch's audio instead of
        // calling std::terminate in this -fno-exceptions build.
        std::unique_ptr<std::uint8_t[]> fresh(new (std::nothrow) std::uint8_t[needed]);
        if (fresh == nullptr)
        {
            note_audio_silence("no memory for the resampler buffer");
            return;
        }
        audio_scratch_ = std::move(fresh);
        audio_scratch_size_ = needed;
    }

    std::uint8_t *out[1] = {audio_scratch_.get()};
    const std::uint8_t *const *in =
        frame->extended_data != nullptr ? frame->extended_data : frame->data;
    const int converted = swr_convert(swr_, out, capacity, in, frame->nb_samples);
    if (converted <= 0)
    {
        note_audio_silence("resampler produced no frames");
        return;
    }
    const float *samples = reinterpret_cast<const float *>(audio_scratch_.get());
    std::size_t count = static_cast<std::size_t>(converted);
    if (audio_seek_target_ >= 0.0)
    {
        // Sound from before the seek target (the demuxer restarted at an
        // earlier keyframe) must not play: the clock already says "target".
        std::int64_t stamp = frame->best_effort_timestamp;
        if (stamp == AV_NOPTS_VALUE)
            stamp = frame->pts;
        if (stamp == AV_NOPTS_VALUE || audio_.stream == nullptr)
        {
            audio_seek_target_ = -1.0; // no timing: play it rather than guess
        }
        else
        {
            const double start =
                static_cast<double>(stamp) * av_q2d(audio_.stream->time_base) - pts_origin_;
            const double skip_s = audio_seek_target_ - start;
            if (skip_s > 0.0)
            {
                const std::size_t skip =
                    static_cast<std::size_t>(skip_s * static_cast<double>(kOutputRate));
                if (skip >= count)
                    return; // the whole frame is before the target
                samples += skip * 2u;
                count -= skip;
            }
            audio_seek_target_ = -1.0;
        }
    }
    if (tempo_graph_ != nullptr)
        push_tempo(samples, count);
    else
        push_audio(samples, count);
}

bool FfmpegPlayer::build_tempo()
{
    free_tempo();
    if (speed_ > 0.99 && speed_ < 1.01)
        return true;
    tempo_graph_ = avfilter_graph_alloc();
    if (tempo_graph_ == nullptr)
        return false;
    char args[160];
    std::snprintf(args, sizeof(args),
                  "sample_rate=%d:sample_fmt=flt:channel_layout=stereo:time_base=1/%d", kOutputRate,
                  kOutputRate);
    int result = avfilter_graph_create_filter(&tempo_src_, avfilter_get_by_name("abuffer"), "in",
                                              args, nullptr, tempo_graph_);
    if (result >= 0)
        result = avfilter_graph_create_filter(&tempo_sink_, avfilter_get_by_name("abuffersink"),
                                              "out", nullptr, nullptr, tempo_graph_);
    // atempo takes 0.5 .. 100 per stage: 0.25x is two halves.
    char chain[200];
    if (speed_ < 0.5)
        std::snprintf(chain, sizeof(chain), "atempo=0.5,atempo=%.4f", speed_ / 0.5);
    else
        std::snprintf(chain, sizeof(chain), "atempo=%.4f", speed_);
    std::strncat(chain, ",aformat=sample_fmts=flt:sample_rates=48000:channel_layouts=stereo",
                 sizeof(chain) - std::strlen(chain) - 1);
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    if (result >= 0 && outputs != nullptr && inputs != nullptr)
    {
        outputs->name = av_strdup("in");
        outputs->filter_ctx = tempo_src_;
        outputs->pad_idx = 0;
        outputs->next = nullptr;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = tempo_sink_;
        inputs->pad_idx = 0;
        inputs->next = nullptr;
        result = avfilter_graph_parse_ptr(tempo_graph_, chain, &inputs, &outputs, nullptr);
        if (result >= 0)
            result = avfilter_graph_config(tempo_graph_, nullptr);
    }
    else if (result >= 0)
        result = AVERROR(ENOMEM);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (result < 0)
    {
        log::line("ffmpeg: atempo %.2f unavailable: %s", speed_, av_error(result).c_str());
        free_tempo();
        return false;
    }
    tempo_pts_ = 0;
    log::line("ffmpeg: speed %.2fx (%s)", speed_, chain);
    return true;
}

void FfmpegPlayer::free_tempo()
{
    if (tempo_graph_ != nullptr)
        avfilter_graph_free(&tempo_graph_);
    tempo_src_ = nullptr;
    tempo_sink_ = nullptr;
}

void FfmpegPlayer::push_tempo(const float *stereo, std::size_t frames)
{
    AVFrame *in = av_frame_alloc();
    if (in == nullptr)
        return;
    in->format = AV_SAMPLE_FMT_FLT;
    in->sample_rate = kOutputRate;
    av_channel_layout_default(&in->ch_layout, 2);
    in->nb_samples = static_cast<int>(frames);
    in->pts = tempo_pts_;
    tempo_pts_ += static_cast<std::int64_t>(frames);
    if (av_frame_get_buffer(in, 0) < 0)
    {
        av_frame_free(&in);
        return;
    }
    std::memcpy(in->data[0], stereo, frames * 2 * sizeof(float));
    const int added = av_buffersrc_add_frame(tempo_src_, in); // takes the reference
    av_frame_free(&in);
    if (added < 0)
        return;
    AVFrame *out = av_frame_alloc();
    if (out == nullptr)
        return;
    while (av_buffersink_get_frame(tempo_sink_, out) >= 0)
    {
        if (out->nb_samples > 0)
            push_audio(reinterpret_cast<const float *>(out->data[0]),
                       static_cast<std::size_t>(out->nb_samples));
        av_frame_unref(out);
    }
    av_frame_free(&out);
}

void FfmpegPlayer::set_speed(double speed)
{
    speed = std::clamp(speed, 0.25, 2.0);
    if (std::abs(speed - speed_) < 0.001)
        return;
    const double at = clock_;
    speed_ = speed;
    // The buffered sound was stretched for the old speed: start the sound
    // over at the clock with the new graph (a seek drains the sink).
    if (running_)
        seek(at);
    else
        build_tempo();
}

void FfmpegPlayer::push_audio(const float *stereo, std::size_t frames)
{
    if (frames == 0)
    {
        return;
    }
    if (carry_frames_ > 0)
    {
        flush_carry();
    }

    std::size_t written = 0;
    if (sink_ != nullptr)
    {
        written = sink_->write(stereo, frames);
    }
    else
    {
        note_audio_silence("no sink to write to");
    }
    audio_frames_written_ += written;
    if (!audio_first_write_logged_)
    {
        // The one line that proves the decoded audio really reaches the sink;
        // "nothing is audible" then has to come from after this point.
        audio_first_write_logged_ = true;
        log::line("ffmpeg: audio sink write %zu/%zu frames buffered=%zu", written, frames,
                  static_cast<std::size_t>(sink_ != nullptr ? sink_->buffered() : 0u));
    }
    if (written >= frames)
    {
        return;
    }
    note_audio_silence("sink accepted only part of the decoded audio");

    const std::size_t rest = frames - written;
    if (carry_frames_ + rest > kMaxCarryFrames && carry_frames_ > 0)
    {
        std::size_t drop = carry_frames_ + rest - kMaxCarryFrames;
        if (drop > carry_frames_)
        {
            drop = carry_frames_;
        }
        std::memmove(carry_.get(), carry_.get() + drop * 2,
                     (carry_frames_ - drop) * 2 * sizeof(float));
        carry_frames_ -= drop;
    }
    const std::size_t offset = carry_frames_ * 2;
    if (carry_capacity_ < offset + rest * 2)
    {
        // Grown with nothrow new like the picture slots: out of memory drops
        // this carried audio instead of aborting the app.
        std::unique_ptr<float[]> fresh(new (std::nothrow) float[offset + rest * 2]);
        if (fresh == nullptr)
        {
            note_audio_silence("no memory for the carry buffer");
            return;
        }
        if (carry_frames_ > 0)
        {
            std::memcpy(fresh.get(), carry_.get(), carry_frames_ * 2 * sizeof(float));
        }
        carry_ = std::move(fresh);
        carry_capacity_ = offset + rest * 2;
    }
    std::memcpy(carry_.get() + offset, stereo + written * 2, rest * 2 * sizeof(float));
    carry_frames_ += rest;
}

bool FfmpegPlayer::flush_carry()
{
    if (carry_frames_ == 0)
    {
        return true;
    }
    if (sink_ == nullptr)
    {
        note_audio_silence("no sink to drain the carry buffer");
        carry_frames_ = 0;
        return true;
    }
    const std::size_t written = sink_->write(carry_.get(), carry_frames_);
    audio_frames_written_ += written;
    if (written >= carry_frames_)
    {
        carry_frames_ = 0;
        return true;
    }
    if (written > 0)
    {
        std::memmove(carry_.get(), carry_.get() + written * 2,
                     (carry_frames_ - written) * 2 * sizeof(float));
        carry_frames_ -= written;
    }
    return false;
}

void FfmpegPlayer::update_clock()
{
    const auto now = std::chrono::steady_clock::now();
    if (audio_active_ && sink_ != nullptr)
    {
        const double written = static_cast<double>(audio_frames_written_);
        const double buffered = static_cast<double>(sink_->buffered());
        double played = written - buffered;
        if (played < 0.0)
        {
            played = 0.0;
        }
        clock_ = audio_base_s_ + played * speed_ / static_cast<double>(kOutputRate);
    }
    else if (wall_anchor_)
    {
        double elapsed = std::chrono::duration<double>(now - wall_last_).count();
        if (elapsed > 0.5)
        {
            elapsed = 0.5; // a hitch must not fast-forward the timeline
        }
        if (elapsed > 0.0)
        {
            clock_ += elapsed * speed_;
        }
    }
    wall_last_ = now;
}

VideoFrameView FfmpegPlayer::poll_frame()
{
    // The one place a picture goes on screen: present_due_picture() drops what
    // the clock has passed, shows what is due and holds while the queue is
    // ahead. poll_frame() is called every rendered frame, so this runs on the
    // frame thread at display rate.
    const bool presented = present_due_picture();
    note_drift();

    VideoFrameView view;
    // Failure scenario: an exposed index that outlived a pool shrink would
    // index past video_buffers_, so the index is bounds-checked before use.
    // The view's width*height*4 is exactly the slot's size: the slot was
    // acquired for the same frame that set video_width_/video_height_, and a
    // resolution change clears the pool and resets exposed_buffer_ first.
    if (exposed_buffer_ >= 0 && static_cast<std::size_t>(exposed_buffer_) < video_buffer_count_ &&
        video_buffers_[exposed_buffer_].bytes != nullptr)
    {
        view.rgba = video_buffers_[exposed_buffer_].bytes.get();
        view.nv12 = video_nv12_;
        view.bytes = video_buffers_[exposed_buffer_].size;
        view.width = video_width_;
        view.height = video_height_;
        view.pts = exposed_pts_;
        view.fresh = presented;
    }
    return view;
}

void FfmpegPlayer::seek(double seconds)
{
    if (!running_)
    {
        return;
    }
    if (live_)
    {
        // A live broadcast plays at its edge; the playlist keeps only the
        // last few segments, so there is nowhere to seek to.
        log::line("ffmpeg: seek ignored on a live stream");
        return;
    }
    if (seconds < 0.0)
    {
        seconds = 0.0;
    }
    const std::int64_t timestamp =
        static_cast<std::int64_t>(seconds * static_cast<double>(AV_TIME_BASE));

    if (video_.format != nullptr)
    {
        avformat_seek_file(video_.format, -1, INT64_MIN, timestamp, INT64_MAX,
                           AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(video_.codec);
        if (hardware_active_)
        {
            // The hardware decoder starts over from the keyframe the seek lands on.
            std::string ignored;
            if (hardware_worker_ != nullptr)
                hardware_worker_->settle(); // no Decode may run during the reset
            hardware_wait_key_ = true;
            hardware_skipped_to_key_ = 0;
            if (hardware_ != nullptr)
                (void)hardware_->reset(&ignored);
            av_bsf_flush(hardware_bsf_);
        }
        video_.eof = false;
        video_.drained = false;
        video_.stalled_reads = 0; // a fresh start: old stalls must not end it at once
        video_.consecutive_errors = 0;
    }
    if (audio_.format != nullptr)
    {
        avformat_seek_file(audio_.format, -1, INT64_MIN, timestamp, INT64_MAX,
                           AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(audio_.codec);
        audio_.eof = false;
        audio_.drained = false;
        audio_.stalled_reads = 0; // a fresh start: old stalls must not end it at once
        audio_.consecutive_errors = 0;
    }
    if (sws_ != nullptr)
    {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if (sws_dst_ != nullptr)
        av_frame_free(&sws_dst_);
    sws_src_w_ = sws_src_h_ = sws_dst_w_ = sws_dst_h_ = 0;
    sws_src_format_ = -1;
    if (swr_ != nullptr)
    {
        swr_close(swr_);
        swr_init(swr_);
    }

    carry_frames_ = 0;
    audio_frames_written_ = 0;
    audio_base_s_ = seconds;
    build_tempo(); // a fresh graph: nothing stretched before the seek is left in it
    audio_seek_target_ = audio_.format != nullptr ? seconds : -1.0;
    video_seek_target_ = video_.format != nullptr ? seconds : -1.0;
    video_seek_skipped_ = 0;
    if (sink_ != nullptr)
    {
        sink_->drain();
    }

    std::size_t dropped = 0;
    while (!video_queue_.empty())
    {
        release_video_buffer(video_queue_.front().buffer);
        video_queue_.pop();
        ++dropped;
    }
    release_video_buffer(exposed_buffer_);
    exposed_buffer_ = -1;
    exposed_pts_ = kNoPts;
    picture_presented_ = false;
    late_drop_run_ = 0;
    // One line per seek: the clear is a risky transition and a crash right
    // after it must still be nameable in yt.log.
    log::line("ffmpeg: seek %.2fs cleared %zu queued pictures", seconds, dropped);
    last_video_pts_ = seconds;
    clock_ = seconds;
    wall_anchor_ = true;
    wall_last_ = std::chrono::steady_clock::now();
    ended_ = false;
    // A sound path given up earlier (a dead read) gets its chance again: the
    // demuxer above was moved and the range reader asks the server anew.
    if (!audio_active_ && audio_.format != nullptr && swr_ != nullptr && sink_ != nullptr)
    {
        audio_active_ = true;
        sink_->set_paused(paused_);
        log::line("ffmpeg: seek re-enabled the sound");
    }
    start_priming(); // the cut sound refills before the clock moves on
}

void FfmpegPlayer::set_paused(bool paused)
{
    if (paused == paused_)
    {
        return;
    }
    paused_ = paused;
    // The sink holds its buffered second of sound while paused; without this
    // it played on after the picture stopped and resumed out of sync.
    if (sink_ != nullptr && audio_active_)
        sink_->set_paused(paused_ || priming_);
    if (!paused_)
    {
        wall_last_ = std::chrono::steady_clock::now();
        if (audio_active_ && sink_ != nullptr)
        {
            const double written = static_cast<double>(audio_frames_written_);
            const double buffered = static_cast<double>(sink_->buffered());
            double played = written - buffered;
            if (played < 0.0)
            {
                played = 0.0;
            }
            audio_base_s_ = clock_ - played * speed_ / static_cast<double>(kOutputRate);
        }
    }
}

double FfmpegPlayer::duration_s() const
{
    if (live_)
        return 0.0; // a broadcast on the air has no end yet
    if (video_.stream != nullptr && video_.stream->duration != AV_NOPTS_VALUE)
    {
        return static_cast<double>(video_.stream->duration) * av_q2d(video_.stream->time_base);
    }
    if (video_.format != nullptr && video_.format->duration != AV_NOPTS_VALUE)
    {
        return static_cast<double>(video_.format->duration) / static_cast<double>(AV_TIME_BASE);
    }
    return 0.0;
}

double FfmpegPlayer::buffered_s() const
{
    const double ahead = last_video_pts_ - clock_;
    return ahead > 0.0 ? ahead : 0.0;
}

void FfmpegPlayer::release_io(Demuxer &demuxer)
{
    if (demuxer.io != nullptr)
    {
        av_freep(&demuxer.io->buffer);
        avio_context_free(&demuxer.io);
    }
    demuxer.range.reset();
    if (demuxer.live != nullptr)
    {
        demuxer.live->close_reader(demuxer.live_reader);
        demuxer.live = nullptr;
        demuxer.live_reader = -1;
    }
}

void FfmpegPlayer::release_demuxer(Demuxer &demuxer)
{
    if (demuxer.packet != nullptr)
    {
        av_packet_free(&demuxer.packet);
    }
    if (demuxer.frame != nullptr)
    {
        av_frame_free(&demuxer.frame);
    }
    if (demuxer.codec != nullptr)
    {
        avcodec_free_context(&demuxer.codec);
    }
    if (demuxer.format != nullptr)
    {
        avformat_close_input(&demuxer.format);
    }
    release_io(demuxer);
    demuxer.stream = nullptr;
    demuxer.stream_index = -1;
    demuxer.eof = false;
    demuxer.drained = false;
    demuxer.consecutive_errors = 0;
    demuxer.stalled_reads = 0;
    demuxer.last_error_log = std::chrono::steady_clock::time_point{};
}

void FfmpegPlayer::release()
{
    abort_.store(true);
    free_tempo();
    if (swr_ != nullptr)
    {
        swr_free(&swr_);
    }
    if (sws_ != nullptr)
    {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if (sws_dst_ != nullptr)
        av_frame_free(&sws_dst_);
    sws_src_w_ = sws_src_h_ = sws_dst_w_ = sws_dst_h_ = 0;
    sws_src_format_ = -1;
    // Null in software mode, where the session was never created.
    hardware_worker_.reset(); // before the session it decodes with
    hardware_.reset();
    hardware_active_ = false;
    if (hardware_bsf_ != nullptr)
        av_bsf_free(&hardware_bsf_);
    av_packet_free(&hardware_packet_);
    av_frame_free(&hardware_frame_);
    release_demuxer(video_);
    release_demuxer(audio_);
    // After the demuxers: their readers point into these.
    live_video_.reset();
    live_audio_.reset();
    live_ = false;
    pts_origin_ = 0.0;

    video_queue_.clear();
    clear_video_pool();
    audio_scratch_.reset();
    audio_scratch_size_ = 0;
    carry_.reset();
    carry_capacity_ = 0;
    carry_frames_ = 0;

    if (sink_ != nullptr)
    {
        // The sound already decoded must not play on after the video was
        // left (up to half a second sat in the ring).
        sink_->drain();
        sink_->set_paused(false); // the next video must not start silent
    }
    sink_ = nullptr;
    running_ = false;
    paused_ = false;
    priming_ = false;
    ended_ = false;
    audio_active_ = false;
    quality_.clear();

    exposed_buffer_ = -1;
    exposed_pts_ = kNoPts;
    picture_presented_ = false;
    last_video_pts_ = 0.0;
    video_width_ = 0;
    video_nv12_ = false;
    video_height_ = 0;
    video_queue_depth_ = kVideoQueueDepth;
    frame_seen_ = false;

    audio_frames_written_ = 0;
    audio_frames_decoded_ = 0;
    audio_base_s_ = 0.0;
    audio_first_write_logged_ = false;
    audio_silence_reason_.clear();
    last_audio_log_ = std::chrono::steady_clock::time_point{};

    video_interval_s_ = 1.0 / 30.0;
    video_frames_dropped_ = 0;
    source_height_ = 0;
    late_drop_run_ = 0;
    last_decode_log_ = std::chrono::steady_clock::time_point{};
    last_drop_log_ = std::chrono::steady_clock::time_point{};
    last_drift_log_ = std::chrono::steady_clock::time_point{};
    last_acquire_log_ = std::chrono::steady_clock::time_point{};
    last_release_log_ = std::chrono::steady_clock::time_point{};
    last_nomem_log_ = std::chrono::steady_clock::time_point{};

    clock_ = 0.0;
    wall_anchor_ = false;

    abort_.store(false);
    open_deadline_us_.store(0);
    update_deadline_us_.store(0);
    last_budget_log_ = std::chrono::steady_clock::time_point{};
}

} // namespace

std::unique_ptr<Player> make_player()
{
    return std::unique_ptr<Player>(new FfmpegPlayer());
}

} // namespace yt

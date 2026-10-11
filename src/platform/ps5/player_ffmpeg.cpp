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
// bounded number of packets, converts whichever frames came out and hands the
// newest picture to poll_frame(). Audio goes straight to the AudioSink, which
// owns the audio thread. No threads are created here; every stage logs one
// "ffmpeg: ..." line so yt.log shows which step failed or is too slow, and
// open()/update() failures are also readable through error().

#include "yt/log.hpp"
#include "yt/player.hpp"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace yt
{

namespace
{

// Output format of the audio path: 48 kHz stereo float, the sink's contract.
constexpr int kOutputRate = 48000;
constexpr int kDecoderThreads = 4;
constexpr int kVideoPacketsPerUpdate = 4;
constexpr int kAudioPacketsPerUpdate = 8;
// Stop feeding the sink once it holds half a second; keeps latency low.
constexpr int kAudioCushionFrames = kOutputRate / 2;
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
// Packets an AV1 decoder may consume during open() without emitting a frame;
// after that the open fails so the app falls back to the next format instead
// of showing a black screen.
constexpr int kAv1ProbePackets = 8;
// A video frame this far behind the audio clock is stale: converting and
// queueing it cannot catch up, so it is dropped.
constexpr double kLateFrameToleranceS = 0.10;
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

// One demuxer/decoder pair. Video and audio each own one of these.
struct Demuxer
{
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
    double buffered_s() const override;
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

    bool open_demuxer(Demuxer &demuxer, const std::string &url, bool video);
    bool init_audio_resampler();
    std::string make_quality_label() const;

    void pump_video(int budget);
    void pump_audio(int budget);
    void emit_video(AVFrame *frame);
    void emit_audio(AVFrame *frame);
    double frame_pts(const AVFrame *frame) const;

    void push_audio(const float *stereo, std::size_t frames);
    bool flush_carry();
    void update_clock();

    // Decodes up to `packets` of the video stream during open() to prove the
    // decoder really produces pictures; stops at the first frame it emits.
    void probe_video_frames(int packets);
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
    std::string error_;
    std::string quality_;

    SwsContext *sws_ = nullptr;
    SwrContext *swr_ = nullptr;

    // Double-buffered RGBA pictures. exposed_ is the buffer last handed to the
    // caller, pending_ the one holding the newest decoded frame.
    std::vector<std::uint8_t> frames_[2];
    int exposed_ = -1;
    int pending_ = -1;
    bool pending_valid_ = false;
    double pending_pts_ = 0.0;
    double returned_pts_ = kNoPts;
    double last_video_pts_ = 0.0;
    int video_width_ = 0;
    int video_height_ = 0;
    bool frame_seen_ = false;

    std::vector<std::uint8_t> audio_scratch_;
    std::vector<float> carry_;
    std::size_t carry_frames_ = 0;
    std::uint64_t audio_frames_written_ = 0;
    std::uint64_t audio_frames_decoded_ = 0;
    double audio_base_s_ = 0.0;
    bool audio_first_write_logged_ = false;
    std::string audio_silence_reason_;
    std::chrono::steady_clock::time_point last_audio_log_{};

    double video_interval_s_ = 1.0 / 30.0;
    std::uint64_t video_frames_dropped_ = 0;
    int late_drop_run_ = 0;
    // The decode-slow and drop lines are throttled separately: they describe
    // different things and must be able to appear in the same second.
    std::chrono::steady_clock::time_point last_decode_log_{};
    std::chrono::steady_clock::time_point last_drop_log_{};

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

    if (!open_demuxer(video_, video_url, true))
        return fail(error_);
    quality_ = make_quality_label();

    if (video_.stream->codecpar->codec_id == AV_CODEC_ID_AV1)
    {
        // A decoder that opens but never emits a picture is a black screen the
        // viewer cannot recover from; probing the first packets turns that into
        // a precise open failure so the app falls back to the next format.
        probe_video_frames(kAv1ProbePackets);
        if (!frame_seen_)
        {
            return fail(error_.empty() ? "av1: decoder produced no frames" : error_);
        }
    }

    if (audio_url.empty())
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
        if (!open_demuxer(audio_, audio_url, false))
            return fail(error_);
        if (!init_audio_resampler())
            return fail(error_);
        audio_active_ = true;
        log::line("ffmpeg: audio active sink=%p", static_cast<const void *>(sink));
    }

    open_deadline_us_.store(0);
    running_ = true;
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

    AVDictionary *options = nullptr;
    av_dict_set(&options, "timeout", kNetworkTimeout, 0);
    av_dict_set(&options, "rw_timeout", kNetworkTimeout, 0);
    av_dict_set(&options, "reconnect", "1", 0);
    log::line("ffmpeg: open %s url=%s", video ? "video" : "audio", url.substr(0, 64).c_str());
    int result = avformat_open_input(&format, url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (result < 0)
    {
        error_ = "avformat_open_input: " + av_error(result);
        avformat_close_input(&format);
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
    demuxer.codec->thread_count = kDecoderThreads;
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
// the frames it needs to prove that the decoder works at all.
void FfmpegPlayer::probe_video_frames(int packets)
{
    int sent = 0;
    while (sent < packets && !video_.drained && !frame_seen_)
    {
        if (abort_.load() || now_us() >= open_deadline_us_.load())
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
            // misleading "decoder produced no frames". End of stream before a
            // picture keeps that wording, because there the decoder really
            // produced nothing, and a cut by the open deadline is reported as
            // the open timeout it is.
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
              static_cast<unsigned long long>(video_frames_dropped_));
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

void FfmpegPlayer::pump_video(int budget)
{
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
        const std::int64_t step_start_us = now_us();
        const int received = avcodec_receive_frame(video_.codec, video_.frame);
        if (received == 0)
        {
            video_.consecutive_errors = 0;
            // A picture the audio clock has already passed cannot be caught up
            // with: converting and queueing it would only add latency, so it is
            // dropped and the decoder goes straight back for the next packet.
            // The run is capped (see kMaxLateDropRun) and the first frame is
            // always shown, so a black screen and a frozen picture are both
            // ruled out.
            const double pts = frame_pts(video_.frame);
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
            late_drop_run_ = 0;
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
            audio_.drained = true;
            // The audio clock cannot be the master clock of a path that is
            // gone: the picture continues under the wall-clock fallback that
            // video-only playback already uses.
            audio_active_ = false;
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
    return static_cast<double>(timestamp) * av_q2d(video_.stream->time_base);
}

void FfmpegPlayer::emit_video(AVFrame *frame)
{
    const int width = frame->width;
    const int height = frame->height;
    if (width <= 0 || height <= 0)
    {
        return;
    }

    int target = exposed_ == 0 ? 1 : 0;
    if (video_width_ != width || video_height_ != height)
    {
        frames_[0].clear();
        frames_[1].clear();
        exposed_ = -1;
        pending_ = -1;
        pending_valid_ = false;
        video_width_ = width;
        video_height_ = height;
        target = 0;
    }

    const std::size_t size =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    if (frames_[target].size() != size)
    {
        frames_[target].assign(size, 0u);
    }

    sws_ =
        sws_getCachedContext(sws_, width, height, static_cast<AVPixelFormat>(frame->format), width,
                             height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (sws_ == nullptr)
    {
        error_ = "sws_getCachedContext failed";
        return;
    }

    std::uint8_t *destination[4] = {frames_[target].data(), nullptr, nullptr, nullptr};
    const int destination_stride[4] = {width * 4, 0, 0, 0};
    sws_scale(sws_, frame->data, frame->linesize, 0, height, destination, destination_stride);

    const double pts = frame_pts(frame);
    pending_ = target;
    pending_pts_ = pts;
    pending_valid_ = true;
    last_video_pts_ = pts;
    if (!frame_seen_)
        log::line("ffmpeg: first frame %dx%d pts=%.2f", width, height, pts);
    frame_seen_ = true;

    if (!audio_active_ && !wall_anchor_)
    {
        wall_anchor_ = true;
        clock_ = pts;
        wall_last_ = std::chrono::steady_clock::now();
    }
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
    if (audio_scratch_.size() < needed)
    {
        audio_scratch_.resize(needed);
    }

    std::uint8_t *out[1] = {audio_scratch_.data()};
    const std::uint8_t *const *in =
        frame->extended_data != nullptr ? frame->extended_data : frame->data;
    const int converted = swr_convert(swr_, out, capacity, in, frame->nb_samples);
    if (converted <= 0)
    {
        note_audio_silence("resampler produced no frames");
        return;
    }
    push_audio(reinterpret_cast<const float *>(audio_scratch_.data()),
               static_cast<std::size_t>(converted));
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
        std::memmove(carry_.data(), carry_.data() + drop * 2,
                     (carry_frames_ - drop) * 2 * sizeof(float));
        carry_frames_ -= drop;
    }
    const std::size_t offset = carry_frames_ * 2;
    carry_.resize(offset + rest * 2);
    std::memcpy(carry_.data() + offset, stereo + written * 2, rest * 2 * sizeof(float));
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
    const std::size_t written = sink_->write(carry_.data(), carry_frames_);
    audio_frames_written_ += written;
    if (written >= carry_frames_)
    {
        carry_frames_ = 0;
        return true;
    }
    if (written > 0)
    {
        std::memmove(carry_.data(), carry_.data() + written * 2,
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
        clock_ = audio_base_s_ + played / static_cast<double>(kOutputRate);
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
            clock_ += elapsed;
        }
    }
    wall_last_ = now;
}

VideoFrameView FfmpegPlayer::poll_frame()
{
    VideoFrameView view;
    if (pending_valid_)
    {
        pending_valid_ = false;
        if (pending_pts_ > returned_pts_ && pending_ >= 0 && !frames_[pending_].empty())
        {
            exposed_ = pending_;
            returned_pts_ = pending_pts_;
            view.rgba = frames_[exposed_].data();
            view.width = video_width_;
            view.height = video_height_;
            view.pts = pending_pts_;
            view.fresh = true;
            return view;
        }
    }
    if (exposed_ >= 0 && !frames_[exposed_].empty())
    {
        view.rgba = frames_[exposed_].data();
        view.width = video_width_;
        view.height = video_height_;
        view.pts = returned_pts_;
        view.fresh = false;
    }
    return view;
}

void FfmpegPlayer::seek(double seconds)
{
    if (!running_)
    {
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
        video_.eof = false;
        video_.drained = false;
    }
    if (audio_.format != nullptr)
    {
        avformat_seek_file(audio_.format, -1, INT64_MIN, timestamp, INT64_MAX,
                           AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(audio_.codec);
        audio_.eof = false;
        audio_.drained = false;
    }
    if (sws_ != nullptr)
    {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if (swr_ != nullptr)
    {
        swr_close(swr_);
        swr_init(swr_);
    }

    carry_frames_ = 0;
    audio_frames_written_ = 0;
    audio_base_s_ = seconds;
    if (sink_ != nullptr)
    {
        sink_->drain();
    }

    pending_ = -1;
    pending_valid_ = false;
    returned_pts_ = kNoPts;
    last_video_pts_ = seconds;
    clock_ = seconds;
    wall_anchor_ = true;
    wall_last_ = std::chrono::steady_clock::now();
    ended_ = false;
}

void FfmpegPlayer::set_paused(bool paused)
{
    if (paused == paused_)
    {
        return;
    }
    paused_ = paused;
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
            audio_base_s_ = clock_ - played / static_cast<double>(kOutputRate);
        }
    }
}

double FfmpegPlayer::duration_s() const
{
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
    if (swr_ != nullptr)
    {
        swr_free(&swr_);
    }
    if (sws_ != nullptr)
    {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    release_demuxer(video_);
    release_demuxer(audio_);

    frames_[0].clear();
    frames_[1].clear();
    audio_scratch_.clear();
    carry_.clear();
    carry_frames_ = 0;

    sink_ = nullptr;
    running_ = false;
    paused_ = false;
    ended_ = false;
    audio_active_ = false;
    quality_.clear();

    exposed_ = -1;
    pending_ = -1;
    pending_valid_ = false;
    pending_pts_ = 0.0;
    returned_pts_ = kNoPts;
    last_video_pts_ = 0.0;
    video_width_ = 0;
    video_height_ = 0;
    frame_seen_ = false;

    audio_frames_written_ = 0;
    audio_frames_decoded_ = 0;
    audio_base_s_ = 0.0;
    audio_first_write_logged_ = false;
    audio_silence_reason_.clear();
    last_audio_log_ = std::chrono::steady_clock::time_point{};

    video_interval_s_ = 1.0 / 30.0;
    video_frames_dropped_ = 0;
    late_drop_run_ = 0;
    last_decode_log_ = std::chrono::steady_clock::time_point{};
    last_drop_log_ = std::chrono::steady_clock::time_point{};

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

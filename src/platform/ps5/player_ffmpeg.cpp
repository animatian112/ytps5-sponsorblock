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
// owns the audio thread. No threads are created here, and nothing is logged:
// the caller reads error().

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
    double audio_base_s_ = 0.0;

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
    if (update_deadline != 0 && now >= update_deadline)
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

    if (!audio_url.empty() && sink != nullptr)
    {
        if (!open_demuxer(audio_, audio_url, false))
            return fail(error_);
        if (!init_audio_resampler())
            return fail(error_);
        audio_active_ = true;
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
        error_ = "no decoder for stream";
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
    int result = swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_FLT, kOutputRate, &in_layout,
                                     audio_.codec->sample_fmt, in_rate, 0, nullptr);
    if (result < 0 || swr_ == nullptr)
    {
        error_ = "swr_alloc_set_opts2: " + av_error(result < 0 ? result : AVERROR(ENOMEM));
        return false;
    }
    result = swr_init(swr_);
    if (result < 0)
    {
        error_ = "swr_init: " + av_error(result);
        return false;
    }
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
        const int received = avcodec_receive_frame(video_.codec, video_.frame);
        if (received == 0)
        {
            emit_video(video_.frame);
            av_frame_unref(video_.frame);
            ++work;
            continue;
        }
        if (received == AVERROR_EOF)
        {
            video_.drained = true;
            break;
        }
        if (received != AVERROR(EAGAIN))
        {
            video_.drained = true;
            break;
        }
        if (video_.eof)
        {
            // Flushed already and still no frame: the stream is spent.
            video_.drained = true;
            break;
        }
        const int read = av_read_frame(video_.format, video_.packet);
        if (read == AVERROR(EAGAIN))
        {
            break;
        }
        if (out_of_time())
        {
            // The budget stopped the read: leave the stream open and try again
            // next frame, not mistake the abort for the end.
            return;
        }
        if (read < 0)
        {
            video_.eof = true;
            avcodec_send_packet(video_.codec, nullptr);
            ++work;
            continue;
        }
        if (video_.packet->stream_index == video_.stream_index)
        {
            const int sent = avcodec_send_packet(video_.codec, video_.packet);
            if (sent < 0 && sent != AVERROR(EAGAIN))
            {
                video_.eof = true;
                avcodec_send_packet(video_.codec, nullptr);
            }
        }
        av_packet_unref(video_.packet);
        ++work;
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
            emit_audio(audio_.frame);
            av_frame_unref(audio_.frame);
            ++work;
            continue;
        }
        if (received == AVERROR_EOF)
        {
            audio_.drained = true;
            break;
        }
        if (received != AVERROR(EAGAIN))
        {
            audio_.drained = true;
            break;
        }
        if (audio_.eof)
        {
            audio_.drained = true;
            break;
        }
        const int read = av_read_frame(audio_.format, audio_.packet);
        if (read == AVERROR(EAGAIN))
        {
            break;
        }
        if (out_of_time())
        {
            // The budget stopped the read: leave the stream open and try again
            // next frame, not mistake the abort for the end.
            return;
        }
        if (read < 0)
        {
            audio_.eof = true;
            avcodec_send_packet(audio_.codec, nullptr);
            ++work;
            continue;
        }
        if (audio_.packet->stream_index == audio_.stream_index)
        {
            const int sent = avcodec_send_packet(audio_.codec, audio_.packet);
            if (sent < 0 && sent != AVERROR(EAGAIN))
            {
                audio_.eof = true;
                avcodec_send_packet(audio_.codec, nullptr);
            }
        }
        av_packet_unref(audio_.packet);
        ++work;
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
    if (converted > 0)
    {
        push_audio(reinterpret_cast<const float *>(audio_scratch_.data()),
                   static_cast<std::size_t>(converted));
    }
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
    audio_frames_written_ += written;
    if (written >= frames)
    {
        return;
    }

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
    audio_base_s_ = 0.0;
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

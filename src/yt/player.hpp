// youtube-tv - Video playback: demux, decode, audio and the master clock.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The interface is platform-neutral; the implementation is not:
//   src/platform/ps5/player_ffmpeg.cpp  - PacBrew ffmpeg on the console
//   host/player_stub.cpp                - PC snapshots: reports no video
// Both sides keep the same call pattern: set_decode_mode() once, open() once,
// update() every frame from the frame loop, poll_frame() while the frame is
// drained, close() when the player screen is left.

#pragma once

#include "yt/types.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace yt
{

// Decoded audio goes to whatever started the audio thread before startup.
class AudioSink
{
  public:
    virtual ~AudioSink() = default;
    // Interleaved stereo float frames at 48 kHz; returns how many fit.
    virtual std::size_t write(const float *stereo, std::size_t frames) = 0;
    // How many frames are buffered but not yet played (the decoder's cushion).
    virtual std::size_t buffered() const = 0;
    virtual void drain() = 0;
    // Stops (and resumes) playing what is buffered, keeping it for the resume.
    virtual void set_paused(bool paused)
    {
        (void)paused;
    }
};

// One frame, RGBA8, owned by the player; valid until the next poll_frame().
struct VideoFrameView
{
    const std::uint8_t *rgba = nullptr;
    int width = 0;
    int height = 0;
    double pts = 0.0; // presentation time, seconds
    bool fresh = false;
};

class Player
{
  public:
    virtual ~Player() = default;

    // Selects how the next open() decodes video. The app calls this once per
    // open(); the console player keeps it and falls back to software whenever
    // the hardware path cannot deliver a picture, so it never has to be exact.
    virtual void set_decode_mode(DecodeMode mode) = 0;

    // Opens the video-only and audio-only URLs of one adaptive pair. A missing
    // audio URL is allowed (video-only playback). Returns false on failure and
    // fills error(); open() may be called again afterwards.
    virtual bool open(const std::string &video_url, const std::string &audio_url,
                      AudioSink *sink) = 0;
    virtual void close() = 0;
    virtual bool running() const = 0;
    virtual double duration_s() const = 0;
    // The video's codec and height, for the player overlay ("1080p VP9").
    virtual std::string quality_label() const = 0;

    // Pulls demuxed packets, decodes, feeds the audio sink and the bounded
    // picture queue. Call once per frame; does bounded work so a frame never
    // stalls.
    virtual void update() = 0;
    // The frame due against the clock, or fresh = false while the picture is
    // held (nothing new was due yet). The buffer stays valid until the next
    // poll_frame().
    virtual VideoFrameView poll_frame() = 0;

    // Master clock: audio playback position in seconds.
    virtual double clock_s() const = 0;
    virtual void seek(double seconds) = 0;
    virtual void set_paused(bool paused) = 0;
    virtual bool paused() const = 0;
    // Playback speed, 0.25 .. 2; the sound keeps its pitch.
    virtual void set_speed(double speed)
    {
        (void)speed;
    }
    virtual double speed() const
    {
        return 1.0;
    }
    // Seconds of undecoded video ahead of the clock (0 while buffering).
    virtual double buffered_s() const = 0;
    virtual bool ended() const = 0;
    virtual const std::string &error() const = 0;
    // How the decoder keeps up: frames dropped for arriving late so far, and
    // the decoded picture's own height (before any scaling to the screen).
    virtual std::uint64_t dropped_frames() const
    {
        return 0;
    }
    virtual int source_height() const
    {
        return 0;
    }
};

// Defined per platform.
std::unique_ptr<Player> make_player();

} // namespace yt

// youtube-tv - PC player stub: snapshots and unit tests have no video.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console implementation of yt::make_player() lives in
// src/platform/ps5/player_ffmpeg.cpp, which the host build does not compile.
// This stub keeps the same call pattern valid: open() fails with a clear
// message, everything else reports an idle, neutral player.

#include "yt/player.hpp"

namespace yt
{

namespace
{

constexpr const char *kNoVideo = "video requires the console build";

class StubPlayer final : public Player
{
  public:
    void set_decode_mode(DecodeMode mode) override
    {
        // Stored only: there is no video on the host, and open() reports that.
        decode_mode_ = mode;
    }

    bool open(const std::string &, const std::string &, AudioSink *) override
    {
        return false;
    }
    void close() override
    {
    }

    bool running() const override
    {
        return false;
    }
    double duration_s() const override
    {
        return 0.0;
    }
    std::string quality_label() const override
    {
        return std::string();
    }

    void update() override
    {
    }
    VideoFrameView poll_frame() override
    {
        return VideoFrameView{};
    }

    double clock_s() const override
    {
        return 0.0;
    }
    void seek(double) override
    {
    }
    void set_paused(bool) override
    {
    }
    bool paused() const override
    {
        return false;
    }
    double buffered_s() const override
    {
        return 0.0;
    }
    bool ended() const override
    {
        return false;
    }
    const std::string &error() const override
    {
        return error_;
    }

  private:
    std::string error_ = kNoVideo;
    DecodeMode decode_mode_ = DecodeMode::auto_detect;
};

} // namespace

std::unique_ptr<Player> make_player()
{
    return std::unique_ptr<Player>(new StubPlayer());
}

} // namespace yt

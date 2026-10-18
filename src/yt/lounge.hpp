// youtube-tv - Cast from the phone: the YouTube "Link with TV code" screen.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SmartTube's remote control, the way YouTube's TV apps do it: the console
// registers as a lounge screen (www.youtube.com/api/lounge), shows a TV code
// the phone's YouTube app links with (Settings > Watch on TV > Link with TV
// code), then keeps a long poll open for the phone's commands (play this
// video, pause, seek, next...) and tells the phone what is playing.
// Everything network-side runs on worker threads; the frame only takes
// commands and reports the player's state.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace yt
{

// Takes the complete "length\n<json>" pieces off the front of a lounge
// stream (lengths count UTF-16 units) and leaves a partial one in buffer.
std::vector<std::string> take_lounge_chunks(std::string &buffer);

class Lounge
{
  public:
    struct Command
    {
        enum class Kind : std::uint8_t
        {
            play_video, // video_id (from time_s), playlist in `videos`
            play,
            pause,
            seek, // time_s
            stop,
            next,
            previous,
            volume, // volume 0..100
        };
        Kind kind = Kind::play;
        std::string video_id;
        double time_s = 0.0;
        std::vector<std::string> videos;
        int volume = 100;
    };
    struct Status
    {
        std::string code; // "123 456 789 012", empty while it is being fetched
        bool connected = false;
        std::string remote; // the phone's name when one is linked
        std::string error;
    };

    // data_root keeps the screen's id, so a linked phone stays linked.
    Lounge(std::string data_root, std::string screen_name);
    ~Lounge();
    Lounge(const Lounge &) = delete;
    Lounge &operator=(const Lounge &) = delete;

    void start();
    void stop();
    bool running() const;
    // A fresh TV code.
    void new_code();
    Status status() const;
    std::vector<Command> take_commands();
    // What the player is doing: state 1 playing, 2 paused, 3 buffering,
    // 0 ended, -1 nothing. Cheap; the worker sends it when it changed.
    void report(const std::string &video_id, double position_s, double duration_s, int state);

    struct Impl; // public so the worker threads can hold it

  private:
    std::shared_ptr<Impl> impl_;
};

} // namespace yt

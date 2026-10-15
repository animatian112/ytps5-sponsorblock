// youtube-tv - The player's controls in SmartTube's layout.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hidden while a video plays. X pauses and brings them up, Left/Right seek
// 10 s, any other button shows them. On screen: the title and channel at the
// lower left, a full-width seek bar with the SponsorBlock segments painted on
// it, and a row of round buttons under it (restart, -10 s, play/pause, +10 s,
// next video; "Skip <segment>" on the right while one plays). Down from the
// buttons hands the focus to the suggestions row, which slides the whole
// block up; Circle hides the controls before it leaves the player.

#pragma once

#include "core/input.hpp"
#include "ui/components.hpp"

#include <string>
#include <vector>

namespace hui::app
{

class PlayerChrome
{
  public:
    enum class Command
    {
        none,
        toggle_pause,
        seek,         // to seek_target()
        restart,      // to 0
        next,         // the first suggestion
        skip_segment, // the SponsorBlock segment under the playhead
        suggestions,  // Down from the buttons: the screen moves the focus to the row
    };

    void set_times(double position, double duration, double buffered);
    void set_playing(bool playing);
    void set_ranges(std::vector<ui::MediaRange> ranges);
    void set_info(std::string title, std::string subtitle, std::string quality);
    void set_has_next(bool has_next);
    // The label of the segment under the playhead ("Sponsor"), or empty.
    void set_segment(std::string label);

    // Reads the frame. `back` is consumed (hides the controls) when they are
    // on screen; the screen sees Command::none and leaves on the next Circle.
    Command handle(const InputFrame &input, ui::Feedback &feedback, bool *consumed_back);
    double seek_target() const
    {
        return seek_target_;
    }

    void show();
    void hide();
    bool shown() const
    {
        return shown_;
    }
    // 0 hidden .. 1 on screen, animated.
    float visibility() const
    {
        return visibility_;
    }
    // 0 .. 1: how far the block has slid up for the suggestions row.
    void set_lifted(bool lifted)
    {
        lifted_ = lifted;
    }
    float lift() const
    {
        return lift_;
    }

    void update(float dt);
    // Draws scrim, text, seek bar and buttons, moved up by `rise` pixels.
    void draw(ui::Canvas &canvas, const ui::Fonts &fonts, float rise) const;

  private:
    enum class Button
    {
        restart,
        rewind,
        play,
        forward,
        next,
        skip,
    };
    enum class Zone
    {
        seek,
        buttons,
    };

    std::vector<Button> buttons() const;
    void wake();
    Command seek_by(double seconds);
    Command press(Button button);

    double position_ = 0.0;
    double duration_ = 0.0;
    double buffered_ = 0.0;
    bool playing_ = true;
    std::vector<ui::MediaRange> ranges_;
    std::string title_;
    std::string subtitle_;
    std::string quality_;
    std::string segment_;
    bool has_next_ = false;

    bool shown_ = true;
    float visibility_ = 1.0f;
    float idle_ = 0.0f;
    bool lifted_ = false;
    float lift_ = 0.0f;
    Zone zone_ = Zone::buttons;
    Button focus_ = Button::play;
    double seek_target_ = 0.0;
    float seek_hold_ = 0.0f; // shows the target, not the clock, just after a seek
};

} // namespace hui::app

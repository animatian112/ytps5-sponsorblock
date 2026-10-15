// youtube-tv - The player's controls in SmartTube's layout.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hidden while a video plays. X pauses and brings them up, Left/Right seek
// 10 s, any other button shows them. On screen: the title and channel at the
// lower left, a full-width seek bar with the SponsorBlock segments painted on
// it, and a row of round buttons under it: restart, -10 s, play/pause, +10 s,
// next video on the left; "Skip <segment>", subtitles, speed, quality and the
// sound track on the right. Those four open a panel on the right edge with
// their choices. Down from the buttons hands the focus to the suggestions
// row, which slides the whole block up; Circle closes a panel, then hides the
// controls, before it leaves the player.

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
        next,         // the next suggestion
        skip_segment, // the SponsorBlock segment under the playhead
        suggestions,  // Down from the buttons: the screen moves the focus to the row
        open_captions,
        open_speed,
        open_quality,
        open_audio,
        pick, // a panel choice: panel() and picked()
    };
    enum class Panel
    {
        none,
        captions,
        speed,
        quality,
        audio,
    };

    void set_times(double position, double duration, double buffered);
    void set_playing(bool playing);
    void set_ranges(std::vector<ui::MediaRange> ranges);
    void set_info(std::string title, std::string subtitle, std::string quality);
    void set_has_next(bool has_next);
    // The label of the segment under the playhead ("Sponsor"), or empty.
    void set_segment(std::string label);
    // Labels inside the right-hand buttons: "CC" lit when subtitles are on,
    // the speed ("1.5x"), the quality ("1080p"); the sound-track button only
    // shows when the video has more than one track.
    void set_badges(bool captions_on, std::string speed, std::string quality, bool audio_tracks);

    // Reads the frame. `back` is consumed (closes the panel, then hides the
    // controls) when they are on screen; the screen leaves on the next Circle.
    Command handle(const InputFrame &input, ui::Feedback &feedback, bool *consumed_back);
    double seek_target() const
    {
        return seek_target_;
    }

    // The choice panel: the screen opens it in answer to open_*.
    void open_panel(Panel panel, std::string title, std::vector<std::string> options, int selected);
    void close_panel();
    Panel panel() const
    {
        return panel_;
    }
    int picked() const
    {
        return picked_;
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
    void set_lifted(bool lifted)
    {
        lifted_ = lifted;
    }
    // 0 .. 1: how far the block has slid up for the suggestions row.
    float lift() const
    {
        return lift_;
    }

    void update(float dt);
    // Draws scrim, text, seek bar, buttons and the panel, the block moved up
    // by `rise` pixels.
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
        captions,
        speed,
        quality,
        audio,
    };
    enum class Zone
    {
        seek,
        buttons,
    };

    std::vector<Button> buttons() const;
    static bool right_side(Button button);
    float button_width(const ui::Fonts &fonts, Button button) const;
    std::string button_label(Button button) const;
    void wake();
    Command seek_by(double seconds);
    Command press(Button button);
    Command handle_panel(const InputFrame &input, ui::Feedback &feedback, bool *consumed_back);
    void draw_panel(ui::Canvas &canvas, const ui::Fonts &fonts) const;

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
    bool captions_on_ = false;
    std::string speed_label_ = "1x";
    std::string quality_label_;
    bool audio_tracks_ = false;

    bool shown_ = true;
    float visibility_ = 1.0f;
    float idle_ = 0.0f;
    bool lifted_ = false;
    float lift_ = 0.0f;
    Zone zone_ = Zone::buttons;
    Button focus_ = Button::play;
    double seek_target_ = 0.0;
    float seek_hold_ = 0.0f; // shows the target, not the clock, just after a seek

    Panel panel_ = Panel::none;
    std::string panel_title_;
    std::vector<std::string> panel_options_;
    int panel_selected_ = -1;
    int panel_focus_ = 0;
    int picked_ = -1;
    float panel_open_ = 0.0f; // animated 0..1
};

} // namespace hui::app

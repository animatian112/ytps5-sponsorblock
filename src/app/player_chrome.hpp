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
#include "yt/types.hpp"
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
        like,
        dislike,
        subscribe,
        channel,
        save, // add to playlist
        open_info,
        open_chapters,
        cycle_mode, // the playback mode button: next, shuffle, repeat, pause, close
        pick,       // a panel choice: panel() and picked()
        open_comments,
        open_sleep,
        open_dim,
    };
    enum class Panel
    {
        none,
        captions,
        speed,
        quality,
        audio,
        info,
        chapters,
        comments,
        sleep,
        dim,
    };

    // The video's chapters: marks on the seek bar, the current one's name
    // above it, a Chapters button.
    // The channel's picture (a texture, 0: the initial in a circle).
    void set_channel_avatar(std::uint32_t texture)
    {
        avatar_ = texture;
    }
    // Settings::playback_mode, for the mode button's icon.
    void set_mode(int mode)
    {
        mode_ = mode;
    }
    void set_chapters(std::vector<yt::Chapter> chapters)
    {
        chapters_ = std::move(chapters);
    }
    // A seek the player pressed left/right for, once the presses stopped
    // (SmartTube's seek: the bar moves at once, the video follows when the
    // target settles, so ten presses cost one seek). True once per seek.
    bool take_seek()
    {
        const bool ready = seek_ready_;
        seek_ready_ = false;
        return ready;
    }

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
    // The account buttons (signed in only): like, dislike and subscribe, lit
    // for the standing YouTube reports.
    void set_account(bool signed_in, bool liked, bool disliked, bool subscribed);
    // The channel button opens the video's channel page.
    void set_has_channel(bool has_channel)
    {
        has_channel_ = has_channel;
    }
    // The clock line on the right ("Fri Oct 16  10:34 AM").
    // SmartTube's player settings.
    struct Options
    {
        double seek_step = 10.0; // seconds per left/right on the seek bar
        float hide_after = 4.0f; // seconds without input while playing (0: never)
        bool clock = true;       // the clock at the right of the top row
        bool remaining = true;   // "Remaining: 4:21"
        bool quality = true;     // "1080p" at the right of the top row
        bool tooltips = true;    // the focused button's name
        int ok_behavior = 0;     // Cross while hidden: 0 controls, 1 pause, 2 both
    };
    void set_options(const Options &options)
    {
        options_ = options;
    }
    void set_clock(std::string clock)
    {
        clock_ = std::move(clock);
    }

    // Reads the frame. `back` is consumed (closes the panel, then hides the
    // controls) when they are on screen; the screen leaves on the next Circle.
    Command handle(const InputFrame &input, ui::Feedback &feedback, bool *consumed_back);
    double seek_target() const
    {
        return seek_target_;
    }
    // True while the player moves the seek target (the preview shows), and
    // the time the bar points at.
    bool previewing() const
    {
        return shown_ && (seek_pending_ > 0.0f || (zone_ == Zone::seek && seek_hold_ > 0.0f));
    }
    double preview_time() const
    {
        return seek_target_;
    }
    // The storyboard frame of preview_time(): a texture and its rectangle.
    void set_preview(std::uint32_t texture, const gfx::Rect &uv, float aspect)
    {
        preview_texture_ = texture;
        preview_uv_ = uv;
        preview_aspect_ = aspect;
    }

    // The choice panel: the screen opens it in answer to open_*.
    void open_panel(Panel panel, std::string title, std::vector<std::string> options, int selected);
    void close_panel();
    Panel panel() const
    {
        return panel_;
    }
    // A reading panel's text arrived or grew (comments): the focus stays.
    void set_panel_options(std::vector<std::string> options);
    int panel_focus() const
    {
        return panel_focus_;
    }
    int panel_count() const
    {
        return static_cast<int>(panel_options_.size());
    }
    // A sleep timer is set: its button is lit.
    void set_sleep_on(bool on)
    {
        sleep_on_ = on;
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
        like,
        dislike,
        subscribe,
        channel,
        save,
        info,
        chapters,
        mode,
        comments,
        sleep,
        dim,
    };
    enum class Zone
    {
        top,
        seek,
        bottom,
    };

    // SmartTube's two rows: transport above the seek bar, the video's own
    // buttons below it.
    std::vector<Button> top_buttons() const;
    std::vector<Button> bottom_buttons() const;
    std::string tooltip(Button button) const;
    bool lit(Button button) const;
    void draw_icon(gfx::DrawList &list, const ui::Fonts &fonts, Button button, float cx, float cy,
                   gfx::Color ink) const;
    void wake();
    Command seek_by(double seconds);
    Command press(Button button);
    Command handle_panel(const InputFrame &input, ui::Feedback &feedback, bool *consumed_back);
    void draw_panel(ui::Canvas &canvas, const ui::Fonts &fonts) const;

    double position_ = 0.0;
    double duration_ = 0.0;
    double buffered_ = 0.0;
    Options options_;
    std::vector<yt::Chapter> chapters_;
    int mode_ = 0;
    std::uint32_t avatar_ = 0;
    std::uint32_t preview_texture_ = 0;
    gfx::Rect preview_uv_{0.0f, 0.0f, 1.0f, 1.0f};
    float preview_aspect_ = 16.0f / 9.0f;
    float seek_pending_ = 0.0f; // seconds until a moved target is sought
    bool seek_ready_ = false;
    int seek_streak_ = 0;   // presses in a row: the step grows while held
    float seek_gap_ = 0.0f; // seconds since the last seek press
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
    bool signed_in_ = false;
    bool liked_ = false;
    bool disliked_ = false;
    bool subscribed_ = false;
    bool has_channel_ = false;
    std::string clock_;

    bool shown_ = true;
    float visibility_ = 1.0f;
    float idle_ = 0.0f;
    bool lifted_ = false;
    float lift_ = 0.0f;
    Zone zone_ = Zone::top;
    Button focus_ = Button::play;
    double seek_target_ = 0.0;
    float seek_hold_ = 0.0f; // shows the target, not the clock, just after a seek

    Panel panel_ = Panel::none;
    bool sleep_on_ = false;
    std::string panel_title_;
    std::vector<std::string> panel_options_;
    int panel_selected_ = -1;
    int panel_focus_ = 0;
    int picked_ = -1;
    float panel_open_ = 0.0f; // animated 0..1
};

} // namespace hui::app

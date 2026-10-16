// youtube-tv - The player: video texture, transport controls, related row.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/context.hpp"
#include "app/player_chrome.hpp"
#include "app/screen.hpp"
#include "yt/captions.hpp"
#include "yt/player.hpp"
#include "yt/service.hpp"
#include "yt/sponsorblock.hpp"

#include "ui/components.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace hui::app
{

class PlayerScreen final : public Screen
{
  public:
    explicit PlayerScreen(Context &context);
    ~PlayerScreen() override;

    const char *id() const override
    {
        return "player";
    }
    const char *title() const override
    {
        return "Playback";
    }
    const char *hint() const override
    {
        return "";
    }
    void enter() override;
    void leave() override;
    void update(const InputFrame &input, float ui_feedback, ui::Feedback &feedback) override;
    void draw(const Context &context, Frame &frame) const override;
    std::span<const TourStep> tour() const override;

  private:
    void request_video(const std::string &video_id);
    yt::CodecChoice codec_choice() const;
    void drop_texture();
    void draw_hud(ui::Canvas &canvas, const Context &context) const;
    void update_sponsorblock(const InputFrame &input, ui::Feedback &feedback);
    void reset_sponsorblock();
    void skip_segment_now(ui::Feedback &feedback);
    // Opens the same video again with the quality or sound track changed,
    // resuming where it was.
    void reopen();
    void open_menu(PlayerChrome::Command command);
    void apply_pick();
    void update_captions();
    void draw_captions(ui::Canvas &canvas, const Context &context) const;

    Context &context_;
    std::unique_ptr<yt::Player> player_;
    std::string requested_id_;
    std::string playing_id_;
    PlayerChrome chrome_;
    ui::Carousel related_;
    std::vector<yt::FeedItem> related_items_;
    std::uint64_t related_revision_ = 0;
    std::uint64_t video_revision_ = 0;
    std::uint32_t texture_ = 0;
    int texture_width_ = 0;
    int texture_height_ = 0;
    double last_pts_ = -1.0;
    std::chrono::steady_clock::time_point last_upload_log_{};
    bool in_related_ = false;
    bool ended_ = false;
    bool consumed_player_ = false;
    bool login_retry_sent_ = false; // one retry per gate, not one per frame
    float clock_ = 0.0f;
    double history_reported_s_ = 0.0; // the position last sent to watch history
    std::string history_toast_for_;   // the video whose history result was shown
    bool history_ok_shown_ = false;   // "added to history" shows once per session

    // Choices made in the player's menus; they last until the app closes.
    int quality_override_ = 0; // a height cap; 0 follows Settings
    std::string audio_track_;  // an audioTrack.id; empty plays the original
    double speed_ = 1.0;
    double resume_s_ = -1.0; // where a reopened video continues
    std::vector<int> menu_heights_;
    std::vector<std::string> menu_tracks_;
    // Subtitles: the language picked last is turned on again for the next
    // video that has it.
    yt::captions::Loader captions_;
    std::string caption_language_; // empty: off
    std::string caption_url_;      // the track loaded
    std::string captions_for_;     // the video it was chosen for
    std::string caption_text_;

    // Autoplay: when the video ends, the next suggestion starts after a
    // countdown (X now, Circle cancels), as in SmartTube.
    float autoplay_left_ = -1.0f; // seconds; negative when not counting
    bool autoplay_cancelled_ = false;
    const yt::FeedItem *next_suggestion() const;
    void play_next();

    // SponsorBlock: the segments of playing_id_, the seek-bar ranges drawn from
    // them, and the segment under the playhead (for the Triangle hint).
    yt::sponsorblock::Skipper sponsor_;
    std::string sponsor_video_;
    int sponsor_scope_ = -1;
    std::uint64_t sponsor_revision_ = 0;
    std::string sponsor_here_;
};

} // namespace hui::app

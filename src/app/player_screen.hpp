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
#include "yt/storyboard.hpp"

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
    const yt::FeedItem *focused_item() const override
    {
        if (!in_related_ || in_chapters_)
            return nullptr;
        const int index = related_.focus();
        if (index < 0 || index >= static_cast<int>(related_items_.size()))
            return nullptr;
        return &related_items_[static_cast<std::size_t>(index)];
    }
    void update(const InputFrame &input, float ui_feedback, ui::Feedback &feedback) override;
    void draw(const Context &context, Frame &frame) const override;
    std::span<const TourStep> tour() const override;

  private:
    void request_video(const std::string &video_id);
    // Cast from the phone: its commands, and what the player reports back.
    void update_remote();
    // The comments panel's lines from the service's comments of this video.
    std::vector<std::string> comment_lines() const;
    void update_extras(float dt);
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
    // SmartTube's chapters: a row of their own above the suggestions, each
    // card the storyboard frame of where it starts.
    ui::Carousel chapters_row_;
    bool in_chapters_ = false;
    std::uint64_t chapters_row_for_ = 0; // chapters_revision_ the row was built from
    // How far the controls rise for the rows below them.
    float rise() const;
    void fill_chapter_row();
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

    // Choices made in the player's menus; they last until the app closes.
    int quality_override_ = 0; // a height cap; 0 follows Settings
    int itag_override_ = 0;    // one format picked in "Video formats" (this video)
    std::string audio_track_;  // an audioTrack.id; empty plays the original
    double speed_ = 1.0;
    // Shuffle: the random pick from the section that plays next, chosen once
    // per video so the Next button and the end agree; the played ones are
    // left out until the section is used up.
    void choose_shuffle();
    std::string shuffle_pick_;
    std::string shuffle_for_;
    std::vector<std::string> played_;
    std::uint32_t rng_ = 0x9e3779b9u;
    // Jumps to the next (1) or previous (-1) chapter.
    void jump_chapter(int step);
    // The service's video state, copied only when it changed (it carries the
    // format list and the description: a copy every frame stalled the UI).
    const yt::VideoView &video_view() const;
    mutable yt::VideoView view_cache_;
    mutable std::uint64_t view_cache_revision_ = ~0ull;
    float waiting_s_ = 0.0f;     // how long the current video has been loading
    float back_armed_ = 0.0f;    // "double back": seconds the second Circle still exits
    float sleep_left_s_ = -1.0f; // the sleep timer (-1: off)
    // Above 1080p, a decoder that keeps dropping frames steps the quality
    // down to 1080p (what the console decodes smoothly).
    float smooth_check_s_ = 0.0f;
    std::uint64_t smooth_dropped_ = 0;
    int smooth_strikes_ = 0;
    bool sleep_at_end_ = false; // ... or stop at the end of this video
    std::uint64_t comments_seen_ = 0;
    bool comments_asked_more_ = false;
    std::vector<yt::Chapter> chapters_;
    yt::Storyboard storyboard_; // the seek preview frames of playing_id_
    std::string storyboard_for_;
    std::uint64_t chapters_revision_ = 0;
    std::string chapters_for_;
    int applied_speed_index_ = -1;     // Settings::speed_index last applied
    bool end_handled_ = false;         // the playback mode already acted on this end
    bool captions_chosen_ = false;     // the user picked captions (or Off) this session
    bool sponsor_markers_ = true;      // Settings::sb_markers the bar was painted with
    std::vector<double> skipped_ends_; // segments already skipped once (by end time)
    double resume_s_ = -1.0;           // where a reopened video continues
    std::vector<int> menu_heights_;
    std::vector<int> menu_itags_;
    std::vector<std::string> menu_tracks_;
    // Subtitles: the language picked last is turned on again for the next
    // video that has it.
    yt::captions::Loader captions_;
    std::string caption_language_; // empty: off
    std::string caption_url_;      // the track loaded
    std::string captions_for_;     // the video it was chosen for
    std::string caption_text_;

    // Autoplay: when the video ends, the next suggestion starts at once.
    const yt::FeedItem *next_suggestion() const;
    void play_next();

    // SponsorBlock: the segments of playing_id_, the seek-bar ranges drawn from
    // them, and the segment under the playhead (for the Triangle hint).
    yt::sponsorblock::Skipper sponsor_;
    std::string sponsor_video_;
    yt::sponsorblock::Options sponsor_options_;
    std::uint64_t sponsor_revision_ = 0;
    std::string sponsor_here_;
    double sponsor_highlight_ = -1.0; // the highlight ahead (Triangle jumps), or -1
    std::string sponsor_notice_for_;  // the video the whole-video label was told for
};

} // namespace hui::app

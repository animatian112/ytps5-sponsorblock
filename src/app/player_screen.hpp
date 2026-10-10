// youtube-tv - The player: video texture, transport controls, related row.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/context.hpp"
#include "app/screen.hpp"
#include "yt/player.hpp"
#include "yt/service.hpp"

#include "ui/components.hpp"

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
        return "L1/R1 skip 10s   Circle back   Options quit";
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

    Context &context_;
    std::unique_ptr<yt::Player> player_;
    std::string requested_id_;
    std::string playing_id_;
    ui::MediaControls controls_;
    ui::Carousel related_;
    std::vector<yt::FeedItem> related_items_;
    std::uint64_t related_revision_ = 0;
    std::uint64_t video_revision_ = 0;
    std::uint32_t texture_ = 0;
    int texture_width_ = 0;
    int texture_height_ = 0;
    double last_pts_ = -1.0;
    bool in_related_ = false;
    bool ended_ = false;
    bool consumed_player_ = false;
    bool login_retry_sent_ = false; // one retry per gate, not one per frame
    float clock_ = 0.0f;
};

} // namespace hui::app

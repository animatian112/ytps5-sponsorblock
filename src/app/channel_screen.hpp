// youtube-tv - A YouTube channel, laid out as SmartTube does it.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/context.hpp"
#include "app/rows_view.hpp"
#include "app/screen.hpp"

#include <cstdint>
#include <string>

namespace hui::app
{

// The header (picture, name, subscribers) and one row per part of the
// channel: its uploads, the home page's rows, Shorts, Live, playlists and the
// channels it features. Opened from any channel card, button or menu;
// Circle goes back to where it was opened.
class ChannelScreen final : public Screen
{
  public:
    explicit ChannelScreen(Context &context);

    const char *id() const override
    {
        return "channel";
    }
    const char *title() const override
    {
        return "Channel";
    }
    void enter() override;
    bool uses_back() const override
    {
        return true;
    }
    const yt::FeedItem *focused_item() const override
    {
        return rows_.focused_item();
    }
    void update(const InputFrame &input, float dt, ui::Feedback &feedback) override;
    void draw(const Context &context, Frame &frame) const override;

  private:
    Context &context_;
    RowsView rows_;
    std::string channel_id_;
    std::uint64_t seen_revision_ = 0;
    std::uint64_t seen_channel_revision_ = 0;
    float clock_ = 0.0f;
};

} // namespace hui::app

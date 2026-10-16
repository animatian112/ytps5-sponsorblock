// youtube-tv - Home: what YouTube recommends, as a grid.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/context.hpp"
#include "app/feed_view.hpp"
#include "app/rows_view.hpp"
#include "app/screen.hpp"

#include <memory>

namespace hui::app
{

class HomeScreen final : public Screen
{
  public:
    explicit HomeScreen(Context &context);

    const char *id() const override
    {
        return "home";
    }
    const char *title() const override
    {
        return "Home";
    }
    const char *hint() const override
    {
        return "Triangle refresh   Hold Cross: video menu   L1/R1 section   Options quit";
    }
    // True once the grid ran off its left edge: the app moves the focus into
    // the rail.
    bool wants_rail_focus() const override
    {
        return use_rows_ ? rows_.wants_rail_focus() : feed_.wants_rail_focus();
    }
    void enter() override;
    const yt::FeedItem *focused_item() const override
    {
        return use_rows_ ? rows_.focused_item() : feed_.focused_item();
    }
    void update(const InputFrame &input, float dt, ui::Feedback &feedback) override;
    void draw(const Context &context, Frame &frame) const override;
    std::span<const TourStep> tour() const override;

  private:
    void refresh_from_service();

    Context &context_;
    FeedView feed_;
    // SmartTube's Home: one row per shelf when the feed has shelves.
    RowsView rows_;
    bool use_rows_ = false;
    std::uint64_t seen_revision_ = 0;
    // Home's rows also draw on these feeds (SmartTube's Home: recommended,
    // continue watching, subscriptions).
    std::uint64_t seen_subscriptions_ = 0;
    std::uint64_t seen_history_ = 0;
    bool extras_requested_ = false;
    bool more_requested_ = false;
    float clock_ = 0.0f;
};

} // namespace hui::app

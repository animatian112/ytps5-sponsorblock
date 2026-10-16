// youtube-tv - A topic section of the rail (Music, Gaming, News, Live,
// Sports): YouTube's topic channel for it, as rows like SmartTube's.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/context.hpp"
#include "app/feed_view.hpp"
#include "app/rows_view.hpp"
#include "app/screen.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace hui::app
{

class TopicScreen final : public Screen
{
  public:
    TopicScreen(Context &context, const char *id, const char *title, std::string browse_id);
    // A playlist page: the browse id comes from context.pending_browse on
    // each enter, and Circle goes back.
    static std::unique_ptr<TopicScreen> playlist_page(Context &context);

    const char *id() const override
    {
        return id_;
    }
    const char *title() const override
    {
        return title_.c_str();
    }
    const char *hint() const override
    {
        return playlist_page_ ? "Circle back   Options quit"
                              : "Left: menu   L1/R1 section   Options quit";
    }
    bool wants_rail_focus() const override
    {
        return use_rows_ ? rows_.wants_rail_focus() : grid_.wants_rail_focus();
    }
    void enter() override;
    const yt::FeedItem *focused_item() const override
    {
        return use_rows_ ? rows_.focused_item() : grid_.focused_item();
    }
    void update(const InputFrame &input, float dt, ui::Feedback &feedback) override;
    void draw(const Context &context, Frame &frame) const override;

  private:
    bool mine() const;
    void refresh();

    Context &context_;
    const char *id_;
    std::string title_;
    bool playlist_page_ = false;
    std::string browse_id_;
    FeedView grid_;
    RowsView rows_;
    bool use_rows_ = false;
    std::uint64_t seen_revision_ = 0;
    bool more_requested_ = false;
    float clock_ = 0.0f;
};

} // namespace hui::app

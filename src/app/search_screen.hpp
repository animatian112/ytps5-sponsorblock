// youtube-tv - Search: a query field, the on-screen keyboard and the results
// grid, all fed by the service's search snapshots. Confirming one of the
// channel cards the answer carried opens that channel's page in place of the
// query answer.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/context.hpp"
#include "app/feed_view.hpp"
#include "app/screen.hpp"

#include "ui/components.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace hui::app
{

class SearchScreen final : public Screen
{
  public:
    explicit SearchScreen(Context &context);

    const char *id() const override
    {
        return "search";
    }
    const char *title() const override
    {
        return "Search";
    }
    const char *hint() const override
    {
        // Left and right turn the channel tabs while the tab bar has the
        // focus; L1/R1 stay the app's section keys on every screen.
        return in_channel_ ? "Circle back   Left/Right: tab   Options quit"
                           : "Circle back   Left: menu   Options quit";
    }
    void enter() override;
    const yt::FeedItem *focused_item() const override
    {
        return spot_ == Spot::results ? results_.focused_item() : nullptr;
    }
    void update(const InputFrame &input, float dt, ui::Feedback &feedback) override;
    void draw(const Context &context, Frame &frame) const override;
    std::span<const TourStep> tour() const override;
    bool wants_rail_focus() const override;
    bool uses_back() const override;

  private:
    // The component that owns the focus. The keyboard and the channel row only
    // exist in search mode, the tab bar only on a channel page.
    enum class Spot : unsigned char
    {
        field,
        keyboard,
        channels,
        results, // the grid: the query answer or the open channel's videos
        tabs,    // the tab bar of the open channel page
    };

    void run_search();
    void refresh_results();
    void refresh_channel();
    void refresh_channel_feed();
    void open_channel(std::size_t index);
    void open_channel_item(const yt::ChannelItem &channel);
    void back_to_results();
    // Remembers the cell the results grid is left on when the focus moves up
    // to the channel's tab bar; the grid comes back to it on the way down.
    void remember_grid();
    void update_search(const InputFrame &input, ui::Feedback &feedback);
    void update_channel(const InputFrame &input, ui::Feedback &feedback);
    void draw_channel(ui::Canvas &canvas, const Context &context) const;
    // The channel page has not answered yet: no component owns the focus.
    bool channel_loading() const;

    Context &context_;
    ui::SearchField field_;
    ui::Keyboard keys_;
    FeedView results_;
    ui::Carousel channels_; // the channel row of the current answer
    ui::TabBar tabs_;       // the tabs of the open channel page
    std::vector<yt::ChannelItem> channel_items_;
    Spot spot_ = Spot::field;
    bool in_channel_ = false; // the grid shows a channel page, not the answer
    // The page came from outside the search (a card menu, the player, the
    // sidebar): Circle goes back there, not to a search nobody asked for.
    bool channel_external_ = false;
    std::string channel_id_;         // the channel the page shows ("" = none)
    yt::ChannelItem channel_header_; // what that page answered with
    yt::Load channel_state_ = yt::Load::idle;
    std::uint32_t channel_avatar_ = 0;
    int grid_focus_ = 0;         // the cell the results grid was left on
    bool grid_scrolled_ = false; // it had walked below its first row
    bool has_searched_ = false;  // a query has been sent at least once
    bool searching_ = false;     // the service has not answered the last query yet
    bool more_requested_ = false;
    std::uint64_t seen_revision_ = 0;
    std::uint64_t seen_channel_revision_ = 0;
    std::uint64_t seen_channel_feed_revision_ = 0;
    float clock_ = 0.0f;
};

} // namespace hui::app

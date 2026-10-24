// youtube-tv - A feed as SmartTube shows Home: one titled row per shelf
// ("Recommended", "Trending", ...), each scrolling sideways, the rows
// scrolling up and down.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/context.hpp"
#include "yt/types.hpp"

#include "ui/components.hpp"

#include <memory>
#include <string>
#include <vector>

namespace hui::app
{

class RowsView
{
  public:
    void set_bounds(const gfx::Rect &bounds);
    const gfx::Rect &bounds() const
    {
        return bounds_;
    }
    // Groups consecutive items by FeedItem::shelf; untitled items join the row
    // before them (or a "Recommended" row). Keeps the focus where it was.
    void set_items(const std::vector<yt::FeedItem> &items, const Context &context);
    // How many shelves the items form: a feed with one is better as a grid.
    static int shelf_count(const std::vector<yt::FeedItem> &items);

    bool handle(const InputFrame &input, ui::Feedback &feedback);
    void update(float dt, const Context &context);
    void draw(ui::Canvas &canvas) const;
    void set_active(bool active);

    bool has_activation() const
    {
        return !activated_.empty();
    }
    const std::string &activated() const
    {
        return activated_;
    }
    void clear_activation()
    {
        activated_.clear();
    }
    // Left past the first card of a row: the app hands the focus to the rail.
    bool wants_rail_focus() const
    {
        return wants_rail_;
    }
    // The focus is on one of the last rows: time to ask for more.
    bool near_end() const;
    // The card the focus sits on, or nullptr.
    const yt::FeedItem *focused_item() const;
    bool empty() const
    {
        return rows_.empty();
    }

  private:
    struct Row
    {
        std::string title;
        std::vector<yt::FeedItem> items;
        ui::Carousel shelf;
    };
    float row_height() const;
    void place();

    std::vector<std::unique_ptr<Row>> rows_;
    gfx::Rect bounds_{160.0f, 150.0f, 1680.0f, 860.0f};
    int focus_row_ = 0;
    bool active_ = true;
    bool wants_rail_ = false;
    std::string activated_;
    float scroll_ = 0.0f; // the rows' offset, animated toward the focus row
};

} // namespace hui::app

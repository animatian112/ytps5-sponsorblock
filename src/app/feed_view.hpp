// youtube-tv - The video grid shared by Home, Search and Subscriptions:
// cards from the feed, the loading placeholder and the error state.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/context.hpp"
#include "yt/types.hpp"

#include "ui/components.hpp"

#include <string>
#include <vector>

namespace hui::app
{

class FeedView
{
  public:
    FeedView();

    ui::GridView &grid();
    void set_bounds(const gfx::Rect &bounds);
    // Replaces the cards; called only when the feed's revision changed.
    void set_items(const std::vector<yt::FeedItem> &items, const Context &context);
    void set_state(yt::Load state, const std::string &error);
    // Empty-state text ("Noch nichts geladen", "No results", ...).
    void set_empty_text(std::string text);

    // Forwards nav to the grid while it is active; returns true when the grid
    // consumed the frame.
    bool handle(const InputFrame &input, ui::Feedback &feedback, const Context &context);
    void update(float dt, const Context &context);
    void draw(ui::Canvas &canvas, const Context &context) const;

    // The video the last handle() activated, if any.
    bool has_activation() const
    {
        return activated_ >= 0;
    }
    const yt::FeedItem &activated() const
    {
        return activated_item_;
    }
    void clear_activation()
    {
        activated_ = -1;
    }
    const std::vector<yt::FeedItem> &items() const
    {
        return items_;
    }
    // True when the focused cell is in the last row: time to ask for more.
    bool at_last_row() const;
    // True after the frame in which the player pushed left past the first
    // column: the app moves the focus into the rail.
    bool wants_rail_focus() const;
    void set_active(bool active);
    void enter();
    // The cell the grid's focus sits on, and a way to put it back there: a
    // screen remembers both while another component (the channel tab bar)
    // holds the focus. set_focus scrolls the cell's row into view.
    int focus_index() const;
    void set_focus(int index);
    float smooth_focus() const;

  private:
    ui::GridView grid_;
    std::vector<yt::FeedItem> items_;
    yt::Load state_ = yt::Load::idle;
    std::string error_;
    std::string empty_text_;
    int activated_ = -1;
    yt::FeedItem activated_item_;
};

// "12:34" from seconds; also used by the player and the cards.
std::string format_duration(double seconds);

} // namespace hui::app

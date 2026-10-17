// youtube-tv - The menu a video card opens when Cross is held (SmartTube's).
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A centered dialog: the card's title on top, then its entries ("Open
// channel", "Save to Watch later", "Not interested", ...). Up/Down walk them,
// Cross picks, Circle closes. The app builds the entries and acts on the pick;
// the same dialog shows the account's playlists for "Save to playlist".

#pragma once

#include "core/input.hpp"
#include "ui/components.hpp"

#include <string>
#include <utility>
#include <vector>

namespace hui::app
{

class CardMenu
{
  public:
    struct Entry
    {
        std::string label;
        int id = 0;
    };

    void open(std::string title, std::vector<Entry> entries);
    void close();
    bool active() const
    {
        return active_;
    }
    // The picked entry's id, or -1 (nothing yet, or closed with Circle).
    int handle(const InputFrame &input, ui::Feedback &feedback);
    void update(float dt);
    void draw(ui::Canvas &canvas, const ui::Fonts &fonts) const;
    // The color scheme (theme.hpp palette()).
    void set_scheme(int scheme)
    {
        scheme_ = scheme;
    }

  private:
    int scheme_ = 0;
    std::string title_;
    std::vector<Entry> entries_;
    int focus_ = 0;
    bool active_ = false;
    float shown_ = 0.0f; // animated 0..1
};

} // namespace hui::app

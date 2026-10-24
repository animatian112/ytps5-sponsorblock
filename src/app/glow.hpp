// youtube-tv - The Glow layout: the focused video behind a large header.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Settings > User interface > Layout > Glow, after the newer TV apps (and
// YouTube MaxD's Afterglow): the focused video's picture fills the screen,
// dimmed and faded into the page; its title, channel and views stand large
// at the top; the rows sit below, as rounded pictures with a white ring and
// the words under them. The SmartTube layout stays the default.

#pragma once

#include "app/thumbs.hpp"
#include "core/settings.hpp"
#include "gfx/draw_list.hpp"
#include "ui/components.hpp"
#include "yt/types.hpp"

#include <cstdint>
#include <string>

namespace hui::app
{

inline bool glow_layout(const Settings &settings)
{
    return settings.ui_layout == 1;
}

// Where the rows start under the header, and the classic top otherwise.
constexpr float kGlowContentTop = 500.0f;
inline float content_top(const Settings &settings, float classic_top)
{
    return glow_layout(settings) ? kGlowContentTop : classic_top;
}

// The card look of each layout (the grid's and every shelf's).
void style_cards(ui::CardLook &card, const Settings &settings);

class GlowBackdrop
{
  public:
    // Follows the focused card (nullptr: none). The picture changes once the
    // focus has rested a moment, fading in over the one before.
    void update(const yt::FeedItem *focused, ThumbCache &thumbs, float dt);
    // The page, the picture and its fades (under everything, still while the
    // sidebar pushes the page aside).
    void draw_backdrop(gfx::DrawList &list) const;
    // The focused card's label, title and channel line (moves with the page).
    void draw_hero(gfx::DrawList &list, const ui::Fonts &fonts) const;
    void clear();

  private:
    struct Layer
    {
        std::string video_id;
        std::uint32_t texture = 0;
        float alpha = 0.0f;
    };
    static std::string picture_url(const std::string &video_id);

    Layer current_;
    Layer previous_;
    std::string pending_id_;
    float rest_s_ = 0.0f;
    // The header's words, for the card in focus now.
    bool has_item_ = false;
    std::string label_;
    std::string title_;
    std::string meta_;
    float hero_alpha_ = 0.0f;
};

} // namespace hui::app

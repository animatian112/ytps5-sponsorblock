// youtube-tv - The app's one theme: YouTube's dark palette as plain data.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui/theme.hpp"

namespace hui::app
{

const ui::Theme &youtube_theme();
// The same for video cards (SmartTube's look): small corners, and the duration
// on a dark pill rather than on the red accent.
const ui::Theme &card_theme();

// SmartTube's color schemes (Settings > User interface > Color scheme): the
// sidebar, the side menus' body and header, and a focused menu row.
struct Palette
{
    gfx::Color sidebar;
    gfx::Color dialog;
    gfx::Color header;
    gfx::Color focus;
};
const Palette &palette(int scheme);

// The content column right of the collapsed rail, shared by every section.
constexpr float kContentX = 160.0f;
constexpr float kContentW = 1680.0f;
constexpr float kSectionTitleY = 104.0f; // baseline of "Home", "Subscriptions", ...
constexpr float kSectionTitleSize = 36.0f;

} // namespace hui::app

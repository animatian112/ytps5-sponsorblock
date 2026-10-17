// youtube-tv - SmartTube's icons (MIT, Copyright (c) yuliskov), loaded once.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// White glyphs on a transparent 96x96 square under <assets>/icons. Each is
// decoded and uploaded the first time it is drawn and kept until exit, so
// they never compete with the thumbnail cache.

#pragma once

#include "gfx/draw_list.hpp"
#include "gfx/gl_batch.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace hui::app
{

// Where the PNGs live and which batch uploads them; call once at start-up.
void init_icons(gfx::GlBatch &batch, std::string root);

// The texture for icons/<name>.png, or 0 if it is missing.
std::uint32_t icon_texture(std::string_view name);

// Draws the icon filling box, tinted; returns false if it is missing.
bool draw_icon(gfx::DrawList &list, std::string_view name, const gfx::Rect &box, gfx::Color tint);

// The size of icons/<name>.png in pixels (0, 0 when missing).
void icon_size(std::string_view name, int *width, int *height);

} // namespace hui::app

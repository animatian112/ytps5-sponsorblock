// youtube-tv - The GL side of colour emoji: pictures decoded on first use.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace hui::gfx
{
class GlBatch;
}

namespace hui::app
{

// Loads assets/emoji/twemoji.pack and registers the texture function that
// gfx::DrawList::text uses for emoji (frame thread only). False when the pack
// is missing: text then draws without emoji, as before.
bool init_emoji(gfx::GlBatch &batch, const std::string &pack_path);

} // namespace hui::app

// youtube-tv - Colour emoji in text: Twemoji pictures in place of glyphs.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fonts have no emoji, so a title like "Best day ever 😂🔥" showed boxes.
// assets/emoji/twemoji.pack (tools/pack-emoji.py) holds Twemoji's 72 px
// pictures by code point sequence (ZWJ families, skin tones, flags, keycaps).
// Font::measure/layout ask match() at every position: a sequence found there
// is one square picture, drawn by DrawList::text through the texture function
// the app registers (it decodes a picture the first time it is drawn).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hui::gfx::emoji
{

// Takes the pack's bytes; false (and no emoji) when it is not a pack.
bool load(std::string data);
std::size_t count();

// The emoji whose sequence starts at text[at]: its id (1-based) and the bytes
// it spans, or 0 when none does.
int match(std::string_view text, std::size_t at, std::size_t *consumed);
// The PNG of an emoji id; empty for an unknown id.
std::string_view png(int id);

// Code points that never draw by themselves: variation selectors, the
// zero-width joiner, skin tone modifiers and tag characters left over from a
// sequence the pack does not have.
bool invisible(std::uint32_t codepoint);

// The texture for an emoji id (0 while unavailable). Set by the app.
using TextureFunction = std::uint32_t (*)(int id);
void set_texture_function(TextureFunction function);
std::uint32_t texture(int id);

// Size of an emoji relative to the text size, and its advance.
constexpr float kScale = 1.05f;
constexpr float kAdvance = 1.17f;

} // namespace hui::gfx::emoji

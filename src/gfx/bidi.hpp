// youtube-tv - Right-to-left text: Arabic letter shapes and display order.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Font::layout draws code points left to right, one glyph each, so Arabic
// and Hebrew came out backwards, and Arabic letters stood apart in their
// isolated shape instead of joining into words (#5, #6). There is no
// HarfBuzz or FriBidi on the console; what titles need is small enough to
// do here:
//   - Arabic shaping: each letter becomes the presentation-form code point
//     for its position in the word (initial, medial, final, isolated), and
//     lam + alef becomes its ligature. The Arabic face
//     (assets/fonts/dejavu-sans-arabic.huifont) has a glyph for each form.
//   - Display order: a cut-down Unicode Bidirectional Algorithm (UAX #9) for
//     one line without embeddings or isolates: the paragraph direction from
//     the first strong letter, the weak-type rules for numbers (W1, W4-W7),
//     neutrals (N1, N2), implicit levels (I1, I2), reordering (L2) and
//     mirrored brackets (L4).
// Lines are wrapped in logical order first (Font::wrap); each line is then
// shaped and reordered on its own when it is measured or drawn.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace hui::gfx
{

// One line of UTF-8 in reading order, turned into the order and letter
// shapes the renderer draws left to right. Text with no right-to-left letter
// comes back as given, without a copy (the common case: checked byte-wise
// first, so Latin titles cost one scan). Otherwise the result is built in
// *storage and the view points into it. Emoji sequences stay whole.
std::string_view visual_line(std::string_view text, std::string *storage);

// True for a code point written right to left (Hebrew, Arabic, Syriac,
// Thaana, N'Ko and their presentation forms).
bool is_rtl(std::uint32_t codepoint);

// The Arabic letter's presentation form for its joining position, or the
// letter itself when it has none (also for anything that is not one).
enum class JoinForm : std::uint8_t
{
    isolated,
    final_form,
    initial,
    medial,
};
std::uint32_t arabic_form(std::uint32_t letter, JoinForm form);

} // namespace hui::gfx

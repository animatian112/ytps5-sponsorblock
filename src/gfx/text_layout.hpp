// ps5-homebrew-ui - Unicode visual ordering for the baked font renderer.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace hui::gfx::text_layout
{
// Logical UTF-8 stays in the model. Convert a single display line to visual
// order, with Arabic presentation forms and intact packed emoji sequences.
std::string visual(std::string_view text);
// A safe display-unit boundary for clipping: packed emoji and trailing marks
// stay with their base. This is not a general Unicode grapheme segmenter.
std::size_t next_boundary(std::string_view text, std::size_t start);
} // namespace hui::gfx::text_layout

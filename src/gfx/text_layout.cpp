// ps5-homebrew-ui - Unicode bidi and Arabic joining for baked SDF glyphs.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/text_layout.hpp"

#include "gfx/emoji.hpp"
#include "gfx/font.hpp"
#include "third_party/fribidi/fribidi.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace hui::gfx::text_layout
{
namespace
{
void append(std::string &out, std::uint32_t cp)
{
    if (cp < 0x80)
        out += static_cast<char>(cp);
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool control(std::uint32_t cp)
{
    const auto type = fribidi_get_bidi_type(cp);
    return FRIBIDI_IS_EXPLICIT_OR_BN(type) || FRIBIDI_IS_ISOLATE(type) || cp == 0x061C ||
           cp == 0x200E || cp == 0x200F;
}
} // namespace

std::string visual(std::string_view text)
{
    // Preserve the overwhelmingly common ASCII path without Unicode work.
    if (std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 32 && c < 127; }))
        return std::string(text);
    std::vector<FriBidiChar> logical;
    std::vector<std::string_view> pictures;
    for (std::size_t i = 0; i < text.size();)
    {
        std::size_t consumed = 0;
        if (emoji::match(text, i, &consumed) != 0)
        {
            // One neutral object per emoji, including families and flags.
            logical.push_back(0xFFFC);
            pictures.push_back(text.substr(i, consumed));
            i += consumed;
        }
        else
        {
            logical.push_back(next_codepoint(text, &i));
            pictures.emplace_back();
        }
    }
    if (logical.empty() ||
        logical.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return std::string(text);
    const auto count = static_cast<FriBidiStrIndex>(logical.size());
    std::vector<FriBidiChar> ordered(logical.size());
    std::vector<FriBidiStrIndex> from(logical.size());
    FriBidiParType direction = FRIBIDI_PAR_ON; // first strong character
    if (!fribidi_log2vis(logical.data(), count, &direction, ordered.data(), nullptr, from.data(),
                         nullptr))
        return std::string(text);
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < ordered.size(); ++i)
    {
        const auto picture = pictures[static_cast<std::size_t>(from[i])];
        if (!picture.empty())
            result.append(picture);
        else if (!control(ordered[i]))
            append(result, ordered[i]);
    }
    return result;
}

std::size_t next_boundary(std::string_view text, std::size_t start)
{
    if (start >= text.size())
        return text.size();
    std::size_t consumed = 0;
    std::size_t end = start;
    if (emoji::match(text, start, &consumed) != 0)
        end += consumed;
    else
        next_codepoint(text, &end);
    while (end < text.size())
    {
        std::size_t next = end;
        const auto cp = next_codepoint(text, &next);
        if (fribidi_get_bidi_type(cp) != FRIBIDI_TYPE_NSM && cp != 0x200C && cp != 0x200D)
            break;
        end = next;
    }
    return end;
}
} // namespace hui::gfx::text_layout

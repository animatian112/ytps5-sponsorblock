// youtube-tv - Tests for emoji matching in text.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/emoji.hpp"

#include "core/save_file.hpp"
#include "gfx/font.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{

class Emoji : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        std::string pack;
        ASSERT_TRUE(hui::save::read_file(std::string(HUI_SOURCE_DIR) + "/assets/emoji/twemoji.pack",
                                         &pack, 16u << 20));
        ASSERT_TRUE(hui::gfx::emoji::load(std::move(pack)));
    }

    static std::size_t span(const std::string &text, std::size_t at = 0)
    {
        std::size_t consumed = 0;
        return hui::gfx::emoji::match(text, at, &consumed) != 0 ? consumed : 0;
    }
};

TEST_F(Emoji, MatchesSinglesSequencesFlagsAndKeycaps)
{
    EXPECT_GT(hui::gfx::emoji::count(), 3000u);
    EXPECT_EQ(span("\xF0\x9F\x98\x82 lol"), 4u); // face with tears of joy
    // A variation selector after the emoji belongs to it.
    EXPECT_EQ(span("\xE2\x9D\xA4\xEF\xB8\x8F!"), 6u); // red heart + fe0f
    // A family joined by zero-width joiners is one picture.
    EXPECT_EQ(span("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"),
              18u);
    // A thumbs up with a skin tone.
    EXPECT_EQ(span("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"), 8u);
    // Two regional indicators are a flag (Japan), the next pair another.
    const std::string flags = "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";
    EXPECT_EQ(span(flags), 8u);
    EXPECT_EQ(span(flags, 8), 8u);
    // Keycap one: a digit only starts an emoji when the keycap mark follows.
    EXPECT_EQ(span("1\xEF\xB8\x8F\xE2\x83\xA3"), 7u);
    EXPECT_EQ(span("1 and 2"), 0u);
    EXPECT_EQ(span("plain text"), 0u);
    // Text punctuation in the general punctuation block is not an emoji.
    EXPECT_EQ(span("\xE2\x80\x94"), 0u); // em dash
}

TEST_F(Emoji, LeftoverModifiersDrawNothing)
{
    EXPECT_TRUE(hui::gfx::emoji::invisible(0xfe0f));
    EXPECT_TRUE(hui::gfx::emoji::invisible(0x200d));
    EXPECT_TRUE(hui::gfx::emoji::invisible(0x1f3fd));
    EXPECT_FALSE(hui::gfx::emoji::invisible('a'));
    EXPECT_FALSE(hui::gfx::emoji::png(1).empty());
    EXPECT_TRUE(hui::gfx::emoji::png(0).empty());
}

} // namespace

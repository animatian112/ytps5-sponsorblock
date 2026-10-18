// youtube-tv - Cast from the phone: the lounge stream's chunks.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/lounge.hpp"

#include <gtest/gtest.h>

TEST(Lounge, SplitsCompleteChunksAndKeepsTheRest)
{
    std::string buffer = "11\n[[1,[\"a\"]]]14\n[[2,[\"noop\"]";
    std::vector<std::string> chunks = yt::take_lounge_chunks(buffer);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0], "[[1,[\"a\"]]]");
    EXPECT_EQ(buffer, "14\n[[2,[\"noop\"]");
    buffer += "]]";
    chunks = yt::take_lounge_chunks(buffer);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0], "[[2,[\"noop\"]]]");
    EXPECT_TRUE(buffer.empty());
}

TEST(Lounge, CountsLengthsInUtf16Units)
{
    // "é" is one unit in two bytes, the emoji two units in four bytes.
    std::string buffer = "5\n\"\xC3\xA9\xF0\x9F\x98\x80\"3\nabc";
    const std::vector<std::string> chunks = yt::take_lounge_chunks(buffer);
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0], "\"\xC3\xA9\xF0\x9F\x98\x80\"");
    EXPECT_EQ(chunks[1], "abc");
}

TEST(Lounge, StartsStoppedWithoutACode)
{
    yt::Lounge lounge("/nonexistent", "Test");
    EXPECT_FALSE(lounge.running());
    EXPECT_TRUE(lounge.status().code.empty());
    EXPECT_TRUE(lounge.take_commands().empty());
}

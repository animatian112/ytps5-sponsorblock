// youtube-tv - Chapters from description timestamps.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/chapters.hpp"

#include <gtest/gtest.h>

TEST(Chapters, ReadsTimestampedLinesFromZero)
{
    const auto chapters = yt::parse_chapters("Great video!\n\nChapters:\n0:00 Intro\n"
                                             "1:23 - Setup\n(12:05) The build\n1:02:03 | Outro\n"
                                             "Follow me on things\n");
    ASSERT_EQ(chapters.size(), 4u);
    EXPECT_EQ(chapters[0].title, "Intro");
    EXPECT_DOUBLE_EQ(chapters[1].start_s, 83.0);
    EXPECT_EQ(chapters[1].title, "Setup");
    EXPECT_DOUBLE_EQ(chapters[2].start_s, 725.0);
    EXPECT_EQ(chapters[2].title, "The build");
    EXPECT_DOUBLE_EQ(chapters[3].start_s, 3723.0);
    EXPECT_EQ(chapters[3].title, "Outro");
    EXPECT_EQ(yt::chapter_at(chapters, 100.0), 1);
    EXPECT_EQ(yt::chapter_at(chapters, 0.0), 0);
}

TEST(Chapters, NeedsThreeFromZero)
{
    EXPECT_TRUE(yt::parse_chapters("0:00 a\n1:00 b\n").empty());
    EXPECT_TRUE(yt::parse_chapters("0:10 a\n1:00 b\n2:00 c\n").empty());
    EXPECT_TRUE(yt::parse_chapters("no times here").empty());
}

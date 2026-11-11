// youtube-tv - Stats for nerds: the slow lines, refreshed a few times a second.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/stats_overlay.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{

using hui::app::StatsOverlay;

constexpr float kFrame = 1.0f / 60.0f;

TEST(StatsOverlay, FirstTickRebuildsThenWaitsForThePeriod)
{
    StatsOverlay stats;
    EXPECT_TRUE(stats.tick(kFrame));
    // At 60 fps a quarter second is 15 frames: the next 14 reuse the lines.
    int rebuilds = 0;
    for (int i = 0; i < 14; ++i)
        rebuilds += stats.tick(kFrame) ? 1 : 0;
    EXPECT_EQ(rebuilds, 0);
    EXPECT_TRUE(stats.tick(kFrame + 0.001f));
}

TEST(StatsOverlay, RebuildsAboutFourTimesASecond)
{
    StatsOverlay stats;
    int rebuilds = 0;
    for (int i = 0; i < 600; ++i) // ten seconds at 60 fps
        rebuilds += stats.tick(kFrame) ? 1 : 0;
    EXPECT_GE(rebuilds, 36);
    EXPECT_LE(rebuilds, 41);
}

TEST(StatsOverlay, LongFrameDoesNotCauseABurst)
{
    StatsOverlay stats;
    ASSERT_TRUE(stats.tick(kFrame));
    // A two-second stall rebuilds once, not eight times in a row.
    EXPECT_TRUE(stats.tick(2.0f));
    EXPECT_FALSE(stats.tick(kFrame));
    EXPECT_FALSE(stats.tick(kFrame));
}

TEST(StatsOverlay, InvalidateRebuildsOnTheNextTick)
{
    StatsOverlay stats;
    ASSERT_TRUE(stats.tick(kFrame));
    EXPECT_FALSE(stats.tick(kFrame));
    stats.invalidate();
    EXPECT_TRUE(stats.tick(0.0f));
    EXPECT_FALSE(stats.tick(kFrame));
}

TEST(StatsOverlay, KeepsTheLinesAndWidth)
{
    StatsOverlay stats;
    EXPECT_TRUE(stats.lines().empty());
    EXPECT_FLOAT_EQ(stats.width(), 0.0f);
    stats.set({"client  tv", "format  vp9"}, 312.5f);
    ASSERT_EQ(stats.lines().size(), 2u);
    EXPECT_EQ(stats.lines()[1], "format  vp9");
    EXPECT_FLOAT_EQ(stats.width(), 312.5f);
    // invalidate() only marks the lines stale; they stay drawable until the
    // rebuild replaces them.
    stats.invalidate();
    EXPECT_EQ(stats.lines().size(), 2u);
}

TEST(StatsOverlay, SplitsTheStatsBlock)
{
    EXPECT_TRUE(StatsOverlay::split_lines("").empty());
    EXPECT_EQ(StatsOverlay::split_lines("decode  sw"), (std::vector<std::string>{"decode  sw"}));
    EXPECT_EQ(StatsOverlay::split_lines("a\nb\nc"), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(StatsOverlay::split_lines("a\nb\n"), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(StatsOverlay::split_lines("a\n\nb"), (std::vector<std::string>{"a", "", "b"}));
}

} // namespace

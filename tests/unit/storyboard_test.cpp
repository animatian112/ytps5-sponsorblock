// youtube-tv - Seek previews from storyboard specs.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/storyboard.hpp"

#include <gtest/gtest.h>

TEST(Storyboard, FindsTheSheetAndCellOfATime)
{
    const char *spec = "https://i.ytimg.com/sb/abc/storyboard3_L$L/$N.jpg?sqp=xyz|"
                       "48#27#100#10#10#0#default#rs$AAA|"
                       "80#45#120#10#10#2000#M$M#rs$BBB|"
                       "160#90#120#5#5#2000#M$M#rs$CCC";
    yt::Storyboard board;
    ASSERT_TRUE(board.parse(spec, 240.0, 160));
    // 2 s a frame, 25 a sheet: 61 s is frame 30, sheet 1, cell 5 (row 1, col 0).
    const yt::StoryboardFrame frame = board.frame_at(61.0);
    EXPECT_EQ(frame.url, "https://i.ytimg.com/sb/abc/storyboard3_L2/M1.jpg?sqp=xyz&sigh=rs$CCC");
    EXPECT_FLOAT_EQ(frame.u, 0.0f);
    EXPECT_FLOAT_EQ(frame.v, 0.2f);
    EXPECT_FLOAT_EQ(frame.w, 0.2f);
    EXPECT_EQ(frame.width, 160);
    EXPECT_FALSE(yt::Storyboard().parse("", 10.0));
}

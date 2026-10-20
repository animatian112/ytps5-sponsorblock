// youtube-tv - Search suggestions: the autocomplete answer's two wordings.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/suggest.hpp"

#include <gtest/gtest.h>

TEST(Suggest, ReadsTheYoutubeAndTheFirefoxWording)
{
    const auto youtube = yt::parse_suggestions(
        R"(window.google.ac.h(["mario",[["mario kart",0,[512]],["mario party",0,[512]]],{"k":1}]))");
    ASSERT_EQ(youtube.size(), 2u);
    EXPECT_EQ(youtube[0], "mario kart");
    EXPECT_EQ(youtube[1], "mario party");

    const auto firefox = yt::parse_suggestions(R"(["ps5",["ps5 pro","ps5 games"]])");
    ASSERT_EQ(firefox.size(), 2u);
    EXPECT_EQ(firefox[1], "ps5 games");

    EXPECT_TRUE(yt::parse_suggestions("").empty());
    EXPECT_TRUE(yt::parse_suggestions("<html>").empty());
}

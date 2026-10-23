// youtube-tv - DeArrow: which title and thumbnail a branding answer shows.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/dearrow.hpp"

#include <gtest/gtest.h>

TEST(DeArrow, ShowsTheFirstTrustedTitleAndThumbnail)
{
    yt::Branding branding;
    ASSERT_TRUE(yt::parse_branding(
        R"({"titles":[{"title":">How the engine works","original":false,"votes":2,"locked":false,"UUID":"a"},
                      {"title":"YOU WON'T BELIEVE","original":true,"votes":0,"locked":false,"UUID":"b"}],
            "thumbnails":[{"timestamp":91.237,"original":false,"votes":0,"locked":false,"UUID":"c"}],
            "randomTime":0.4,"videoDuration":300})",
        &branding));
    EXPECT_EQ(branding.title, "How the engine works");
    EXPECT_TRUE(branding.has_thumbnail);
    EXPECT_DOUBLE_EQ(branding.thumbnail_s, 91.237);
    EXPECT_EQ(yt::dearrow_thumbnail_url("dQw4w9WgXcQ", branding.thumbnail_s),
              "https://dearrow-thumb.ajay.app/api/v1/getThumbnail?videoID=dQw4w9WgXcQ&time=91.237");
}

TEST(DeArrow, KeepsYoutubesWhenUntrustedOrOriginal)
{
    yt::Branding branding;
    ASSERT_TRUE(yt::parse_branding(
        R"({"titles":[{"title":"Voted down","original":false,"votes":-1,"locked":false}],
            "thumbnails":[{"timestamp":null,"original":true,"votes":5,"locked":false}]})",
        &branding));
    EXPECT_TRUE(branding.title.empty());
    EXPECT_FALSE(branding.has_thumbnail);

    // A locked entry is shown whatever its votes.
    ASSERT_TRUE(yt::parse_branding(
        R"({"titles":[{"title":"Locked one","original":false,"votes":-3,"locked":true}],"thumbnails":[]})",
        &branding));
    EXPECT_EQ(branding.title, "Locked one");

    EXPECT_FALSE(yt::parse_branding("not json", &branding));
    EXPECT_EQ(yt::clean_dearrow_title("  >Kept casing  "), "Kept casing");
}

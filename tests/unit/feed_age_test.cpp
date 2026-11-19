// youtube-tv - The age on a feed card, and Subscriptions sorted by it.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/feed_age.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{

TEST(FeedAge, ReadsTheAgeAfterTheLastBullet)
{
    EXPECT_EQ(yt::age_seconds("22K views \xE2\x80\xA2 3 hours ago"), 3 * 3600);
    EXPECT_EQ(yt::age_seconds("1.2M views \xE2\x80\xA2 2 days ago"), 2 * 86400);
    EXPECT_EQ(yt::age_seconds("21y ago"), 21LL * 365 * 86400);
    EXPECT_EQ(yt::age_seconds("5 mo ago"), 5LL * 30 * 86400);
    EXPECT_EQ(yt::age_seconds("Streamed 1 day ago"), 86400);
    EXPECT_EQ(yt::age_seconds("12 views"), -1);
}

TEST(FeedAge, ReadsTheAppsLanguages)
{
    EXPECT_EQ(yt::age_seconds("hace 3 horas"), 3 * 3600);
    EXPECT_EQ(yt::age_seconds("il y a 2 semaines"), 14 * 86400);
    EXPECT_EQ(yt::age_seconds("vor 5 Minuten"), 300);
    EXPECT_EQ(yt::age_seconds("3 dni temu"), 3 * 86400);
}

TEST(FeedAge, SortsNewestFirstWithShortsLastAndNoRepeats)
{
    std::vector<yt::FeedItem> items(5);
    items[0].video_id = "a";
    items[0].detail = "10 views \xE2\x80\xA2 2 weeks ago"; // "Most relevant"
    items[0].shelf = "Most relevant";
    items[1].video_id = "b";
    items[1].detail = "5 views \xE2\x80\xA2 1 hour ago";
    items[2].video_id = "c";
    items[2].detail = "1 view \xE2\x80\xA2 3 days ago";
    items[3].video_id = "a"; // again, in the newest row
    items[3].detail = "10 views \xE2\x80\xA2 2 weeks ago";
    items[4].video_id = "s";
    items[4].detail = "9 views \xE2\x80\xA2 5 minutes ago";
    items[4].short_form = true;
    yt::sort_newest_first(&items);
    ASSERT_EQ(items.size(), 4u);
    EXPECT_EQ(items[0].video_id, "b");
    EXPECT_EQ(items[1].video_id, "c");
    EXPECT_EQ(items[2].video_id, "a");
    EXPECT_EQ(items[3].video_id, "s");
    EXPECT_TRUE(items[2].shelf.empty());
}

} // namespace

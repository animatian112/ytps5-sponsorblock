// youtube-tv - Watch history: the playback-statistics URLs.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/watch.hpp"

#include <gtest/gtest.h>

namespace
{

TEST(Watch, TheNonceIsSixteenUrlSafeCharactersAndVaries)
{
    const std::string a = yt::watch::make_cpn(1);
    const std::string b = yt::watch::make_cpn(2);
    ASSERT_EQ(a.size(), 16u);
    EXPECT_NE(a, b);
    EXPECT_EQ(
        a.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"),
        std::string::npos);
}

TEST(Watch, ThePlaybackPingKeepsTheTrackingFieldsAndDropsFexp)
{
    const std::string base =
        "https://s.youtube.com/api/stats/playback?cl=1&docid=abc&fexp=1,2,3&ei=xyz&ver=1";
    EXPECT_EQ(yt::watch::playback_ping(base, "CPNCPNCPNCPNCPN0"),
              "https://s.youtube.com/api/stats/playback?cl=1&docid=abc&ei=xyz&ver=2&cpn="
              "CPNCPNCPNCPNCPN0");
}

TEST(Watch, AccountPingsGoToWwwAndNameTheTvClient)
{
    EXPECT_EQ(yt::watch::account_host("https://s.youtube.com/api/stats/playback?docid=x"),
              "https://www.youtube.com/api/stats/playback?docid=x");
    EXPECT_EQ(yt::watch::account_host("https://example.com/a"), "https://example.com/a");
    const std::string url = yt::watch::playback_ping(
        "https://s.youtube.com/api/stats/playback?docid=abc&c=VISIONOS", "N", {"TVHTML5", "7.1"});
    EXPECT_NE(url.find("&c=TVHTML5&cver=7.1&"), std::string::npos);
    EXPECT_EQ(url.find("c=VISIONOS"), std::string::npos);
}

TEST(Watch, TheWatchtimePingNamesTheSpan)
{
    const std::string url = yt::watch::watchtime_ping(
        "https://s.youtube.com/api/stats/watchtime?docid=abc", "N", 10.0, 70.5, 300.0);
    EXPECT_EQ(url, "https://s.youtube.com/api/stats/watchtime?docid=abc&ver=2&cpn=N&st=10.000&et="
                   "70.500&cmt=70.500&len=300.000&state=playing");
}

} // namespace

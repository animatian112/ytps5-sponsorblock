// youtube-tv - Subtitles: json3 timed text.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/captions.hpp"

#include <gtest/gtest.h>

namespace
{

TEST(Captions, ReadsJson3EventsAndSkipsBareNewlines)
{
    const char *body =
        R"({"wireMagic":"pb3","events":[)"
        R"({"tStartMs":0,"dDurationMs":5000,"id":1,"wWinId":1},)"
        R"({"tStartMs":1000,"dDurationMs":2000,"segs":[{"utf8":"Hello "},{"utf8":"there"}]},)"
        R"({"tStartMs":2500,"dDurationMs":10,"aAppend":1,"segs":[{"utf8":"\n"}]},)"
        R"({"tStartMs":3000,"dDurationMs":1500,"segs":[{"utf8":"Second line\n"}]}]})";
    std::vector<yt::captions::Cue> cues;
    ASSERT_TRUE(yt::captions::parse_json3(body, &cues));
    ASSERT_EQ(cues.size(), 2u);
    EXPECT_EQ(cues[0].text, "Hello there");
    EXPECT_DOUBLE_EQ(cues[0].start_s, 1.0);
    EXPECT_DOUBLE_EQ(cues[0].end_s, 3.0);
    EXPECT_EQ(cues[1].text, "Second line");
    EXPECT_FALSE(yt::captions::parse_json3("not json", &cues));
}

} // namespace

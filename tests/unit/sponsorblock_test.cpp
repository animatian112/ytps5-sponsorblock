// youtube-tv - SponsorBlock: the request, the segment worker and the skip rules.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/sponsorblock.hpp"

#include "http_stub.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

namespace
{

namespace sb = yt::sponsorblock;
using hui::testing::HttpStub;

constexpr const char *kVideo = "dQw4w9WgXcQ";

constexpr const char *kAnswer =
    R"([{"category":"intro","actionType":"skip","segment":[0,12.5],"UUID":"a"},)"
    R"({"category":"sponsor","actionType":"skip","segment":[60,90],"UUID":"b"},)"
    R"({"category":"selfpromo","actionType":"skip","segment":[92,100],"UUID":"c"},)"
    R"({"category":"outro","actionType":"skip","segment":[580,700],"UUID":"d"}])";

void answer(long status, std::string body)
{
    HttpStub::instance().set_handler(
        [status, body](const yt::http::Request &) -> yt::http::Response
        {
            yt::http::Response response;
            response.status = status;
            response.body = body;
            return response;
        });
}

// Polls (paused, at 0 s) until the worker's answer was adopted.
bool wait_loaded(sb::Skipper &skipper)
{
    for (int i = 0; i < 400; ++i)
    {
        (void)skipper.poll(0.0, 600.0, false, sb::Mode::skip);
        if (!skipper.status().empty())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

TEST(SponsorBlock, AsksTheApiForTheVideoAndCategories)
{
    answer(200, kAnswer);
    sb::Skipper skipper;
    skipper.load(kVideo, sb::Scope::promotion);
    ASSERT_TRUE(wait_loaded(skipper));
    const auto exchanges = HttpStub::instance().exchanges();
    ASSERT_EQ(exchanges.size(), 1u);
    const std::string &url = exchanges[0].request.url;
    EXPECT_EQ(url.rfind("https://sponsor.ajay.app/api/skipSegments?videoID=dQw4w9WgXcQ", 0), 0u);
    EXPECT_NE(url.find("%22sponsor%22"), std::string::npos);
    EXPECT_NE(url.find("%22selfpromo%22"), std::string::npos);
    EXPECT_NE(url.find("%22interaction%22"), std::string::npos);
    EXPECT_EQ(url.find("%22intro%22"), std::string::npos);
    HttpStub::instance().clear();
}

TEST(SponsorBlock, SkipsOnEntryChainsNeighboursAndOnlyOnce)
{
    answer(200, kAnswer);
    sb::Skipper skipper;
    skipper.load(kVideo, sb::Scope::all);
    ASSERT_TRUE(wait_loaded(skipper));
    EXPECT_EQ(skipper.status(), "ok");
    ASSERT_EQ(skipper.segments().size(), 4u);
    EXPECT_EQ(skipper.segments()[1].label, "Sponsor");

    sb::Decision d = skipper.poll(0.5, 600.0, true, sb::Mode::skip);
    EXPECT_EQ(d.kind, sb::Kind::skip);
    EXPECT_DOUBLE_EQ(d.target_s, 12.5);
    EXPECT_EQ(d.label, "Intro");

    // Into the sponsor read: chained with the self-promotion 2 s later.
    d = skipper.poll(60.3, 600.0, true, sb::Mode::skip);
    EXPECT_EQ(d.kind, sb::Kind::skip);
    EXPECT_DOUBLE_EQ(d.target_s, 100.0);

    // Rewound into it again: skipped once only.
    d = skipper.poll(60.3, 600.0, true, sb::Mode::skip);
    EXPECT_EQ(d.kind, sb::Kind::none);

    // A segment past the end is clamped to the duration.
    d = skipper.poll(580.5, 600.0, true, sb::Mode::skip);
    EXPECT_EQ(d.kind, sb::Kind::skip);
    EXPECT_DOUBLE_EQ(d.target_s, 600.0);
    HttpStub::instance().clear();
}

TEST(SponsorBlock, PausedAskAndMarkModesDoNotSeek)
{
    answer(200, kAnswer);
    sb::Skipper skipper;
    skipper.load(kVideo, sb::Scope::all);
    ASSERT_TRUE(wait_loaded(skipper));
    EXPECT_EQ(skipper.poll(0.5, 600.0, false, sb::Mode::skip).kind, sb::Kind::none);
    EXPECT_EQ(skipper.poll(0.5, 600.0, true, sb::Mode::mark).kind, sb::Kind::none);
    EXPECT_EQ(skipper.poll(0.5, 600.0, true, sb::Mode::ask).kind, sb::Kind::ask);

    // The skip button works in the middle of a segment, in any mode.
    const sb::Decision manual = skipper.skip_now(75.0, 600.0);
    EXPECT_EQ(manual.kind, sb::Kind::skip);
    EXPECT_DOUBLE_EQ(manual.target_s, 100.0);
    EXPECT_EQ(skipper.skip_now(200.0, 600.0).kind, sb::Kind::none);
    HttpStub::instance().clear();
}

TEST(SponsorBlock, NoSegmentsErrorsAndBadIds)
{
    answer(404, "Not Found");
    sb::Skipper skipper;
    skipper.load(kVideo, sb::Scope::all);
    ASSERT_TRUE(wait_loaded(skipper));
    EXPECT_EQ(skipper.status(), "none");
    EXPECT_TRUE(skipper.segments().empty());
    EXPECT_EQ(skipper.poll(1.0, 600.0, true, sb::Mode::skip).kind, sb::Kind::none);

    answer(500, "oops");
    skipper.load(kVideo, sb::Scope::all);
    ASSERT_TRUE(wait_loaded(skipper));
    EXPECT_EQ(skipper.status(), "error: HTTP 500");

    answer(200, "[{broken");
    skipper.load(kVideo, sb::Scope::all);
    ASSERT_TRUE(wait_loaded(skipper));
    EXPECT_EQ(skipper.status(), "error: bad answer");

    HttpStub::instance().clear();
    skipper.load("not a video id", sb::Scope::all);
    EXPECT_EQ(skipper.poll(1.0, 600.0, true, sb::Mode::skip).kind, sb::Kind::none);
    EXPECT_TRUE(HttpStub::instance().exchanges().empty());
}

TEST(SponsorBlock, AnAnswerForAnOlderVideoIsDropped)
{
    // The first load's answer arrives after the second load started.
    HttpStub::instance().set_handler(
        [](const yt::http::Request &request) -> yt::http::Response
        {
            yt::http::Response response;
            if (request.url.find("AAAAAAAAAAA") != std::string::npos)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                response.status = 200;
                response.body = R"([{"category":"sponsor","segment":[1,50],"UUID":"x"}])";
            }
            else
            {
                response.status = 404;
                response.body = "Not Found";
            }
            return response;
        });
    sb::Skipper skipper;
    skipper.load("AAAAAAAAAAA", sb::Scope::all);
    skipper.load(kVideo, sb::Scope::all);
    ASSERT_TRUE(wait_loaded(skipper));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    (void)skipper.poll(0.0, 600.0, false, sb::Mode::skip);
    EXPECT_EQ(skipper.status(), "none");
    EXPECT_TRUE(skipper.segments().empty());
    HttpStub::instance().clear();
}

TEST(SponsorBlock, ScopesSelectCategories)
{
    EXPECT_NE(sb::categories_for(sb::Scope::sponsors), 0u);
    EXPECT_LT(sb::categories_for(sb::Scope::sponsors), sb::categories_for(sb::Scope::promotion));
    EXPECT_LT(sb::categories_for(sb::Scope::promotion), sb::categories_for(sb::Scope::all));
}

} // namespace

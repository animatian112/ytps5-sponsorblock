// youtube-tv - yt.log to a phone: the little web server's answers.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/log_share.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

TEST(LogShare, TheTokenPathGetsThePage)
{
    bool download = true;
    const std::string answer = yt::log_share_answer("/abc123", "abc123", "", &download);
    EXPECT_FALSE(download);
    EXPECT_EQ(answer.rfind("HTTP/1.1 200", 0), 0u);
    EXPECT_NE(answer.find("text/html"), std::string::npos);
    EXPECT_NE(answer.find("yt.log"), std::string::npos);
}

TEST(LogShare, TheFileComesAsAnAttachment)
{
    bool download = false;
    const std::string answer =
        yt::log_share_answer("/abc123/yt.log", "abc123", "line one\nline two\n", &download);
    EXPECT_TRUE(download);
    EXPECT_NE(answer.find("attachment"), std::string::npos);
    EXPECT_NE(answer.find("line two"), std::string::npos);
}

TEST(LogShare, AWrongTokenFindsNothing)
{
    bool download = true;
    const std::string answer = yt::log_share_answer("/nope/yt.log", "abc123", "secret", &download);
    EXPECT_FALSE(download);
    EXPECT_EQ(answer.rfind("HTTP/1.1 404", 0), 0u);
    EXPECT_EQ(answer.find("secret"), std::string::npos);
}

TEST(LogShare, ThePasteFormEscapesTheLog)
{
    const std::string form = yt::log_paste_form("a b&c=d\n");
    EXPECT_EQ(form.rfind("content=a+b%26c%3Dd%0A&", 0), 0u);
    EXPECT_NE(form.find("expiry_days=7"), std::string::npos);
    EXPECT_NE(form.find("syntax=text"), std::string::npos);
}

} // namespace

// youtube-tv - Where each video was left.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/resume_store.hpp"

#include <gtest/gtest.h>

namespace
{

using hui::app::ResumeStore;

TEST(ResumeStore, KeepsTheMiddleAndForgetsTheEnds)
{
    ResumeStore store({}); // no file
    store.remember("aaaaaaaaaaa", 125.0, 600.0);
    EXPECT_DOUBLE_EQ(store.position("aaaaaaaaaaa"), 125.0);
    store.remember("aaaaaaaaaaa", 4.0, 600.0); // barely started: from the start
    EXPECT_DOUBLE_EQ(store.position("aaaaaaaaaaa"), 0.0);
    store.remember("bbbbbbbbbbb", 595.0, 600.0); // watched to the end
    EXPECT_DOUBLE_EQ(store.position("bbbbbbbbbbb"), 0.0);
    store.remember("short", 50.0, 600.0); // not a video id
    EXPECT_DOUBLE_EQ(store.position("short"), 0.0);
}

TEST(ResumeStore, TheFileRoundTrips)
{
    const std::vector<std::pair<std::string, double>> entries = {{"aaaaaaaaaaa", 61.5},
                                                                 {"bbbbbbbbbbb", 3600.0}};
    const std::string text = ResumeStore::format(entries);
    EXPECT_EQ(text, "aaaaaaaaaaa 61.5\nbbbbbbbbbbb 3600.0\n");
    EXPECT_EQ(ResumeStore::parse(text), entries);
    EXPECT_TRUE(ResumeStore::parse("garbage\nxx 1\n").empty());
}

} // namespace

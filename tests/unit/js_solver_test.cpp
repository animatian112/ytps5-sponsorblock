// youtube-tv - Tests for the QuickJS n/signature solver.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/js_solver.hpp"

#include "core/save_file.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace
{

TEST(JsSolver, FindsAndReplacesTheNOfAUrl)
{
    const std::string query =
        "https://rr1.googlevideo.com/videoplayback?expire=1&n=AbC123&itag=136";
    EXPECT_EQ(yt::url_n_value(query), "AbC123");
    EXPECT_EQ(yt::url_with_n(query, "zz"),
              "https://rr1.googlevideo.com/videoplayback?expire=1&n=zz&itag=136");
    // "sn=" and friends are not the n parameter.
    EXPECT_EQ(yt::url_n_value("https://x/videoplayback?sn=1&mn=2"), "");
    const std::string last = "https://x/videoplayback?itag=1&n=QQ";
    EXPECT_EQ(yt::url_with_n(last, "RR"), "https://x/videoplayback?itag=1&n=RR");
    const std::string path = "https://x/videoplayback/id/7/n/PathN/itag/22";
    EXPECT_EQ(yt::url_n_value(path), "PathN");
    EXPECT_EQ(yt::url_with_n(path, "Done"), "https://x/videoplayback/id/7/n/Done/itag/22");
    EXPECT_EQ(yt::url_with_n("https://x/videoplayback?itag=1", "a"),
              "https://x/videoplayback?itag=1");
}

// A player's preprocessed copy kept on disk is run as it is: the solver's
// functions come from it, without fetching or parsing the player.
TEST(JsSolver, RunsAKeptPreprocessedPlayer)
{
    const std::string dir = "/tmp/hui-js-solver-test";
    (void)hui::save::ensure_directory(dir);
    const std::string url = "https://www.youtube.com/s/player/test0001/tv-player-ias.js";
    const std::string kept =
        "_result.n = function (n) { return n.split('').reverse().join('') + '_'; };"
        "_result.sig = function (s) { return s.slice(2); };";
    ASSERT_EQ(hui::save::write_atomic(dir + "/" + yt::JsSolver::cache_name(url), kept), "");

    yt::JsSolver solver(std::string(HUI_SOURCE_DIR) + "/assets/js", dir);
    std::string error;
    ASSERT_TRUE(solver.prepare(url, {}, &error)) << error;
    EXPECT_EQ(solver.player_url(), url);
    EXPECT_EQ(solver.solve_n("abc", &error), "cba_");
    EXPECT_EQ(solver.solve_sig("xyz123", &error), "z123");
    EXPECT_EQ(solver.transform_url("https://g/videoplayback?n=hello&a=1", &error),
              "https://g/videoplayback?n=olleh_&a=1");
    std::remove((dir + "/" + yt::JsSolver::cache_name(url)).c_str());
}

// A player that is not one: the error says so instead of crashing.
TEST(JsSolver, ReportsAPlayerItCannotRead)
{
    yt::JsSolver solver(std::string(HUI_SOURCE_DIR) + "/assets/js", {});
    std::string error;
    EXPECT_FALSE(
        solver.prepare("https://www.youtube.com/s/player/bad/base.js", "var nothing = 1;", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(solver.player_url().empty());
}

} // namespace

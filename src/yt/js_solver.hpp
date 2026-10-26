// youtube-tv - YouTube's own "n" and signature code, run in QuickJS.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Some answers (the TV client's, which is the one that plays age-restricted
// videos for a signed-in account) carry stream URLs whose "n" parameter must
// be transformed by the player JS before googlevideo serves them; untouched,
// every range request is refused with 403. The TV player hides that transform
// in a small VM the descrambler's JS subset cannot run (yt/decipher.cpp), so
// this runs the player itself in QuickJS-ng, through yt-dlp's EJS solver
// (assets/js, public domain), which finds the n and signature functions.
//
// The first use of a player version parses all of it (seconds); the solver's
// "preprocessed" form is then kept on disk per player, and later starts load
// that in a fraction of a second. Each answer after that is a function call.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace yt
{

class JsSolver
{
  public:
    // solver_dir holds yt.solver.lib.js and yt.solver.core.js; cache_dir
    // (may be empty) keeps the preprocessed players.
    JsSolver(std::string solver_dir, std::string cache_dir);
    ~JsSolver();
    JsSolver(const JsSolver &) = delete;
    JsSolver &operator=(const JsSolver &) = delete;

    // Readies the n and signature functions of this player (its URL names the
    // version). player_js is the file when the caller already has it; empty
    // fetches it. False with *error when it could not.
    bool prepare(const std::string &player_url, const std::string &player_js, std::string *error);
    // The player prepare() last readied, or empty.
    const std::string &player_url() const;

    // The transformed values; empty with *error on failure.
    std::string solve_n(const std::string &challenge, std::string *error);
    std::string solve_sig(const std::string &challenge, std::string *error);

    // The URL with its "n" (query "n=" or the "/n/<value>/" path part)
    // transformed. The URL unchanged when it has none; empty on failure.
    std::string transform_url(const std::string &url, std::string *error);

    // The file name a player's preprocessed copy is kept under.
    static std::string cache_name(const std::string &player_url);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The "n" value of a stream URL ("" when it has none). Exposed for the tests.
std::string url_n_value(const std::string &url);
// The URL with its "n" value replaced. Exposed for the tests.
std::string url_with_n(const std::string &url, const std::string &value);

} // namespace yt

// youtube-tv - Watch history: the playback-statistics pings YouTube's own
// players send, so a video watched here lands in the account's history.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A player answer names two URLs under playbackTracking: videostatsPlaybackUrl
// (one request when playback starts: this adds the video to the history) and
// videostatsWatchtimeUrl (the watched span: this moves the red "watched" bar
// and the resume point). Both are sent with the signed-in account's Bearer
// token, `ver=2` and a client playback nonce (`cpn`), as yt-dlp's
// _mark_watched and SmartTube do.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace yt::watch
{

// 16 characters of the alphabet YouTube's players draw a cpn from.
std::string make_cpn(std::uint64_t seed);

// `base` with the query fields in `fields` set (replacing any of the same
// name) and `fexp` dropped, which the stats endpoint refuses from a client
// that did not run those experiments.
std::string stats_url(const std::string &base,
                      const std::vector<std::pair<std::string, std::string>> &fields);

// Who is reporting: the TV client the account signed in with. YouTube credits
// a ping to the account only when it names a signed-in client (c, cver) the
// way that client's own player does.
struct Client
{
    std::string name;    // "TVHTML5"
    std::string version; // its clientVersion
};

// The tracking URLs point at s.youtube.com, which answers but only reads
// cookies; YouTube's own TV player sends the pings with its OAuth token to
// www.youtube.com, the host that reads the Authorization header.
std::string account_host(const std::string &url);

// The two requests, as URLs ready to GET. `elapsed_s` is the wall time since
// playback started (rt).
std::string playback_ping(const std::string &playback_url, const std::string &cpn,
                          const Client &client = {});
std::string watchtime_ping(const std::string &watchtime_url, const std::string &cpn, double from_s,
                           double to_s, double length_s, const Client &client = {},
                           double elapsed_s = -1.0);

} // namespace yt::watch

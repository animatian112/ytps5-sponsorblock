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

// The two requests, as URLs ready to GET.
std::string playback_ping(const std::string &playback_url, const std::string &cpn);
std::string watchtime_ping(const std::string &watchtime_url, const std::string &cpn, double from_s,
                           double to_s, double length_s);

} // namespace yt::watch

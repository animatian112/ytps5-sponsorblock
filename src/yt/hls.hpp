// youtube-tv - HLS playlists for live streams.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "yt/http.hpp"
#include "yt/types.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace yt::hls
{

// One #EXT-X-STREAM-INF entry of a master playlist.
struct Variant
{
    std::string url; // the variant's media playlist, absolute
    long long bandwidth = 0;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    std::string codecs;      // "avc1.4d401f,mp4a.40.2"
    std::string audio_group; // AUDIO="..." when the sound is a separate playlist
    std::string audio_url;   // that group's media playlist, absolute, or empty (muxed)
    int itag = 0;            // googlevideo's /itag/<n>/ in the URL, 0 if none

    Codec video_codec() const;
    bool has_video() const;
};

// Variants in playlist order; #EXT-X-MEDIA TYPE=AUDIO groups fill audio_url.
std::vector<Variant> parse_master(const std::string &text, const std::string &base_url);

struct Segment
{
    std::int64_t sequence = 0;
    double duration = 0.0;
    std::string url;
};

struct MediaPlaylist
{
    bool valid = false; // started with #EXTM3U and had no master entries
    double target_duration = 0.0;
    std::int64_t media_sequence = 0;
    bool ended = false;  // #EXT-X-ENDLIST: the broadcast is over
    std::string map_url; // #EXT-X-MAP (fragmented MP4 init), absolute
    std::vector<Segment> segments;
};

MediaPlaylist parse_media(const std::string &text, const std::string &base_url);

// `ref` against the playlist URL it appeared in.
std::string resolve(const std::string &base, const std::string &ref);

// The playable formats of a master playlist, for the ordinary stream
// selection: one video Format per variant that names a resolution.
std::vector<Format> variant_formats(const std::vector<Variant> &variants);

// Feeds a live media playlist's segments in order: each round reloads the
// playlist and hands every new segment's bytes to `emit`. Playback starts
// `back` segments behind the live edge, like players do, so the first
// seconds arrive at once and a slow segment does not starve the picture.
class LiveFeed
{
  public:
    using Fetch = std::function<http::Response(const std::string &url)>;
    using Emit = std::function<bool(const std::string &bytes)>; // false: stop

    LiveFeed(std::string playlist_url, Fetch fetch, int back = 3);

    // One round. Answers the seconds to wait before the next one, or a
    // negative value when the feed is over: ended() or error() says why.
    double round(const Emit &emit);

    bool ended() const
    {
        return ended_;
    }
    const std::string &error() const
    {
        return error_;
    }
    long error_status() const
    {
        return error_status_;
    }
    std::int64_t next_sequence() const
    {
        return next_;
    }

  private:
    double fail(std::string text, long status);

    std::string url_;
    Fetch fetch_;
    int back_ = 3;
    std::int64_t next_ = -1;
    bool map_sent_ = false;
    bool ended_ = false;
    int failures_ = 0;
    std::string error_;
    long error_status_ = 0;
};

} // namespace yt::hls

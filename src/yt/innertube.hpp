// youtube-tv - InnerTube client: browse, search, player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ad-free by construction: the player response is read for streamingData
// only. adPlacements, adSlots, playerAds and adBreakHeartbeatParams are never
// parsed, so no ad ever reaches the player. The stream URLs are the signed
// googlevideo.com URLs of the adaptive formats.

#pragma once

#include "yt/types.hpp"

#include <memory>
#include <string>

namespace yt
{

class InnerTube
{
  public:
    InnerTube();
    ~InnerTube();
    InnerTube(const InnerTube &) = delete;
    InnerTube &operator=(const InnerTube &) = delete;

    // An OAuth access token (see yt/auth.hpp) enables account pages; empty
    // means anonymous requests.
    void set_access_token(std::string token);
    bool signed_in() const;

    bool home(FeedPage *page, const std::string &continuation = {});
    bool subscriptions(FeedPage *page, const std::string &continuation = {});
    // The account's watch history (browseId FEhistory), newest first.
    bool history(FeedPage *page, const std::string &continuation = {});
    // One signed-in action ("like/like", "subscription/subscribe", ...) with
    // `extra` the request fields after the context. True on a 2xx answer.
    bool action(const char *endpoint, const std::string &extra);
    // rating: "like", "dislike" or "removelike".
    bool rate(const std::string &video_id, const std::string &rating);
    bool subscribe(const std::string &channel_id, bool on);
    bool search(const std::string &query, FeedPage *page, const std::string &continuation = {});
    bool player(const std::string &video_id, VideoInfo *info);
    // Like player(), but keeps every client's answer instead of the first one:
    // a stream URL that one client hands out can be refused by googlevideo
    // (403) while another client's URL for the same video plays. The caller
    // tries them in order. Returns false only when no client answered at all.
    bool player_variants(const std::string &video_id, std::vector<VideoInfo> *out,
                         std::string *error);
    bool related(const std::string &video_id, FeedPage *page);
    // A channel page (browseId "UC..."): header (name, avatar, subscriber
    // line), the channel's tabs and the videos of the tab the response
    // answers with.
    bool channel_page(const std::string &channel_id, ChannelPage *page);
    // Switches to one channel tab (browseId + params from ChannelTab).
    bool channel_tab(const std::string &channel_id, const std::string &params, FeedPage *page);
    // Timeout of one HTTP call, in seconds (default 20). The player path runs
    // a chain of clients, so the caller lowers it to keep the total bounded.
    void set_timeout(long seconds);

    // HTTP status of the last call; 401 means the token has to be refreshed.
    long last_status() const;
    const std::string &last_error() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// JSON parsers over one InnerTube response, exposed for the unit tests.
// Feed pages also collect the channels a search answered with (page->channels,
// from channelRenderer objects: channelId, avatar thumbnail, title, subscriber
// line) next to the videos; the walker skips ad renderers as before.
bool parse_feed_page(std::string_view json, FeedPage *page);
bool parse_video_info(std::string_view json, VideoInfo *info);
// A one-line census of one answer's formats ("24 formats: 12 avc, ...; max
// 1080p, host rr5---sn-x.googlevideo.com"): the per-client log line and the
// no-stream failure text both use it. The plain/ciphered counts and the URL
// host make the kind of URL the answer carries visible in yt.log.
std::string format_summary(const std::vector<Format> &formats);
// One short line naming a format and the kind of URL it carries:
// "itag=137 1080p AVC (ciphered)". The service puts it in front of the ffmpeg
// error text when an open fails, so a 403 names the format and its kind
// instead of the bare demuxer string. nullptr becomes "none".
std::string describe_format(const Format *format);
// The player's signature timestamp (sts): yt-dlp's pattern over the player JS
// ("signatureTimestamp":12345 or "sts":12345), without pulling in <regex>.
// Empty when the snippet carries none. Exposed for the unit tests.
std::string signature_timestamp(std::string_view js);
// A channel browse answer: header, tabs and the selected tab's videos. Also
// exposed for the unit tests.
bool parse_channel_page(std::string_view json, ChannelPage *page);

} // namespace yt

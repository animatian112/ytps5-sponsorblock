// youtube-tv - InnerTube client: browse, search, player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ad-free by construction: the player response is read for streamingData
// only. adPlacements, adSlots, playerAds and adBreakHeartbeatParams are never
// parsed, so no ad ever reaches the player. The stream URLs are the signed
// googlevideo.com URLs of the adaptive formats.

#pragma once

#include "yt/comments.hpp"
#include "yt/types.hpp"

#include <memory>
#include <string>

namespace yt
{

// YouTube's search filter parameter (the "params" of /search): sort 0
// relevance, 1 upload date, 2 view count, 3 rating; date 0 any, 1 last hour,
// 2 today, 3 this week, 4 this month, 5 this year; duration 0 any, 1 under
// 4 minutes, 2 4-20 minutes, 3 over 20 minutes. Empty when nothing is set.
std::string search_params(int sort, int date, int duration);

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
    // Any browse page by id (a topic channel such as Music or Gaming).
    bool browse(const std::string &browse_id, FeedPage *page, const std::string &continuation = {});
    // The account's watch history (browseId FEhistory), newest first.
    bool history(FeedPage *page, const std::string &continuation = {});
    // The signed-in account's display name and avatar URL.
    bool account(std::string *name, std::string *photo, std::string *channel_id = nullptr);
    // One signed-in action ("like/like", "subscription/subscribe", ...) with
    // `extra` the request fields after the context. True on a 2xx answer.
    bool action(const char *endpoint, const std::string &extra);
    // The playbackTracking URLs of a signed-in TV player answer.
    bool tracking(const std::string &video_id, std::string *playback, std::string *watchtime);
    // rating: "like", "dislike" or "removelike".
    bool rate(const std::string &video_id, const std::string &rating);
    bool subscribe(const std::string &channel_id, bool on);
    // "Not interested" / "Don't recommend channel": one token from a card.
    bool send_feedback(const std::string &token);
    // Adds a video to one of the account's playlists ("WL" = Watch later).
    bool add_to_playlist(const std::string &playlist_id, const std::string &video_id);
    // A new private playlist named `title`, holding the video.
    bool create_playlist(const std::string &title, const std::string &video_id);
    bool search(const std::string &query, FeedPage *page, const std::string &continuation = {});
    // The Shorts feed, as SmartTube builds it: the Shorts shelf of the
    // signed-in subscriptions (TV), then YouTube's endless Shorts sequence
    // (WEB reel endpoints), each Short's title from its own reel answer. The
    // continuation is the next sequence token.
    bool shorts(FeedPage *page, const std::string &continuation = {});
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
    // The same tab asked of the signed-out web client (the TV answer of a
    // channel page names no tabs), and its next pages by continuation.
    bool channel_tab_web(const std::string &channel_id, const std::string &params,
                         const std::string &continuation, FeedPage *page);
    // Timeout of one HTTP call, in seconds (default 20). The player path runs
    // a chain of clients, so the caller lowers it to keep the total bounded.
    void set_timeout(long seconds);
    // The country and language the feeds are answered for ("US", "en").
    void set_region(const std::string &gl, const std::string &hl);
    // A page of a video's comments (web /next); an empty continuation asks
    // for the first page. next is the following page's token ("" at the end).
    bool comments(const std::string &video_id, const std::string &continuation,
                  std::vector<Comment> *out, std::string *next);
    // The search filters (search_params()) every new search carries.
    void set_search_params(const std::string &params);

    // HTTP status of the last call; 401 means the token has to be refreshed.
    long last_status() const;
    const std::string &last_error() const;
    // The TV player JS the TV client's signature timestamp came from (empty
    // until a TV request looked it up): the player whose "n" its URLs need.
    const std::string &tv_player_url() const;

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

// The TVHTML5 clientVersion the signed-in requests use (the history pings
// name it too).
const char *tv_client_version();
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

// One entry of a reel_watch_sequence answer: the Short, the params its own
// reel_item_watch request needs, and its (portrait) thumbnail.
struct ReelEntry
{
    std::string video_id;
    std::string params;
    std::string thumbnail_url;
};
// A reel_item_watch answer: the Short it names (id, title, channel, line,
// thumbnail) and the token of the sequence that follows it.
bool parse_reel_item(std::string_view json, FeedItem *item, std::string *sequence);
// A reel_watch_sequence answer: its Shorts and the next token.
bool parse_reel_sequence(std::string_view json, std::vector<ReelEntry> *entries, std::string *next);

} // namespace yt

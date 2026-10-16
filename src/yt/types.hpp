// youtube-tv - Shared data types of the YouTube layer.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The yt/ directory is plain C++20 with no PS5, OpenGL or kit dependency, so
// the same code runs in the console build, in the PC snapshot build and in
// the host unit tests.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace yt
{

// Media URLs from YouTube sometimes arrive protocol-relative ("//yt3.ggpht.com/
// ..."), which a plain libcurl request cannot open: give them a scheme.
std::string absolute_url(std::string_view url);

// Codec of one entry of streamingData (video formats and audio formats share
// the list, as YouTube's DASH response mixes them).
enum class Codec : std::uint8_t
{
    unknown,
    avc,  // video/mp4; codecs="avc1...."
    vp9,  // video/webm; codecs="vp9"
    av1,  // video/mp4; codecs="av01...."
    aac,  // audio/mp4; codecs="mp4a.40.2"
    opus, // audio/webm; codecs="opus"
};

bool is_video_codec(Codec codec);
bool is_audio_codec(Codec codec);
// "AVC", "VP9", "AV1", "AAC", "Opus", "?" - for the player overlay.
const char *codec_label(Codec codec);

struct Format
{
    int itag = 0;
    std::string url;
    Codec codec = Codec::unknown;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    long long bitrate = 0;
    long long content_length = 0;
    int sample_rate = 0;
    // A format gated behind the player's signature arrives as signatureCipher
    // (older answers: cipher) - the unsigned base URL plus the scrambled
    // signature and the query parameter it belongs to. `url` then carries the
    // same base URL so stream selection still sees an ordinary candidate, and
    // `ciphered` marks it: yt::Descrambler has to turn s/sp into the playable
    // URL before it is opened.
    std::string cipher_url;
    std::string cipher_s;
    std::string cipher_sp;
    bool ciphered = false;
    // Videos with dubbed sound carry one audio format per language:
    // audioTrack.id ("en.4"), its displayName ("English (United States)
    // original") and audioIsDefault for the track YouTube would play.
    std::string audio_track_id;
    std::string audio_track_name;
    bool audio_default = false;
    bool drc = false; // a dynamic-range-compressed copy of another format
};

// One subtitle track of a video (captions.playerCaptionsTracklistRenderer).
struct CaptionTrack
{
    std::string url;      // baseUrl; "&fmt=json3" gives the timed text as JSON
    std::string language; // "en"
    std::string name;     // "English (auto-generated)"
    bool automatic = false;
};

// One selectable sound track of a video.
struct AudioTrack
{
    std::string id;
    std::string name;
    bool is_default = false;
};

struct VideoInfo
{
    std::string video_id;
    std::string title;
    std::string author;
    double length_s = 0.0;
    bool live = false;
    std::vector<Format> formats;
    // The InnerTube client that answered, filled by player_variants(): the
    // player screen names it once this client's stream is the one playing.
    std::string client;
    // The player JS this answer's ciphers need (assets.js, or built from
    // assets.jsUrlHash). Empty when the answer carries neither.
    std::string player_js_url;
    // A compact census of the raw streamingData entries ("18 adaptive, 0 with
    // url, 0 with cipher, 0 drm, sabr-url present"): counting before any format
    // is dropped makes a SABR-only answer conclusive. Filled whether or not a
    // format was usable.
    std::string census;
    // True when the answer offered no usable format and looks SABR-only:
    // streamingData.serverAbrStreamingUrl is present, or raw adaptive entries
    // carry neither url nor cipher (yt-dlp issue #12482).
    bool sabr_only = false;
    // The untouched answer body, kept only when nothing was usable, capped at
    // 1 MiB, so a SABR-only failure can be reproduced on the console.
    std::string raw_body;
    // Seconds googlevideo refuses this answer's stream URLs (403) after the
    // answer was handed out: the length of the pre-roll ads it scheduled, or
    // their skip offset (yt-dlp `_get_available_at_timestamp`). 0 = none.
    double preroll_wait_s = 0.0;
    // playbackTracking: the stats URLs that put a watched video in the
    // account's history (see yt/watch.hpp). Empty when the answer has none.
    std::string playback_url;
    std::string watchtime_url;
    std::string channel_id;  // videoDetails.channelId
    std::string description; // videoDetails.shortDescription
    std::string view_count;  // videoDetails.viewCount, digits
    std::vector<CaptionTrack> captions;
    int preroll_ads = 0;
    // std::chrono::steady_clock milliseconds when the URLs start to play:
    // the answer's arrival plus preroll_wait_s. Filled by player_variants().
    long long available_at_ms = 0;
    std::string error; // empty means the response was usable

    bool ok() const
    {
        return error.empty();
    }
};

// A card in a feed. A playlist is a FeedItem too: its video_id is then the
// playlist's browse id ("VL" + playlist id), which is_playlist() tells.
struct FeedItem
{
    std::string video_id;
    std::string title;
    std::string author;
    std::string thumbnail_url;
    double length_s = 0.0;
    bool live = false;
    // "1.2M views • 3 days ago", as the feed words it; empty when absent.
    std::string detail;
    // The shelf the feed placed it on ("Recommended", "Trending"); empty for
    // a flat feed. Consecutive items with the same shelf form one row.
    std::string shelf;
    // 0..1 of it already watched (YouTube's red bar), -1 for none.
    float watched = -1.0f;

    bool is_playlist() const
    {
        return video_id.size() > 2 && video_id[0] == 'V' && video_id[1] == 'L';
    }
};

// A channel as search answers with it (avatar, name, subscriber line).
struct ChannelItem
{
    std::string channel_id; // "UC..." for /browse
    std::string name;
    std::string thumbnail_url;
    std::string subscribers; // "1.2M subscribers", as published
};

// One tab of a channel page ("Home", "Videos", "Live", "Playlists", ...).
// The list comes from the channel's own header response; the params string is
// what a browse request needs to switch to it.
struct ChannelTab
{
    std::string title;
    std::string params;
    bool selected = false; // the tab the first response already answered with
};

// A channel page: header, its tabs and the videos of the selected tab.
struct ChannelPage
{
    ChannelItem channel;
    std::vector<ChannelTab> tabs;
    std::vector<FeedItem> items;
    std::string continuation;
    std::string error;
    bool ok() const
    {
        return error.empty();
    }
};

struct FeedPage
{
    std::vector<FeedItem> items;
    // Channels the same response carried (search only, in encounter order).
    std::vector<ChannelItem> channels;
    std::string continuation; // empty = no further page
    std::string error;
    // While parsing: the shelf being walked, stamped on its items.
    std::string current_shelf;
    // A watch page (/next) also says how the account stands to the video:
    // its channel, whether it is subscribed, and "LIKE" / "DISLIKE" /
    // "INDIFFERENT". meta_known is false when the answer named none of it.
    bool meta_known = false;
    std::string channel_id;
    bool subscribed = false;
    std::string like_status;
    bool ok() const
    {
        return error.empty();
    }
};

// Which decoder should turn the selected stream into frames. auto_detect uses
// the console decoder where the console decodes the codec (AVC, VP9) and falls
// back to software for the rest (AV1); software always decodes on the CPU;
// hardware forces the console decoder and refuses codecs it cannot take.
enum class DecodeMode
{
    auto_detect = 0,
    software = 1,
    hardware = 2,
};

// The three codec switches of the settings screen, plus the resolution cap and
// the decoder the player should use.
struct CodecChoice
{
    bool avc = true;
    bool vp9 = true;
    bool av1 = true;
    int max_height = 1080;
    DecodeMode decode = DecodeMode::auto_detect;
    // audioTrack.id to play; empty picks the video's original track.
    std::string audio_track;
};

// Load state of anything that arrives from the network.
enum class Load : unsigned char
{
    idle,
    loading,
    ready,
    failed,
};

// The one screen of the app: what the shell shows.
enum class Screen : std::uint8_t
{
    home,
    search,
    subscriptions,
    settings,
    player,
    onboard, // the login screen
};

} // namespace yt

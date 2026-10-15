// youtube-tv - InnerTube client: browse, search, player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ad-free by construction: the response is walked for video renderers and for
// streamingData only. adPlacements, adSlots, playerAds and
// adBreakHeartbeatParams are never read, and ad renderer subtrees are skipped,
// so no ad ever reaches the player.
//
// YouTube renames response shapes without notice, so the feed parser is a
// tolerant walker over the whole tree (any object with a string videoId and a
// title is an item) rather than a fixed path. Both parsers are the seam the
// unit tests use.

#include "yt/innertube.hpp"

#include "yt/http.hpp"
#include "yt/log.hpp"

#include <algorithm>

#include "third_party/yyjson/yyjson.h"

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace yt
{

namespace
{

// ---- protocol constants ---------------------------------------------------------------------

constexpr const char *kBaseUrl = "https://www.youtube.com/youtubei/v1/";
constexpr const char *kApiKey = "?key=AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";
constexpr const char *kTvClientName = "TVHTML5";
// Mirrors yt-dlp master `_base.py`'s `tv` block: the Cobalt TVHTML5 version a
// maintained client ships. A stale one answers 400/410, which drops the caller
// to WEB.
constexpr const char *kTvClientVersion = "7.20260707.07.00";
constexpr const char *kWebClientName = "WEB";
constexpr const char *kWebClientVersion = "2.20260623.01.00";

// One InnerTube client. `extra` is appended inside context.client (no leading
// comma); `ua` is the HTTP User-Agent and, for the clients that repeat it, the
// value of context.client.userAgent; `bearer` marks the clients that accept
// the OAuth Bearer token - the TV family only, since YouTube refuses it on WEB
// and VISIONOS needs none. None of the clients here sends thirdParty.embedUrl.
struct Client
{
    const char *name;
    const char *version;
    const char *ua;
    const char *extra;
    bool ua_in_context;
    bool bearer;
};

// The three User-Agents yt-dlp master `_base.py` ships, copied verbatim.
constexpr const char *kTvUserAgent =
    "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034943-gold (unlike Gecko), "
    "Unknown_TV_Unknown_0/Unknown (Unknown, Unknown)";
constexpr const char *kSamsungUserAgent =
    "Mozilla/5.0 (SMART-TV; Linux; Tizen 2.4.0) AppleWebKit/538.1 (KHTML, like Gecko) "
    "Version/2.4.0 TV Safari/538.1";
constexpr const char *kVisionOsUserAgent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) "
    "Version/26.0 Safari/605.1.15";

// The client the auth state selects for browse/search/next: third-party OAuth
// only gets content from the TV client, while signed out only WEB answers.
constexpr Client kWebClient = {kWebClientName, kWebClientVersion, kTvUserAgent, nullptr, false,
                               false};
constexpr Client kTvClient = {kTvClientName, kTvClientVersion,
                              kTvUserAgent,  "\"deviceMake\":\"Sony\",\"deviceModel\":\"PS5\"",
                              false,         true};
// /player walks the clients yt-dlp master ships for the SABR era, in the order
// that answers first: VISIONOS (cookie-less, PO-token-less, still hands out
// per-format URLs), then the two TV shells that take the Bearer token, then
// WEB. Every context.client repeats the HTTP User-Agent; no client sends
// thirdParty.embedUrl. ANDROID_VR and IOS are gone: the first 403s every format
// since 2026-08-17 (yt-dlp issue #17348), the second needs a player PO token.
constexpr Client kPlayerClients[] = {
    {"VISIONOS", "1.02", kVisionOsUserAgent,
     "\"deviceMake\":\"Apple\",\"deviceModel\":\"RealityDevice17,1\",\"osName\":\"visionOS\","
     "\"osVersion\":\"26.5.23O471\"",
     true, false},
    {kTvClientName, "5.20260707", kSamsungUserAgent,
     "\"deviceMake\":\"Samsung\",\"deviceModel\":\"UKS9800\",\"osName\":\"Tizen\","
     "\"osVersion\":\"2.4.0\"",
     true, true},
    {kTvClientName, kTvClientVersion, kTvUserAgent, nullptr, true, true},
    {kWebClientName, kWebClientVersion, kTvUserAgent, nullptr, false, false},
};
// YouTube answers a consent dialog instead of a feed in the EU until the
// visitor answers it. This is the "reject" answer yt-dlp and friends send;
// with it the dialog is gone (verified against the live service).
constexpr const char *kConsentCookie = "SOCS=CAI";
constexpr const std::size_t kMaxItems = 200;
// A search answer carries a handful of channels; the cap only guards a
// pathological response.
constexpr const std::size_t kMaxChannels = 24;
// A channel page carries a handful of tabs; the cap only guards a pathological
// response.
constexpr const std::size_t kMaxTabs = 16;
// The untouched player answer kept on a stream-less result is bounded: enough
// to diagnose a SABR-only answer, not a memory hazard.
constexpr const std::size_t kMaxRawBody = 1u << 20;
// One player JS is script, a few MiB; the signature timestamp is a needle in
// it, so the download is capped like the descrambler caps its own.
constexpr const std::size_t kMaxPlayerJsBytes = 3u << 20;

// ---- tolerant JSON readers ------------------------------------------------------------------

std::string string_field(yyjson_val *obj, const char *key)
{
    yyjson_val *value = yyjson_obj_get(obj, key);
    if (yyjson_is_str(value))
    {
        const char *text = yyjson_get_str(value);
        if (text != nullptr)
            return text;
    }
    return {};
}

// Numbers arrive as JSON numbers or, in several InnerTube fields, as strings.
double number_field(yyjson_val *obj, const char *key, double fallback)
{
    yyjson_val *value = yyjson_obj_get(obj, key);
    if (yyjson_is_num(value))
        return yyjson_get_num(value);
    if (yyjson_is_str(value))
    {
        const char *text = yyjson_get_str(value);
        if (text != nullptr && *text != '\0')
            return std::strtod(text, nullptr);
    }
    return fallback;
}

long long integer_field(yyjson_val *obj, const char *key, long long fallback)
{
    yyjson_val *value = yyjson_obj_get(obj, key);
    if (yyjson_is_num(value))
        return static_cast<long long>(yyjson_get_num(value));
    if (yyjson_is_str(value))
    {
        const char *text = yyjson_get_str(value);
        if (text != nullptr && *text != '\0')
            return std::strtoll(text, nullptr, 10);
    }
    return fallback;
}

// A runs[0].text / simpleText text object ("title", "ownerText", ...).
std::string renderer_text(yyjson_val *container)
{
    yyjson_val *runs = yyjson_obj_get(container, "runs");
    if (yyjson_is_arr(runs))
    {
        yyjson_val *first = yyjson_arr_get_first(runs);
        yyjson_val *text = yyjson_obj_get(first, "text");
        if (yyjson_is_str(text))
            return yyjson_get_str(text);
    }
    yyjson_val *simple = yyjson_obj_get(container, "simpleText");
    if (yyjson_is_str(simple))
        return yyjson_get_str(simple);
    return {};
}

std::string title_text(yyjson_val *obj)
{
    static const char *const keys[] = {"title", "headline"};
    for (const char *key : keys)
    {
        yyjson_val *value = yyjson_obj_get(obj, key);
        if (yyjson_is_str(value))
        {
            const char *text = yyjson_get_str(value);
            if (text != nullptr && *text != '\0')
                return text;
        }
        else if (yyjson_is_obj(value))
        {
            std::string text = renderer_text(value);
            if (!text.empty())
                return text;
        }
    }
    return {};
}

std::string author_text(yyjson_val *obj)
{
    static const char *const keys[] = {"ownerText", "shortBylineText", "longBylineText"};
    for (const char *key : keys)
    {
        yyjson_val *value = yyjson_obj_get(obj, key);
        if (yyjson_is_obj(value))
        {
            std::string text = renderer_text(value);
            if (!text.empty())
                return text;
        }
    }
    return {};
}

// channelRenderer title: title.simpleText first, then title.runs[0].text.
std::string channel_title(yyjson_val *obj)
{
    yyjson_val *title = yyjson_obj_get(obj, "title");
    yyjson_val *simple = yyjson_obj_get(title, "simpleText");
    if (yyjson_is_str(simple))
    {
        const char *text = yyjson_get_str(simple);
        if (text != nullptr)
            return text;
    }
    return renderer_text(title);
}

// channelRenderer subscriber line. All three shapes occur in the wild:
// videoCountText.simpleText, subscriberCountText.simpleText, and either of
// those as runs[0].text. simpleText of either key wins over a runs form.
std::string subscriber_text(yyjson_val *obj)
{
    static const char *const keys[] = {"videoCountText", "subscriberCountText"};
    for (const char *key : keys)
    {
        yyjson_val *value = yyjson_obj_get(obj, key);
        yyjson_val *simple = yyjson_obj_get(value, "simpleText");
        if (yyjson_is_str(simple))
        {
            const char *text = yyjson_get_str(simple);
            if (text != nullptr && *text != '\0')
                return text;
        }
    }
    for (const char *key : keys)
    {
        std::string text = renderer_text(yyjson_obj_get(obj, key));
        if (!text.empty())
            return text;
    }
    return {};
}

// The largest entry of a thumbnails array by width; the last entry when none
// publishes a width.
std::string largest_thumbnail(yyjson_val *entries)
{
    const std::size_t count = yyjson_arr_size(entries);
    if (count == 0)
        return {};

    yyjson_val *best = yyjson_arr_get_last(entries);
    double best_width = -1.0;
    for (std::size_t index = 0; index < count; ++index)
    {
        yyjson_val *entry = yyjson_arr_get(entries, index);
        yyjson_val *width = yyjson_obj_get(entry, "width");
        const double value = yyjson_is_num(width) ? yyjson_get_num(width) : -1.0;
        if (value > best_width)
        {
            best_width = value;
            best = entry;
        }
    }

    // Channel avatars arrive protocol-relative ("//yt3.ggpht.com/..."), which
    // libcurl cannot open, so every thumbnail/sources URL leaves here with a
    // scheme.
    yyjson_val *url = yyjson_obj_get(best, "url");
    if (yyjson_is_str(url))
    {
        const char *text = yyjson_get_str(url);
        if (text != nullptr)
            return absolute_url(text);
    }
    return {};
}

// A thumbnail URL together with the width it publishes; an entry without a
// width counts as width 0.
struct AvatarCandidate
{
    std::string url;
    double width = 0.0;
};

// The widest entry of a thumbnails/sources array as a candidate, its URL
// normalised. Ties go to the first encountered.
AvatarCandidate widest_avatar(yyjson_val *entries)
{
    AvatarCandidate best;
    const std::size_t count = yyjson_arr_size(entries);
    for (std::size_t index = 0; index < count; ++index)
    {
        yyjson_val *entry = yyjson_arr_get(entries, index);
        yyjson_val *url = yyjson_obj_get(entry, "url");
        if (!yyjson_is_str(url))
            continue;
        yyjson_val *width = yyjson_obj_get(entry, "width");
        const double value = yyjson_is_num(width) ? yyjson_get_num(width) : 0.0;
        if (best.url.empty() || value > best.width)
        {
            best.url = absolute_url(yyjson_get_str(url));
            best.width = value;
        }
    }
    return best;
}

// Keep whichever candidate publishes the larger width; on a tie the first
// encountered stays.
void keep_widest(AvatarCandidate *best, const AvatarCandidate &candidate)
{
    if (!candidate.url.empty() && (best->url.empty() || candidate.width > best->width))
    {
        *best = candidate;
    }
}

std::string thumbnail_text(yyjson_val *obj)
{
    yyjson_val *thumbnail = yyjson_obj_get(obj, "thumbnail");
    return largest_thumbnail(yyjson_obj_get(thumbnail, "thumbnails"));
}

// "1:02:03" or "12:34" to seconds; anything else is 0.
double parse_clock(const char *text)
{
    if (text == nullptr)
        return 0.0;
    double total = 0.0;
    const char *cursor = text;
    for (;;)
    {
        char *end = nullptr;
        const long value = std::strtol(cursor, &end, 10);
        if (end == cursor)
            return 0.0;
        total = total * 60.0 + static_cast<double>(value);
        cursor = end;
        if (*cursor != ':')
            break;
        ++cursor;
    }
    return total;
}

double length_seconds(yyjson_val *obj)
{
    yyjson_val *text = yyjson_obj_get(obj, "lengthText");
    yyjson_val *simple = yyjson_obj_get(text, "simpleText");
    if (yyjson_is_str(simple))
        return parse_clock(yyjson_get_str(simple));
    return number_field(obj, "lengthSeconds", 0.0);
}

bool is_live(yyjson_val *obj)
{
    yyjson_val *badges = yyjson_obj_get(obj, "badges");
    const std::size_t badge_count = yyjson_arr_size(badges);
    for (std::size_t index = 0; index < badge_count; ++index)
    {
        yyjson_val *badge = yyjson_arr_get(badges, index);
        yyjson_val *renderer = yyjson_obj_get(badge, "metadataBadgeRenderer");
        yyjson_val *badge_style = yyjson_obj_get(renderer, "style");
        if (yyjson_is_str(badge_style) &&
            std::strcmp(yyjson_get_str(badge_style), "BADGE_STYLE_TYPE_LIVE_NOW") == 0)
            return true;
    }

    yyjson_val *live = yyjson_obj_get(obj, "isLiveNow");
    if (yyjson_is_bool(live) && yyjson_get_bool(live))
        return true;

    yyjson_val *overlays = yyjson_obj_get(obj, "thumbnailOverlays");
    const std::size_t overlay_count = yyjson_arr_size(overlays);
    for (std::size_t index = 0; index < overlay_count; ++index)
    {
        yyjson_val *overlay = yyjson_arr_get(overlays, index);
        yyjson_val *status = yyjson_obj_get(overlay, "thumbnailOverlayTimeStatusRenderer");
        yyjson_val *overlay_style = yyjson_obj_get(status, "style");
        if (yyjson_is_str(overlay_style) && std::strcmp(yyjson_get_str(overlay_style), "LIVE") == 0)
            return true;
    }

    yyjson_val *direct = yyjson_obj_get(obj, "thumbnailOverlayTimeStatusRenderer");
    yyjson_val *direct_style = yyjson_obj_get(direct, "style");
    if (yyjson_is_str(direct_style) && std::strcmp(yyjson_get_str(direct_style), "LIVE") == 0)
        return true;
    return false;
}

// The ad renderers whose subtrees are never entered.
bool is_ad_key(const char *name)
{
    static const char *const keys[] = {
        "adSlotRenderer",
        "adVideoRenderer",
        "playerAdRenderer",
        "promotedVideoRenderer",
        "displayAdRenderer",
        "promotedSparklesWebRenderer",
        "compactPromotedVideoRenderer",
        "inFeedAdLayoutRenderer",
    };
    for (const char *key : keys)
    {
        if (std::strcmp(name, key) == 0)
            return true;
    }
    return false;
}

// ---- the feed walker ------------------------------------------------------------------------

yyjson_val *find_object(yyjson_val *value, const char *key);

// Case-insensitive "haystack contains needle".
bool contains_text(const char *haystack, const char *needle)
{
    if (haystack == nullptr)
        return false;
    const std::size_t needle_len = std::strlen(needle);
    if (needle_len == 0)
        return true;
    for (const char *cursor = haystack; *cursor != '\0'; ++cursor)
    {
        std::size_t index = 0;
        while (index < needle_len && std::tolower(static_cast<unsigned char>(cursor[index])) ==
                                         std::tolower(static_cast<unsigned char>(needle[index])))
            ++index;
        if (index == needle_len)
            return true;
    }
    return false;
}

// Case-insensitive equality ("LIVE").
bool equals_text(const char *text, const char *other)
{
    if (text == nullptr)
        return false;
    std::size_t index = 0;
    for (; text[index] != '\0' && other[index] != '\0'; ++index)
    {
        if (std::tolower(static_cast<unsigned char>(text[index])) !=
            std::tolower(static_cast<unsigned char>(other[index])))
            return false;
    }
    return text[index] == '\0' && other[index] == '\0';
}

// The modern feed wraps a video in a lockupViewModel: richItemRenderer.content
// carries one, and the lockup object itself (contentId + metadata.
// lockupMetadataViewModel) is reached once the walker enters that content.
yyjson_val *resolve_lockup(yyjson_val *obj)
{
    yyjson_val *view_model = yyjson_obj_get(obj, "lockupViewModel");
    if (yyjson_is_obj(view_model))
        return view_model;

    yyjson_val *content_id = yyjson_obj_get(obj, "contentId");
    yyjson_val *metadata = yyjson_obj_get(obj, "metadata");
    if (yyjson_is_str(content_id) &&
        yyjson_is_obj(yyjson_obj_get(metadata, "lockupMetadataViewModel")))
        return obj;
    return nullptr;
}

// lockupMetadataViewModel.title: content, then simpleText, then runs[0].text.
std::string lockup_title(yyjson_val *metadata)
{
    yyjson_val *title = yyjson_obj_get(metadata, "title");
    yyjson_val *content = yyjson_obj_get(title, "content");
    if (yyjson_is_str(content))
    {
        const char *text = yyjson_get_str(content);
        if (text != nullptr && *text != '\0')
            return text;
    }
    yyjson_val *simple = yyjson_obj_get(title, "simpleText");
    if (yyjson_is_str(simple))
    {
        const char *text = yyjson_get_str(simple);
        if (text != nullptr && *text != '\0')
            return text;
    }
    return renderer_text(title);
}

// The author line of a lockup: the first metadata row part that is neither a
// view/subscriber count nor an upload age. Channel "Videos" tabs carry only
// counts and an age, so those lockups leave the field empty.
std::string lockup_author(yyjson_val *metadata)
{
    yyjson_val *content = yyjson_obj_get(metadata, "metadata");
    yyjson_val *view_model = yyjson_obj_get(content, "contentMetadataViewModel");
    yyjson_val *rows = yyjson_obj_get(view_model, "metadataRows");
    const std::size_t row_count = yyjson_arr_size(rows);
    for (std::size_t row = 0; row < row_count; ++row)
    {
        yyjson_val *parts = yyjson_obj_get(yyjson_arr_get(rows, row), "metadataParts");
        const std::size_t part_count = yyjson_arr_size(parts);
        for (std::size_t part = 0; part < part_count; ++part)
        {
            yyjson_val *text = yyjson_obj_get(yyjson_arr_get(parts, part), "text");
            yyjson_val *content_text = yyjson_obj_get(text, "content");
            const char *line = yyjson_is_str(content_text) ? yyjson_get_str(content_text) : nullptr;
            if (line == nullptr || *line == '\0')
                continue;
            if (contains_text(line, "views") || contains_text(line, "subscribers") ||
                contains_text(line, "watching"))
                continue;
            const std::size_t length = std::strlen(line);
            if (length >= 3 && contains_text(line + length - 3, "ago"))
                continue;
            return line;
        }
    }
    return {};
}

// The subscriber row of a lockup metadata: the metadataParts[0].text.content
// of the row whose text ends in "subscribers".
std::string lockup_subscriber_text(yyjson_val *metadata)
{
    yyjson_val *content = yyjson_obj_get(metadata, "metadata");
    yyjson_val *view_model = yyjson_obj_get(content, "contentMetadataViewModel");
    yyjson_val *rows = yyjson_obj_get(view_model, "metadataRows");
    const std::size_t row_count = yyjson_arr_size(rows);
    for (std::size_t row = 0; row < row_count; ++row)
    {
        yyjson_val *parts = yyjson_obj_get(yyjson_arr_get(rows, row), "metadataParts");
        yyjson_val *text = yyjson_obj_get(yyjson_arr_get_first(parts), "text");
        yyjson_val *content_text = yyjson_obj_get(text, "content");
        const char *line = yyjson_is_str(content_text) ? yyjson_get_str(content_text) : nullptr;
        if (line == nullptr || *line == '\0')
            continue;
        const std::size_t length = std::strlen(line);
        if (length >= 11 && equals_text(line + length - 11, "subscribers"))
            return line;
    }
    return {};
}

std::string sources_url(yyjson_val *thumbnail_view_model)
{
    yyjson_val *image = yyjson_obj_get(thumbnail_view_model, "image");
    return largest_thumbnail(yyjson_obj_get(image, "sources"));
}

// contentImage.thumbnailViewModel.image.sources; a second look for any other
// thumbnailViewModel the lockup nests.
std::string lockup_thumbnail(yyjson_val *lockup)
{
    yyjson_val *content_image = yyjson_obj_get(lockup, "contentImage");
    std::string url = sources_url(yyjson_obj_get(content_image, "thumbnailViewModel"));
    if (!url.empty())
        return url;
    return sources_url(find_object(lockup, "thumbnailViewModel"));
}

// The first badge text that is a duration (M:SS / H:MM:SS), or "LIVE" which
// sets the live flag instead.
double lockup_length(yyjson_val *thumbnail_view_model, bool *live)
{
    yyjson_val *overlays = yyjson_obj_get(thumbnail_view_model, "overlays");
    const std::size_t overlay_count = yyjson_arr_size(overlays);
    for (std::size_t o = 0; o < overlay_count; ++o)
    {
        yyjson_val *bottom =
            yyjson_obj_get(yyjson_arr_get(overlays, o), "thumbnailBottomOverlayViewModel");
        yyjson_val *badges = yyjson_obj_get(bottom, "badges");
        const std::size_t badge_count = yyjson_arr_size(badges);
        for (std::size_t b = 0; b < badge_count; ++b)
        {
            yyjson_val *badge =
                yyjson_obj_get(yyjson_arr_get(badges, b), "thumbnailBadgeViewModel");
            const std::string style = string_field(badge, "badgeStyle");
            if (contains_text(style.c_str(), "live"))
                *live = true;

            yyjson_val *text = yyjson_obj_get(badge, "text");
            const char *label = yyjson_is_str(text) ? yyjson_get_str(text) : nullptr;
            if (label == nullptr || *label == '\0')
                continue;
            if (equals_text(label, "LIVE"))
            {
                *live = true;
                return 0.0;
            }
            if (std::strchr(label, ':') != nullptr)
            {
                const double seconds = parse_clock(label);
                if (seconds > 0.0)
                    return seconds;
            }
        }
    }
    return 0.0;
}

// A lockupViewModel video: contentId (11-char video id), title, thumbnail,
// duration badge and author line. Playlists, channels and Shorts are skipped.
void collect_lockup(yyjson_val *obj, FeedPage *page)
{
    if (page->items.size() >= kMaxItems)
        return;

    yyjson_val *lockup = resolve_lockup(obj);
    if (lockup == nullptr)
        return;

    yyjson_val *content_id = yyjson_obj_get(lockup, "contentId");
    const char *video_id = yyjson_is_str(content_id) ? yyjson_get_str(content_id) : nullptr;
    if (video_id == nullptr || std::strlen(video_id) != 11)
        return;

    const std::string content_type = string_field(lockup, "contentType");
    if (contains_text(content_type.c_str(), "playlist") ||
        contains_text(content_type.c_str(), "channel") ||
        contains_text(content_type.c_str(), "shorts"))
        return;

    for (const FeedItem &existing : page->items)
    {
        if (existing.video_id == video_id)
            return;
    }

    yyjson_val *metadata = yyjson_obj_get(lockup, "metadata");
    yyjson_val *lockup_metadata = yyjson_obj_get(metadata, "lockupMetadataViewModel");
    yyjson_val *content_image = yyjson_obj_get(lockup, "contentImage");

    FeedItem item;
    item.video_id = video_id;
    item.title = lockup_title(lockup_metadata);
    item.author = lockup_author(lockup_metadata);
    item.thumbnail_url = lockup_thumbnail(lockup);
    item.live = contains_text(content_type.c_str(), "live");
    item.length_s = lockup_length(yyjson_obj_get(content_image, "thumbnailViewModel"), &item.live);
    page->items.push_back(std::move(item));
}

// The TV client (TVHTML5, the signed-in client) wraps each video in a
// tileRenderer (layout as in SmartTube's TileItem): the id under
// onSelectCommand.watchEndpoint (reelWatchEndpoint for Shorts), the title in
// metadata.tileMetadataRenderer, thumbnail and duration in
// header.tileHeaderRenderer. Channel and playlist tiles carry no video id and
// are skipped.
void collect_tile(yyjson_val *tile, FeedPage *page)
{
    if (!yyjson_is_obj(tile) || page->items.size() >= kMaxItems)
        return;
    yyjson_val *command = yyjson_obj_get(tile, "onSelectCommand");
    std::string video_id = string_field(yyjson_obj_get(command, "watchEndpoint"), "videoId");
    if (video_id.empty())
        video_id = string_field(yyjson_obj_get(command, "reelWatchEndpoint"), "videoId");
    if (video_id.empty())
        return;
    for (const FeedItem &existing : page->items)
    {
        if (existing.video_id == video_id)
            return;
    }

    yyjson_val *header = yyjson_obj_get(yyjson_obj_get(tile, "header"), "tileHeaderRenderer");
    yyjson_val *metadata = yyjson_obj_get(yyjson_obj_get(tile, "metadata"), "tileMetadataRenderer");
    std::string title = renderer_text(yyjson_obj_get(metadata, "title"));
    if (title.empty())
        title = renderer_text(yyjson_obj_get(header, "title"));
    if (title.empty())
        return;

    FeedItem item;
    item.video_id = std::move(video_id);
    item.title = std::move(title);
    // The first line's first item is the channel on video tiles.
    yyjson_val *lines = yyjson_obj_get(metadata, "lines");
    yyjson_val *first_line = yyjson_obj_get(yyjson_arr_get_first(lines), "lineRenderer");
    yyjson_val *first_item = yyjson_obj_get(
        yyjson_arr_get_first(yyjson_obj_get(first_line, "items")), "lineItemRenderer");
    item.author = renderer_text(yyjson_obj_get(first_item, "text"));
    item.thumbnail_url = thumbnail_text(header);
    yyjson_val *overlays = yyjson_obj_get(header, "thumbnailOverlays");
    const std::size_t overlay_count = yyjson_arr_size(overlays);
    for (std::size_t index = 0; index < overlay_count; ++index)
    {
        yyjson_val *status =
            yyjson_obj_get(yyjson_arr_get(overlays, index), "thumbnailOverlayTimeStatusRenderer");
        if (!yyjson_is_obj(status))
            continue;
        const std::string style = string_field(status, "style");
        if (style == "LIVE")
            item.live = true;
        const std::string length = renderer_text(yyjson_obj_get(status, "text"));
        if (!length.empty() && item.length_s <= 0.0)
            item.length_s = parse_clock(length.c_str());
    }
    page->items.push_back(std::move(item));
}

void collect_item(yyjson_val *obj, FeedPage *page)
{
    if (page->items.size() >= kMaxItems)
        return;

    yyjson_val *video = yyjson_obj_get(obj, "videoId");
    if (!yyjson_is_str(video))
        return;
    const char *video_id = yyjson_get_str(video);
    if (video_id == nullptr || *video_id == '\0')
        return;

    std::string title = title_text(obj);
    if (title.empty())
        return;

    for (const FeedItem &existing : page->items)
    {
        if (existing.video_id == video_id)
            return;
    }

    FeedItem item;
    item.video_id = video_id;
    item.title = std::move(title);
    item.author = author_text(obj);
    item.thumbnail_url = thumbnail_text(obj);
    item.length_s = length_seconds(obj);
    item.live = is_live(obj);
    page->items.push_back(std::move(item));
}

// A channelRenderer: the avatar/name/subscriber line a search answered with.
// It never has a videoId, so the video walker ignores it; channels and videos
// stay independent lists.
void collect_channel(yyjson_val *obj, FeedPage *page)
{
    if (page->channels.size() >= kMaxChannels)
        return;

    std::string channel_id = string_field(obj, "channelId");
    if (channel_id.empty())
        return;

    for (const ChannelItem &existing : page->channels)
    {
        if (existing.channel_id == channel_id)
            return;
    }

    ChannelItem item;
    item.channel_id = std::move(channel_id);
    item.name = channel_title(obj);
    item.thumbnail_url = thumbnail_text(obj);
    item.subscribers = subscriber_text(obj);
    page->channels.push_back(std::move(item));
}

// A channel in the TV client's answer is a lockupViewModel whose contentType
// is LOCKUP_CONTENT_TYPE_CHANNEL: contentId, lockupMetadataViewModel title and
// subscriber row, contentImage avatar. The video lockup reader skips channels,
// so the two lists stay independent.
void collect_channel_lockup(yyjson_val *obj, FeedPage *page)
{
    if (page->channels.size() >= kMaxChannels)
        return;

    yyjson_val *lockup = resolve_lockup(obj);
    if (lockup == nullptr)
        return;

    const std::string content_type = string_field(lockup, "contentType");
    if (!contains_text(content_type.c_str(), "channel"))
        return;

    std::string channel_id = string_field(lockup, "contentId");
    if (channel_id.empty())
        return;

    for (const ChannelItem &existing : page->channels)
    {
        if (existing.channel_id == channel_id)
            return;
    }

    yyjson_val *metadata =
        yyjson_obj_get(yyjson_obj_get(lockup, "metadata"), "lockupMetadataViewModel");

    ChannelItem item;
    item.channel_id = std::move(channel_id);
    item.name = lockup_title(metadata);
    item.thumbnail_url = lockup_thumbnail(lockup);
    item.subscribers = lockup_subscriber_text(metadata);
    page->channels.push_back(std::move(item));
}

void walk_feed(yyjson_val *value, FeedPage *page, std::string *continuation)
{
    if (yyjson_is_obj(value))
    {
        collect_item(value, page);
        collect_lockup(value, page);

        yyjson_obj_iter iter;
        yyjson_obj_iter_init(value, &iter);
        yyjson_val *key = nullptr;
        while ((key = yyjson_obj_iter_next(&iter)) != nullptr)
        {
            const char *name = yyjson_get_str(key);
            if (name == nullptr || is_ad_key(name))
                continue;
            yyjson_val *child = yyjson_obj_iter_get_val(key);
            if (std::strcmp(name, "channelRenderer") == 0)
                collect_channel(child, page);
            if (std::strcmp(name, "lockupViewModel") == 0)
                collect_channel_lockup(child, page);
            if (std::strcmp(name, "tileRenderer") == 0)
                collect_tile(child, page);
            if (std::strcmp(name, "continuationCommand") == 0)
            {
                yyjson_val *token = yyjson_obj_get(child, "token");
                if (yyjson_is_str(token))
                    *continuation = yyjson_get_str(token);
            }
            walk_feed(child, page, continuation);
        }
        return;
    }

    if (yyjson_is_arr(value))
    {
        const std::size_t count = yyjson_arr_size(value);
        for (std::size_t index = 0; index < count; ++index)
            walk_feed(yyjson_arr_get(value, index), page, continuation);
    }
}

// ---- the channel-page readers ---------------------------------------------------------------

// The first object under `key`, ad subtrees skipped. A channel header hides
// the name, avatar and subscriber line under different renderers per client,
// so the search is a walk rather than a fixed path.
yyjson_val *find_object(yyjson_val *value, const char *key)
{
    if (yyjson_is_obj(value))
    {
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(value, &iter);
        yyjson_val *entry = nullptr;
        while ((entry = yyjson_obj_iter_next(&iter)) != nullptr)
        {
            const char *name = yyjson_get_str(entry);
            if (name == nullptr || is_ad_key(name))
                continue;
            yyjson_val *child = yyjson_obj_iter_get_val(entry);
            if (std::strcmp(name, key) == 0 && yyjson_is_obj(child))
                return child;
            yyjson_val *found = find_object(child, key);
            if (found != nullptr)
                return found;
        }
        return nullptr;
    }
    if (yyjson_is_arr(value))
    {
        const std::size_t count = yyjson_arr_size(value);
        for (std::size_t index = 0; index < count; ++index)
        {
            yyjson_val *found = find_object(yyjson_arr_get(value, index), key);
            if (found != nullptr)
                return found;
        }
    }
    return nullptr;
}

// A string field or a text object (simpleText / runs[0].text).
std::string text_of(yyjson_val *value)
{
    if (yyjson_is_str(value))
        return yyjson_get_str(value);
    if (yyjson_is_obj(value))
        return renderer_text(value);
    return {};
}

// The first text that carries `needle`, wherever the response hides it.
std::string find_text_containing(yyjson_val *value, const char *needle)
{
    if (yyjson_is_obj(value))
    {
        static const char *const keys[] = {"simpleText", "content"};
        for (const char *key : keys)
        {
            yyjson_val *text = yyjson_obj_get(value, key);
            if (yyjson_is_str(text))
            {
                const char *string = yyjson_get_str(text);
                if (string != nullptr && std::strstr(string, needle) != nullptr)
                    return string;
            }
        }
        std::string runs = renderer_text(value);
        if (runs.find(needle) != std::string::npos)
            return runs;

        yyjson_obj_iter iter;
        yyjson_obj_iter_init(value, &iter);
        yyjson_val *entry = nullptr;
        while ((entry = yyjson_obj_iter_next(&iter)) != nullptr)
        {
            const char *name = yyjson_get_str(entry);
            if (name == nullptr || is_ad_key(name))
                continue;
            std::string found = find_text_containing(yyjson_obj_iter_get_val(entry), needle);
            if (!found.empty())
                return found;
        }
        return {};
    }
    if (yyjson_is_arr(value))
    {
        const std::size_t count = yyjson_arr_size(value);
        for (std::size_t index = 0; index < count; ++index)
        {
            std::string found = find_text_containing(yyjson_arr_get(value, index), needle);
            if (!found.empty())
                return found;
        }
    }
    return {};
}

// pageHeaderViewModel.title: a plain text object, or the dynamic text the
// channel header carries on the web.
std::string header_title(yyjson_val *header)
{
    yyjson_val *title = yyjson_obj_get(header, "title");
    std::string direct = text_of(title);
    if (!direct.empty())
        return direct;
    yyjson_val *dynamic = yyjson_obj_get(title, "dynamicTextViewModel");
    yyjson_val *text = yyjson_obj_get(dynamic, "text");
    yyjson_val *content = yyjson_obj_get(text, "content");
    if (yyjson_is_str(content))
        return yyjson_get_str(content);
    return {};
}

void collect_tab(yyjson_val *tab, ChannelPage *page)
{
    std::string title = text_of(yyjson_obj_get(tab, "title"));
    if (title.empty())
        return;

    ChannelTab entry;
    entry.title = std::move(title);
    yyjson_val *endpoint = yyjson_obj_get(tab, "endpoint");
    yyjson_val *browse = yyjson_obj_get(endpoint, "browseEndpoint");
    entry.params = string_field(browse, "params");
    if (entry.params.empty())
        entry.params = string_field(tab, "params");
    yyjson_val *selected = yyjson_obj_get(tab, "selected");
    entry.selected = yyjson_is_bool(selected) && yyjson_get_bool(selected);

    for (const ChannelTab &existing : page->tabs)
    {
        if (existing.title == entry.title && existing.params == entry.params)
            return;
    }
    page->tabs.push_back(std::move(entry));
}

// Every tabRenderer of the response, in encounter order.
void walk_tabs(yyjson_val *value, ChannelPage *page)
{
    if (page->tabs.size() >= kMaxTabs)
        return;
    if (yyjson_is_obj(value))
    {
        yyjson_val *tab = yyjson_obj_get(value, "tabRenderer");
        if (yyjson_is_obj(tab))
            collect_tab(tab, page);

        yyjson_obj_iter iter;
        yyjson_obj_iter_init(value, &iter);
        yyjson_val *entry = nullptr;
        while ((entry = yyjson_obj_iter_next(&iter)) != nullptr)
        {
            const char *name = yyjson_get_str(entry);
            if (name == nullptr || is_ad_key(name))
                continue;
            walk_tabs(yyjson_obj_iter_get_val(entry), page);
        }
        return;
    }
    if (yyjson_is_arr(value))
    {
        const std::size_t count = yyjson_arr_size(value);
        for (std::size_t index = 0; index < count; ++index)
            walk_tabs(yyjson_arr_get(value, index), page);
    }
}

std::string channel_name_from(yyjson_val *root)
{
    yyjson_val *metadata = find_object(root, "channelMetadataRenderer");
    if (metadata != nullptr)
    {
        std::string title = text_of(yyjson_obj_get(metadata, "title"));
        if (!title.empty())
            return title;
    }

    yyjson_val *header = find_object(root, "pageHeaderViewModel");
    if (header != nullptr)
    {
        std::string title = header_title(header);
        if (!title.empty())
            return title;
    }

    // The header sometimes only names the handle ("@name").
    yyjson_val *handle = find_object(root, "channelHandle");
    if (handle != nullptr)
    {
        std::string text = text_of(yyjson_obj_get(handle, "text"));
        if (!text.empty())
            return text;
    }
    return {};
}

std::string channel_avatar_from(yyjson_val *root)
{
    yyjson_val *header = find_object(root, "pageHeaderViewModel");

    // The channel page carries the same avatar in several shapes that differ in
    // resolution, so collect every candidate and hand back the widest one. On
    // equal widths the first shape encountered wins, the decorated avatar
    // first.
    AvatarCandidate best;

    // The web header nests the decorated avatar deepest.
    if (header != nullptr)
    {
        yyjson_val *image = yyjson_obj_get(header, "image");
        yyjson_val *decorated = yyjson_obj_get(image, "decoratedAvatarViewModel");
        yyjson_val *avatar = yyjson_obj_get(decorated, "avatar");
        yyjson_val *avatar_view = yyjson_obj_get(avatar, "avatarViewModel");
        yyjson_val *sources = yyjson_obj_get(yyjson_obj_get(avatar_view, "image"), "sources");
        keep_widest(&best, widest_avatar(sources));
    }

    // The legacy channel metadata avatar.
    yyjson_val *metadata = find_object(root, "channelMetadataRenderer");
    if (metadata != nullptr)
    {
        yyjson_val *avatar = yyjson_obj_get(metadata, "avatar");
        keep_widest(&best, widest_avatar(yyjson_obj_get(avatar, "thumbnails")));
    }

    // The plain header thumbnail.
    if (header != nullptr)
    {
        yyjson_val *thumbnail = yyjson_obj_get(header, "thumbnail");
        keep_widest(&best, widest_avatar(yyjson_obj_get(thumbnail, "thumbnails")));
    }

    // Any other avatar object the response nests (a header avatar with a
    // thumbnails array, or an avatar wrapping another avatar).
    yyjson_val *avatar = find_object(root, "avatar");
    if (avatar != nullptr)
    {
        AvatarCandidate candidate = widest_avatar(yyjson_obj_get(avatar, "thumbnails"));
        if (candidate.url.empty())
        {
            yyjson_val *nested = yyjson_obj_get(avatar, "avatar");
            candidate = widest_avatar(yyjson_obj_get(nested, "thumbnails"));
        }
        keep_widest(&best, candidate);
    }
    return best.url;
}

std::string channel_subscribers_from(yyjson_val *root)
{
    yyjson_val *count = find_object(root, "subscriberCountText");
    if (count != nullptr)
    {
        std::string text = text_of(count);
        if (!text.empty())
            return text;
    }
    // The page header renders the line as one metadata row part.
    return find_text_containing(root, "subscriber");
}

std::string first_error_message(yyjson_val *root)
{
    yyjson_val *error = yyjson_obj_get(root, "error");
    yyjson_val *message = yyjson_obj_get(error, "message");
    if (yyjson_is_str(message))
        return yyjson_get_str(message);

    yyjson_val *context = yyjson_obj_get(root, "responseContext");
    yyjson_val *errors = yyjson_obj_get(context, "errors");
    yyjson_val *first = yyjson_arr_get_first(errors);
    yyjson_val *error_message = yyjson_obj_get(first, "message");
    if (yyjson_is_str(error_message))
        return yyjson_get_str(error_message);

    yyjson_val *context_error = yyjson_obj_get(context, "error");
    yyjson_val *context_message = yyjson_obj_get(context_error, "message");
    if (yyjson_is_str(context_message))
        return yyjson_get_str(context_message);
    return {};
}

bool has_error_field(yyjson_val *root)
{
    if (yyjson_is_obj(yyjson_obj_get(root, "error")))
        return true;
    yyjson_val *context = yyjson_obj_get(root, "responseContext");
    if (yyjson_is_obj(context))
    {
        if (yyjson_is_arr(yyjson_obj_get(context, "errors")))
            return true;
        if (yyjson_is_obj(yyjson_obj_get(context, "error")))
            return true;
    }
    return false;
}

// ---- format readers -------------------------------------------------------------------------

Codec classify_codec(std::string_view mime)
{
    const auto has = [mime](std::string_view needle)
    { return mime.find(needle) != std::string_view::npos; };
    // An audio mime carries only audio codec names, so those are checked
    // first there; a video (or muxed) mime is classified as video.
    if (mime.rfind("audio/", 0) == 0)
    {
        if (has("opus"))
            return Codec::opus;
        if (has("mp4a"))
            return Codec::aac;
    }
    if (has("av01"))
        return Codec::av1;
    if (has("vp9") || has("vp09"))
        return Codec::vp9;
    if (has("avc1"))
        return Codec::avc;
    if (has("opus"))
        return Codec::opus;
    if (has("mp4a"))
        return Codec::aac;
    return Codec::unknown;
}

// The value of one hex digit, -1 when the character is not one.
int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

std::string percent_decode(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        if (text[index] == '%' && index + 2 < text.size())
        {
            const int high = hex_digit(text[index + 1]);
            const int low = hex_digit(text[index + 2]);
            if (high >= 0 && low >= 0)
            {
                out += static_cast<char>((high << 4) | low);
                index += 2;
                continue;
            }
        }
        out += text[index];
    }
    return out;
}

// One "key=value" field of a urlencoded query string, percent-decoded. A
// literal '+' is kept: signatureCipher encodes it as %2B when it belongs to a
// signature. The response never nests these in JSON, so they are read by hand.
std::string query_field(std::string_view query, std::string_view key)
{
    std::size_t at = 0;
    while (at < query.size())
    {
        const std::size_t end = query.find('&', at);
        const std::size_t length = end == std::string_view::npos ? query.size() - at : end - at;
        const std::string_view pair = query.substr(at, length);
        const std::size_t equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == key)
            return percent_decode(pair.substr(equals + 1));
        if (end == std::string_view::npos)
            break;
        at = end + 1;
    }
    return {};
}

// The host of a stream URL ("rr1---sn-example.googlevideo.com"): naming the
// googlevideo node that has to serve - or refuse, 403 - a format is what makes
// a refusal diagnosable in yt.log. Empty when the URL carries none.
std::string url_host(std::string_view url)
{
    const std::size_t scheme = url.find("://");
    const std::size_t start = scheme == std::string_view::npos ? 0 : scheme + 3;
    const std::size_t end = url.find_first_of("/?#", start);
    return std::string(
        url.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
}

bool read_format(yyjson_val *obj, Format *format)
{
    if (!yyjson_is_obj(obj))
        return false;

    // A format is offered either with a plain `url` or, once the client gates
    // playback behind the player's signature, as `signatureCipher` (older
    // answers: `cipher`) - the unsigned base URL plus the scrambled signature
    // and the query parameter it belongs to. Both stay candidates: selection
    // may pick a ciphered one, the Descrambler turns it into the playable URL
    // before it is opened.
    std::string base = string_field(obj, "url");
    if (base.empty())
    {
        std::string cipher = string_field(obj, "signatureCipher");
        if (cipher.empty())
            cipher = string_field(obj, "cipher");
        if (!cipher.empty())
        {
            base = query_field(cipher, "url");
            format->cipher_s = query_field(cipher, "s");
            format->cipher_sp = query_field(cipher, "sp");
            format->cipher_url = base;
            format->ciphered = true;
        }
    }
    // Without a URL - or, for a ciphered one, without the signature to
    // descramble - there is nothing to open.
    if (base.empty() || (format->ciphered && format->cipher_s.empty()))
        return false;

    // A ciphered format keeps the unsigned base URL here as well, so stream
    // selection sees it as an ordinary candidate.
    format->url = base;
    format->itag = static_cast<int>(integer_field(obj, "itag", 0));
    format->codec = classify_codec(string_field(obj, "mimeType"));
    format->width = static_cast<int>(integer_field(obj, "width", 0));
    format->height = static_cast<int>(integer_field(obj, "height", 0));
    format->fps = number_field(obj, "fps", 0.0);
    format->bitrate = integer_field(obj, "bitrate", 0);
    format->content_length = integer_field(obj, "contentLength", 0);
    format->sample_rate = static_cast<int>(integer_field(obj, "audioSampleRate", 0));
    return true;
}

// ---- request-body helpers -------------------------------------------------------------------

void append_json_string(std::string &out, std::string_view text)
{
    out += '"';
    for (char ch : text)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (byte < 0x20)
            {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[(byte >> 4) & 0x0f];
                out += hex[byte & 0x0f];
            }
            else
                out += ch;
            break;
        }
    }
    out += '"';
}

std::string field_string(const char *name, std::string_view value)
{
    std::string out;
    out += '"';
    out += name;
    out += "\":";
    append_json_string(out, value);
    return out;
}

// ---- pre-roll wait ----------------------------------------------------------------------------

// The `length_seconds` value of an ad renderer's playerVars query string.
double query_seconds(const std::string &query, const char *key)
{
    const std::string needle = std::string(key) + "=";
    std::size_t at = 0;
    while ((at = query.find(needle, at)) != std::string::npos)
    {
        if (at == 0 || query[at - 1] == '&' || query[at - 1] == '?')
        {
            const std::size_t start = at + needle.size();
            const std::size_t end = query.find('&', start);
            const std::string value =
                query.substr(start, end == std::string::npos ? std::string::npos : end - start);
            char *stop = nullptr;
            const double seconds = std::strtod(value.c_str(), &stop);
            if (stop != value.c_str() && seconds >= 0.0)
                return seconds;
            return -1.0;
        }
        at += needle.size();
    }
    return -1.0;
}

// One instreamVideoAdRenderer: its skip offset when YouTube lets it be skipped,
// else its full length. -1 when neither is known.
double ad_wait_seconds(yyjson_val *renderer)
{
    if (!yyjson_is_obj(renderer))
        return -1.0;
    yyjson_val *skip = yyjson_obj_get(renderer, "skipOffsetMilliseconds");
    if (yyjson_is_num(skip))
        return yyjson_get_num(skip) / 1000.0;
    if (yyjson_is_str(skip))
        return std::strtod(yyjson_get_str(skip), nullptr) / 1000.0;
    return query_seconds(string_field(renderer, "playerVars"), "length_seconds");
}

void add_ad(yyjson_val *renderer, double *wait, int *ads)
{
    const double seconds = ad_wait_seconds(renderer);
    if (seconds < 0.0)
        return;
    *wait += seconds;
    ++*ads;
}

// Since 2025 googlevideo answers 403 to a stream URL requested before the
// pre-roll ads of its player answer would have finished, so a client that
// skips them has to wait that long (yt-dlp `_get_available_at_timestamp`).
// Reads adPlacements (AD_PLACEMENT_KIND_START) and adSlots
// (SLOT_TRIGGER_EVENT_BEFORE_CONTENT, plain or sequential layouts).
void read_preroll(yyjson_val *root, VideoInfo *info)
{
    double wait = 0.0;
    int ads = 0;
    yyjson_val *placements = yyjson_obj_get(root, "adPlacements");
    for (std::size_t i = 0, n = yyjson_arr_size(placements); i < n; ++i)
    {
        yyjson_val *placement =
            yyjson_obj_get(yyjson_arr_get(placements, i), "adPlacementRenderer");
        yyjson_val *config =
            yyjson_obj_get(yyjson_obj_get(placement, "config"), "adPlacementConfig");
        if (string_field(config, "kind") != "AD_PLACEMENT_KIND_START")
            continue;
        add_ad(yyjson_obj_get(yyjson_obj_get(placement, "renderer"), "instreamVideoAdRenderer"),
               &wait, &ads);
    }
    yyjson_val *slots = yyjson_obj_get(root, "adSlots");
    for (std::size_t i = 0, n = yyjson_arr_size(slots); i < n; ++i)
    {
        yyjson_val *slot = yyjson_obj_get(yyjson_arr_get(slots, i), "adSlotRenderer");
        if (string_field(yyjson_obj_get(slot, "adSlotMetadata"), "triggerEvent") !=
            "SLOT_TRIGGER_EVENT_BEFORE_CONTENT")
            continue;
        yyjson_val *layout =
            yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(slot, "fulfillmentContent"),
                                                         "fulfilledLayout"),
                                          "playerBytesAdLayoutRenderer"),
                           "renderingContent");
        add_ad(yyjson_obj_get(layout, "instreamVideoAdRenderer"), &wait, &ads);
        yyjson_val *sequence = yyjson_obj_get(
            yyjson_obj_get(layout, "playerBytesSequentialLayoutRenderer"), "sequentialLayouts");
        for (std::size_t j = 0, m = yyjson_arr_size(sequence); j < m; ++j)
        {
            yyjson_val *content = yyjson_obj_get(
                yyjson_obj_get(yyjson_arr_get(sequence, j), "playerBytesAdLayoutRenderer"),
                "renderingContent");
            add_ad(yyjson_obj_get(content, "instreamVideoAdRenderer"), &wait, &ads);
        }
    }
    info->preroll_wait_s = wait;
    info->preroll_ads = ads;
}

} // namespace

// ---- the parsers (the unit-test seam) -------------------------------------------------------

bool parse_feed_page(std::string_view json, FeedPage *page)
{
    if (page == nullptr)
        return false;
    page->items.clear();
    page->channels.clear();
    page->continuation.clear();
    page->error.clear();
    if (json.empty())
    {
        page->error = "bad json";
        return false;
    }

    yyjson_doc *doc = yyjson_read(json.data(), json.size(), 0);
    if (doc == nullptr)
    {
        page->error = "bad json";
        return false;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root))
    {
        page->error = "bad json";
        yyjson_doc_free(doc);
        return false;
    }

    std::string continuation;
    walk_feed(root, page, &continuation);

    // An empty page carrying an error is an error; an empty page without one
    // is the end of the feed.
    if (page->items.empty() && has_error_field(root))
    {
        std::string message = first_error_message(root);
        page->error = message.empty() ? "no items" : message;
    }
    if (!continuation.empty())
        page->continuation = std::move(continuation);

    yyjson_doc_free(doc);
    return page->error.empty();
}

bool parse_channel_page(std::string_view json, ChannelPage *page)
{
    if (page == nullptr)
        return false;
    *page = ChannelPage{};
    if (json.empty())
    {
        page->error = "bad json";
        return false;
    }

    yyjson_doc *doc = yyjson_read(json.data(), json.size(), 0);
    if (doc == nullptr)
    {
        page->error = "bad json";
        return false;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root))
    {
        page->error = "bad json";
        yyjson_doc_free(doc);
        return false;
    }

    walk_tabs(root, page);
    if (!page->tabs.empty())
    {
        bool any_selected = false;
        for (const ChannelTab &tab : page->tabs)
            any_selected = any_selected || tab.selected;
        if (!any_selected)
            page->tabs[0].selected = true;
    }

    // A header field that the response does not carry stays empty rather than
    // failing the page: the tabs and the videos are still usable.
    page->channel.name = channel_name_from(root);
    page->channel.thumbnail_url = channel_avatar_from(root);
    page->channel.subscribers = channel_subscribers_from(root);

    FeedPage feed;
    std::string continuation;
    walk_feed(root, &feed, &continuation);
    page->items = std::move(feed.items);
    if (!continuation.empty())
        page->continuation = std::move(continuation);

    if (page->items.empty() && has_error_field(root))
    {
        std::string message = first_error_message(root);
        page->error = message.empty() ? "no items" : message;
    }

    yyjson_doc_free(doc);
    return page->error.empty();
}

// A one-line census of a client's formats: total, the per-codec split, how
// many carry a plain URL versus only a cipher, the tallest video, and the
// googlevideo host(s) the URLs point at. Used both for the per-client log line
// and the no-stream failure text, so the log and the screen describe the same
// answer - and yt.log shows which kind of URL the formats carry and where it
// would have to be served from.
std::string format_summary(const std::vector<Format> &formats)
{
    std::size_t avc = 0;
    std::size_t vp9 = 0;
    std::size_t av1 = 0;
    std::size_t other = 0;
    std::size_t plain = 0;
    std::size_t ciphered = 0;
    int max_height = 0;
    std::vector<std::string> hosts; // the distinct URL hosts
    for (const Format &format : formats)
    {
        switch (format.codec)
        {
        case Codec::avc:
            ++avc;
            break;
        case Codec::vp9:
            ++vp9;
            break;
        case Codec::av1:
            ++av1;
            break;
        default:
            ++other;
            break;
        }
        if (format.ciphered)
            ++ciphered;
        else
            ++plain;
        if (is_video_codec(format.codec) && format.height > max_height)
            max_height = format.height;
        const std::string host = url_host(format.url);
        if (host.empty())
            continue;
        bool known = false;
        for (const std::string &seen : hosts)
        {
            if (seen == host)
            {
                known = true;
                break;
            }
        }
        if (!known)
            hosts.push_back(host);
    }
    std::string host_list;
    for (const std::string &host : hosts)
    {
        if (!host_list.empty())
            host_list += ",";
        host_list += host;
    }
    if (host_list.empty())
        host_list = "none";

    return std::to_string(formats.size()) + " formats: " + std::to_string(avc) + " avc, " +
           std::to_string(vp9) + " vp9, " + std::to_string(av1) + " av1, " + std::to_string(other) +
           " other, " + std::to_string(plain) + " plain, " + std::to_string(ciphered) +
           " ciphered, max " + std::to_string(max_height) + "p, host " + host_list;
}

// One short line naming a format and the kind of URL it carries:
// "itag=137 1080p AVC (ciphered)". The service puts it in front of the ffmpeg
// text when an open fails, so a 403 names the format and its kind instead of
// the bare demuxer string. nullptr becomes "none".
std::string describe_format(const Format *format)
{
    if (format == nullptr)
        return "none";
    std::string text = "itag=" + std::to_string(format->itag);
    if (format->height > 0)
        text += " " + std::to_string(format->height) + "p";
    text += " ";
    text += codec_label(format->codec);
    text += format->ciphered ? " (ciphered)" : " (plain)";
    return text;
}

// yt-dlp's `(?:signatureTimestamp|sts)\s*:\s*(?P<sts>[0-9]{5})`, by hand: the
// player JS is megabytes, and <regex> would walk it again with no gain. Like
// the regex it is unanchored, so a longer digit run still yields its first
// five digits, and the earliest of the two keys wins.
std::string signature_timestamp(std::string_view js)
{
    std::size_t best = std::string_view::npos;
    std::string value;
    for (std::string_view key : {"signatureTimestamp", "sts"})
    {
        std::size_t at = js.find(key);
        while (at != std::string_view::npos)
        {
            std::size_t pos = at + key.size();
            while (pos < js.size() && std::isspace(static_cast<unsigned char>(js[pos])))
                ++pos;
            // A JSON-shaped occurrence writes the key quoted: `"signatureTimestamp":20732`.
            if (pos < js.size() && js[pos] == '"')
                ++pos;
            while (pos < js.size() && std::isspace(static_cast<unsigned char>(js[pos])))
                ++pos;
            if (pos < js.size() && js[pos] == ':')
            {
                ++pos;
                while (pos < js.size() && std::isspace(static_cast<unsigned char>(js[pos])))
                    ++pos;
                const std::size_t start = pos;
                while (pos < js.size() && std::isdigit(static_cast<unsigned char>(js[pos])))
                    ++pos;
                if (pos - start >= 5 && (best == std::string_view::npos || at < best))
                {
                    best = at;
                    value = std::string(js.substr(start, 5));
                }
            }
            at = js.find(key, at + key.size());
        }
    }
    return value;
}

bool parse_video_info(std::string_view json, VideoInfo *info)
{
    if (info == nullptr)
        return false;
    *info = VideoInfo{};
    if (json.empty())
    {
        info->error = "bad json";
        return false;
    }

    yyjson_doc *doc = yyjson_read(json.data(), json.size(), 0);
    if (doc == nullptr)
    {
        info->error = "bad json";
        return false;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root))
    {
        info->error = "bad json";
        yyjson_doc_free(doc);
        return false;
    }

    yyjson_val *playability = yyjson_obj_get(root, "playabilityStatus");
    yyjson_val *status = yyjson_obj_get(playability, "status");
    const char *status_text = yyjson_is_str(status) ? yyjson_get_str(status) : nullptr;
    if (status_text == nullptr || std::strcmp(status_text, "OK") != 0)
    {
        yyjson_val *reason = yyjson_obj_get(playability, "reason");
        info->error = yyjson_is_str(reason) ? yyjson_get_str(reason) : "unplayable";
        yyjson_doc_free(doc);
        return false;
    }

    yyjson_val *details = yyjson_obj_get(root, "videoDetails");
    info->video_id = string_field(details, "videoId");
    info->title = string_field(details, "title");
    info->author = string_field(details, "author");
    info->length_s = number_field(details, "lengthSeconds", 0.0);
    yyjson_val *live = yyjson_obj_get(details, "isLiveContent");
    info->live = yyjson_is_bool(live) && yyjson_get_bool(live);

    yyjson_val *streaming = yyjson_obj_get(root, "streamingData");
    yyjson_val *adaptive = yyjson_obj_get(streaming, "adaptiveFormats");
    yyjson_val *formats = yyjson_obj_get(streaming, "formats");
    yyjson_val *raw = yyjson_is_arr(adaptive) ? adaptive : formats;

    // The census counts the raw entries before read_format drops anything: an
    // answer whose adaptive entries carry neither url nor cipher, with only
    // streamingData.serverAbrStreamingUrl to offer, is the SABR-only shape of
    // yt-dlp issue #12482 and has to be nameable.
    const std::size_t adaptive_count = yyjson_is_arr(adaptive) ? yyjson_arr_size(adaptive) : 0;
    const std::size_t formats_count = yyjson_is_arr(formats) ? yyjson_arr_size(formats) : 0;
    const std::size_t count = yyjson_arr_size(raw);
    std::size_t with_url = 0;
    std::size_t with_cipher = 0;
    std::size_t drm = 0;
    for (std::size_t index = 0; index < count; ++index)
    {
        yyjson_val *entry = yyjson_arr_get(raw, index);
        if (!yyjson_is_obj(entry))
            continue;
        if (!string_field(entry, "url").empty())
            ++with_url;
        if (!string_field(entry, "signatureCipher").empty() ||
            !string_field(entry, "cipher").empty())
            ++with_cipher;
        if (yyjson_obj_get(entry, "drmFamilies") != nullptr ||
            yyjson_obj_get(entry, "drmParams") != nullptr)
            ++drm;
    }
    const std::string sabr_url = string_field(streaming, "serverAbrStreamingUrl");
    // The source array is named: answers carry adaptiveFormats normally and
    // `formats` for the legacy/progressive shape, and the label is what the
    // log and the SABR failure text read.
    const std::string source = yyjson_is_arr(adaptive)
                                   ? std::to_string(adaptive_count) + " adaptive"
                                   : std::to_string(formats_count) + " formats";
    info->census = source + ", " + std::to_string(with_url) + " with url, " +
                   std::to_string(with_cipher) + " with cipher, " + std::to_string(drm) +
                   " drm, sabr-url " + (sabr_url.empty() ? "absent" : "present");

    info->formats.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        Format format;
        if (read_format(yyjson_arr_get(raw, index), &format))
            info->formats.push_back(std::move(format));
    }

    // No usable format, and either the answer names the SABR endpoint or its
    // adaptive entries carry no url/cipher: SABR-only. A format read_format
    // still rejected (a cipher without its signature) is not SABR.
    info->sabr_only =
        info->formats.empty() &&
        (!sabr_url.empty() || (adaptive_count > 0 && with_url == 0 && with_cipher == 0));
    if (info->formats.empty())
    {
        const std::size_t keep = json.size() < kMaxRawBody ? json.size() : kMaxRawBody;
        info->raw_body.assign(json.data(), keep);
    }

    read_preroll(root, info);

    // The player JS a ciphered format needs. Answers name it directly
    // (assets.js); a few only publish the hash, from which the TV player's
    // fixed path is built.
    yyjson_val *assets = yyjson_obj_get(root, "assets");
    info->player_js_url = string_field(assets, "js");
    if (info->player_js_url.empty())
    {
        const std::string hash = string_field(assets, "jsUrlHash");
        if (!hash.empty())
            info->player_js_url = "https://www.youtube.com/s/player/" + hash +
                                  "/tv-player-ias.vflset/tv-player-ias.js";
    }

    yyjson_doc_free(doc);
    return true;
}

// ---- the client -----------------------------------------------------------------------------

struct InnerTube::Impl
{
    // One POST attempt. `transport_ok` is false when the request never
    // completed (`error` then carries the reason); `http_ok` when the status
    // is 2xx.
    struct Attempt
    {
        const Client *client = nullptr;
        // The name the error text uses; "WEB signed out" for the token-less
        // fallback attempt, otherwise the client's name.
        std::string label;
        long status = 0;
        bool transport_ok = false;
        bool http_ok = false;
        std::string error; // transport failure text
        std::string body;
    };

    std::string access_token;
    std::string visitor_data;
    std::string client_name = kWebClientName;
    std::string client_version = kWebClientVersion;
    long timeout_s = 20;
    long status = 0;
    std::string error;
    // The signature timestamp of the player JS, cached by JS URL so each JS is
    // downloaded at most once per process; `sts` is the value in use (empty
    // until one extraction succeeds, then simply left out of the payload).
    std::vector<std::pair<std::string, std::string>> sts_cache;
    std::string sts;

    // The player request body: the videoId and the playback context every
    // client sends, plus the signature timestamp once it is known. Built per
    // client so an sts read from the first answer already reaches the rest of
    // the chain.
    std::string player_extra(const std::string &video_id) const
    {
        std::string out = field_string("videoId", video_id);
        out += ",\"playbackContext\":{\"contentPlaybackContext\":{\"html5Preference\":";
        out += "\"HTML5_PREF_WANTS\"";
        if (!sts.empty())
        {
            out += ",\"signatureTimestamp\":";
            out += sts;
        }
        out += "}}";
        return out;
    }

    // The signature timestamp the answer's player JS carries, read through the
    // same http layer the descrambler uses. Every failure - no URL, transport
    // error, no match - is silent: the payload simply omits sts, as yt-dlp
    // tolerates.
    void ensure_sts(const std::string &player_js_url)
    {
        if (player_js_url.empty())
            return;
        for (const std::pair<std::string, std::string> &entry : sts_cache)
        {
            if (entry.first == player_js_url)
            {
                if (!entry.second.empty())
                    sts = entry.second;
                return;
            }
        }

        http::Request request;
        request.url = player_js_url;
        request.headers = {{"User-Agent", kTvUserAgent}};
        request.timeout_s = timeout_s;
        request.max_bytes = kMaxPlayerJsBytes;
        const http::Response response = http::perform(request);
        const std::string value = response.error.empty() && response.http_ok()
                                      ? signature_timestamp(response.body)
                                      : std::string();
        sts_cache.emplace_back(player_js_url, value);
        if (!value.empty())
            sts = value;
    }

    // The client the auth state selects: third-party OAuth only gets content
    // from the TV client, while signed out only WEB answers.
    const Client &selected_client()
    {
        const Client &client = access_token.empty() ? kWebClient : kTvClient;
        client_name = client.name;
        client_version = client.version;
        return client;
    }

    std::string payload(const std::string &extra, const Client &client) const
    {
        std::string out;
        out.reserve(512);
        out += "{\"context\":{\"client\":{\"clientName\":\"";
        out += client.name;
        out += "\",\"clientVersion\":\"";
        out += client.version;
        out += "\",\"hl\":\"en\",\"gl\":\"US\"";
        if (client.extra != nullptr && *client.extra != '\0')
        {
            out += ',';
            out += client.extra;
        }
        if (client.ua_in_context)
        {
            out += ",\"userAgent\":";
            append_json_string(out, client.ua);
        }
        if (!visitor_data.empty())
        {
            out += ",\"visitorData\":";
            append_json_string(out, visitor_data);
        }
        out += '}';
        out += "},\"contentCheckOk\":true,\"racyCheckOk\":true";
        if (!extra.empty())
        {
            out += ',';
            out += extra;
        }
        out += '}';
        return out;
    }

    static std::string endpoint_url(const char *endpoint)
    {
        std::string out = kBaseUrl;
        out += endpoint;
        out += kApiKey;
        return out;
    }

    void remember_visitor(const std::string &body)
    {
        if (body.empty())
            return;
        yyjson_doc *doc = yyjson_read(body.data(), body.size(), 0);
        if (doc == nullptr)
            return;
        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *context = yyjson_obj_get(root, "responseContext");
        yyjson_val *visitor = yyjson_obj_get(context, "visitorData");
        if (yyjson_is_str(visitor))
        {
            const char *text = yyjson_get_str(visitor);
            if (text != nullptr && *text != '\0')
                visitor_data = text;
        }
        yyjson_doc_free(doc);
    }

    // One POST with one client; it never retries. The returned attempt and
    // `status` describe what the client answered. The Bearer token goes only
    // to the clients that accept it (`client.bearer`, the TV family): YouTube
    // refuses it on WEB, and VISIONOS needs none. `signed_out` drops it too,
    // which is what the signed-out WEB fallback needs.
    Attempt post_once(const char *endpoint, const std::string &extra, const Client &client,
                      bool signed_out = false)
    {
        Attempt attempt;
        attempt.client = &client;
        attempt.label = signed_out ? "WEB signed out" : client.name;

        std::vector<http::Header> headers;
        headers.push_back(http::Header{"Content-Type", "application/json"});
        headers.push_back(http::Header{"User-Agent", client.ua});
        headers.push_back(http::Header{"Cookie", kConsentCookie});
        if (!access_token.empty() && !signed_out && client.bearer)
            headers.push_back(http::Header{"Authorization", "Bearer " + access_token});

        http::Response response =
            http::post_json(endpoint_url(endpoint), headers, payload(extra, client), timeout_s);
        status = response.status;
        attempt.status = response.status;
        if (!response.error.empty())
        {
            error = response.error;
            attempt.error = response.error;
            return attempt;
        }

        attempt.transport_ok = true;
        attempt.body = std::move(response.body);
        remember_visitor(attempt.body);
        attempt.http_ok = response.status >= 200 && response.status < 300;
        if (!attempt.http_ok)
            error = "http " + std::to_string(response.status) + " (" + attempt.label + ")";
        return attempt;
    }

    // The statuses that drop a TV answer to the WEB client.
    static bool fallback_status(long value)
    {
        return value == 400 || value == 401 || value == 403 || value == 410;
    }

    void log_line(const char *endpoint, const std::string &target, const Attempt &attempt,
                  std::size_t items) const
    {
        log::line("innertube: %s %s client=%s status=%ld items=%zu", endpoint, target.c_str(),
                  attempt.client->name, attempt.status, items);
        // A 200 with nothing read: name the containers the answer was built
        // from, so a format YouTube switched to is visible in the log.
        if (items == 0 && attempt.http_ok)
            log::line("innertube: %s answer holds %s", endpoint,
                      renderer_census(attempt.body).c_str());
    }

    // "tvBrowseRenderer x1, tileRenderer x24, ..." - the most frequent
    // *Renderer / *ViewModel keys of a JSON answer, scanned as text.
    static std::string renderer_census(const std::string &body)
    {
        std::vector<std::pair<std::string, int>> counts;
        std::size_t at = 0;
        while ((at = body.find('"', at)) != std::string::npos)
        {
            const std::size_t end = body.find('"', at + 1);
            if (end == std::string::npos)
                break;
            const std::string_view key(body.data() + at + 1, end - at - 1);
            at = end + 1;
            if (at >= body.size() || body[at] != ':' || key.size() > 48)
                continue;
            if (!key.ends_with("Renderer") && !key.ends_with("ViewModel"))
                continue;
            bool found = false;
            for (auto &entry : counts)
            {
                if (entry.first == key)
                {
                    ++entry.second;
                    found = true;
                    break;
                }
            }
            if (!found && counts.size() < 64)
                counts.emplace_back(std::string(key), 1);
        }
        std::sort(counts.begin(), counts.end(),
                  [](const auto &a, const auto &b) { return a.second > b.second; });
        std::string out;
        for (std::size_t i = 0; i < counts.size() && i < 6; ++i)
        {
            if (!out.empty())
                out += ", ";
            out += counts[i].first + " x" + std::to_string(counts[i].second);
        }
        return out.empty() ? "no renderers (" + std::to_string(body.size()) + " bytes)" : out;
    }

    // The text one attempt contributes to the accumulated error: the client
    // that was refused for an HTTP failure, the transport text otherwise, and
    // the parse error of a 2xx answer that carried no usable data.
    static std::string attempt_reason(const Attempt &attempt, const std::string &parse_error)
    {
        if (!attempt.transport_ok)
            return attempt.error;
        if (!attempt.http_ok)
            return "http " + std::to_string(attempt.status) + " (" + attempt.label + ")";
        return parse_error;
    }

    // "http 400 (TVHTML5), http 400 (WEB signed out)": every failed attempt in
    // order, so the UI names each client that refused. An attempt with nothing
    // to say (a 2xx answer that simply carried no items) is left out.
    static void join_reasons(const std::vector<std::string> &reasons, std::string *out)
    {
        out->clear();
        for (const std::string &reason : reasons)
        {
            if (reason.empty())
                continue;
            if (!out->empty())
                *out += ", ";
            *out += reason;
        }
    }

    // One browse/search/next call: the client the auth state selects, then -
    // only when that was the TV client and it refused the request (400/401/
    // 403/410) or answered without items - exactly one retry as WEB.
    bool feed_call(const char *endpoint, const std::string &extra, const std::string &target,
                   FeedPage *page)
    {
        const Client &primary = selected_client();
        const bool signed_in_client = std::strcmp(primary.name, kWebClientName) != 0;

        Attempt attempt = post_once(endpoint, extra, primary);
        bool parsed = attempt.http_ok && parse_feed_page(attempt.body, page);
        log_line(endpoint, target, attempt, page->items.size());

        if (attempt.transport_ok && attempt.http_ok && parsed &&
            (!signed_in_client || !page->items.empty()))
        {
            error = page->error;
            return true;
        }

        std::vector<std::string> reasons;
        reasons.push_back(attempt_reason(attempt, page->error));
        // A 401 on an authenticated attempt is the server rejecting the
        // account token, not a stale client version: never paper over it with
        // the signed-out WEB answer (an empty feed the player would read as
        // "no subscriptions"). Surface it so the caller can refresh the token
        // or sign out.
        if (signed_in_client && attempt.status == 401)
        {
            join_reasons(reasons, &error);
            return false;
        }
        if (!signed_in_client || !attempt.transport_ok ||
            !(attempt.http_ok || fallback_status(attempt.status)))
        {
            join_reasons(reasons, &error);
            return false;
        }

        // The signed-out WEB client is what serves the fallback content, so the
        // Bearer header is dropped for this attempt (YouTube answers 400 to it).
        client_name = kWebClientName;
        client_version = kWebClientVersion;
        Attempt retry = post_once(endpoint, extra, kWebClient, /*signed_out=*/true);
        parsed = retry.http_ok && parse_feed_page(retry.body, page);
        log_line(endpoint, target, retry, page->items.size());
        if (retry.transport_ok && retry.http_ok && parsed)
        {
            error = page->error;
            return true;
        }
        reasons.push_back(attempt_reason(retry, page->error));
        join_reasons(reasons, &error);
        return false;
    }

    // The same rule for a channel page (header + tabs + the selected tab's
    // videos): its items are the videos the walker read, so a TV answer that
    // parses to zero of them also falls back to WEB.
    bool channel_call(const std::string &extra, const std::string &target, ChannelPage *page)
    {
        const Client &primary = selected_client();
        const bool signed_in_client = std::strcmp(primary.name, kWebClientName) != 0;

        Attempt attempt = post_once("browse", extra, primary);
        bool parsed = attempt.http_ok && parse_channel_page(attempt.body, page);
        log_line("browse", target, attempt, page->items.size());

        if (attempt.transport_ok && attempt.http_ok && parsed &&
            (!signed_in_client || !page->items.empty()))
        {
            error = page->error;
            return true;
        }

        std::vector<std::string> reasons;
        reasons.push_back(attempt_reason(attempt, page->error));
        // As in feed_call: a 401 on an authenticated attempt rejects the
        // token and reaches the caller; only a stale client drops the page to
        // the signed-out WEB client below.
        if (signed_in_client && attempt.status == 401)
        {
            join_reasons(reasons, &error);
            return false;
        }
        if (!signed_in_client || !attempt.transport_ok ||
            !(attempt.http_ok || fallback_status(attempt.status)))
        {
            join_reasons(reasons, &error);
            return false;
        }

        // As with a feed: the fallback is the signed-out WEB client, so no
        // Bearer header.
        client_name = kWebClientName;
        client_version = kWebClientVersion;
        Attempt retry = post_once("browse", extra, kWebClient, /*signed_out=*/true);
        parsed = retry.http_ok && parse_channel_page(retry.body, page);
        log_line("browse", target, retry, page->items.size());
        if (retry.transport_ok && retry.http_ok && parsed)
        {
            error = page->error;
            return true;
        }
        reasons.push_back(attempt_reason(retry, page->error));
        join_reasons(reasons, &error);
        return false;
    }
};

InnerTube::InnerTube() : impl_(std::make_unique<Impl>())
{
}

InnerTube::~InnerTube() = default;

void InnerTube::set_access_token(std::string token)
{
    impl_->access_token = std::move(token);
}

bool InnerTube::signed_in() const
{
    return !impl_->access_token.empty();
}

void InnerTube::set_timeout(long seconds)
{
    impl_->timeout_s = seconds;
}

bool InnerTube::home(FeedPage *page, const std::string &continuation)
{
    if (page == nullptr)
        return false;
    *page = FeedPage{};
    const bool fresh = continuation.empty();
    const std::string extra = fresh ? field_string("browseId", "FEwhat_to_watch")
                                    : field_string("continuation", continuation);
    const std::string target = fresh ? "FEwhat_to_watch" : "continuation";
    const bool parsed = impl_->feed_call("browse", extra, target, page);
    if (!parsed)
        page->error = impl_->error;
    return parsed;
}

bool InnerTube::subscriptions(FeedPage *page, const std::string &continuation)
{
    if (page == nullptr)
        return false;
    *page = FeedPage{};
    const bool fresh = continuation.empty();
    const std::string extra = fresh ? field_string("browseId", "FEsubscriptions")
                                    : field_string("continuation", continuation);
    const bool parsed =
        impl_->feed_call("browse", extra, fresh ? "FEsubscriptions" : "continuation", page);
    if (!parsed)
        page->error = impl_->error;
    return parsed;
}

bool InnerTube::search(const std::string &query, FeedPage *page, const std::string &continuation)
{
    if (page == nullptr)
        return false;
    *page = FeedPage{};
    std::string extra;
    std::string target;
    if (continuation.empty())
    {
        extra = field_string("query", query);
        target = query;
    }
    else
    {
        extra = field_string("continuation", continuation);
        target = "continuation";
    }
    const bool parsed = impl_->feed_call("search", extra, target, page);
    if (!parsed)
        page->error = impl_->error;
    return parsed;
}

bool InnerTube::player_variants(const std::string &video_id, std::vector<VideoInfo> *out,
                                std::string *error)
{
    if (out == nullptr)
        return false;
    out->clear();
    if (error != nullptr)
        error->clear();

    // Walk every maintained client: one may be refused while another answers,
    // and a stream URL one client hands out can be refused by googlevideo (403)
    // while another client's URL for the same video plays, so every answer is
    // kept. The "confirm you're not a bot" gate is per account and network
    // though - no client gets past it, so it ends the walk and becomes a marker
    // the player screen turns into "sign in first".
    std::string chain;
    std::string first_reason;
    std::string last_reason;
    std::string last_client;
    bool any = false;
    bool agree = true;

    for (const Client &client : kPlayerClients)
    {
        Impl::Attempt attempt = impl_->post_once("player", impl_->player_extra(video_id), client);

        int itag = 0;
        std::string reason;
        bool answered = false;
        if (!attempt.transport_ok)
            reason = attempt.error;
        else if (!attempt.http_ok)
            reason = "http " + std::to_string(attempt.status);
        else
        {
            VideoInfo candidate;
            if (parse_video_info(attempt.body, &candidate))
            {
                candidate.client = client.name;
                const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count();
                // Rounded up, plus a second of slack for the clocks in between.
                candidate.available_at_ms =
                    candidate.preroll_wait_s > 0.0
                        ? now_ms + static_cast<long long>(candidate.preroll_wait_s * 1000.0) + 1000
                        : now_ms;
                log::line("player: %s pre-roll ads=%d wait=%.1fs", client.name,
                          candidate.preroll_ads, candidate.preroll_wait_s);
                // The first answer names the player JS; its signature
                // timestamp then rides every later request of the chain.
                impl_->ensure_sts(candidate.player_js_url);
                log::line("player: %s %s [%s]", client.name,
                          format_summary(candidate.formats).c_str(), candidate.census.c_str());
                if (!candidate.formats.empty())
                    itag = candidate.formats.front().itag;
                answered = true;
                reason = "ok";
                out->push_back(std::move(candidate));
            }
            else
                reason = candidate.error;
        }

        log::line("player: %s -> %s itag=%d", client.name, reason.c_str(), itag);

        if (!chain.empty())
            chain += "; ";
        chain += client.name;
        chain += " -> ";
        chain += reason;

        if (answered)
            continue;

        if (reason.find("not a bot") != std::string::npos ||
            reason.find("LOGIN_REQUIRED") != std::string::npos ||
            reason.find("Sign in") != std::string::npos)
        {
            log::line("player: %s", chain.c_str());
            impl_->error = "login_required";
            if (error != nullptr)
                *error = "login_required";
            return false;
        }

        if (!any)
        {
            first_reason = reason;
            any = true;
        }
        else if (reason != first_reason)
            agree = false;
        last_reason = reason;
        last_client = client.name;
    }

    // Every client agreeing is one reason; otherwise the last one is named so
    // the UI can say which client it came from.
    log::line("player: %s", chain.c_str());
    if (!out->empty())
    {
        impl_->error.clear();
        return true;
    }
    impl_->error = agree ? last_reason : last_reason + " (" + last_client + ")";
    if (error != nullptr)
        *error = impl_->error;
    return false;
}

// The first client that answered, for the callers that only need one stream.
bool InnerTube::player(const std::string &video_id, VideoInfo *info)
{
    if (info == nullptr)
        return false;
    *info = VideoInfo{};

    std::vector<VideoInfo> variants;
    std::string error;
    if (!player_variants(video_id, &variants, &error))
    {
        info->error = error.empty() ? impl_->error : error;
        return false;
    }
    *info = std::move(variants.front());
    return true;
}

bool InnerTube::related(const std::string &video_id, FeedPage *page)
{
    if (page == nullptr)
        return false;
    *page = FeedPage{};
    const std::string extra = field_string("videoId", video_id);
    const bool parsed = impl_->feed_call("next", extra, video_id, page);
    if (!parsed)
        page->error = impl_->error;
    return parsed;
}

bool InnerTube::channel_page(const std::string &channel_id, ChannelPage *page)
{
    if (page == nullptr)
        return false;
    *page = ChannelPage{};
    const std::string extra = field_string("browseId", channel_id);
    const bool parsed = impl_->channel_call(extra, channel_id, page);
    if (!parsed)
        page->error = impl_->error;
    page->channel.channel_id = channel_id;
    return parsed;
}

bool InnerTube::channel_tab(const std::string &channel_id, const std::string &params,
                            FeedPage *page)
{
    if (page == nullptr)
        return false;
    *page = FeedPage{};
    std::string extra = field_string("browseId", channel_id);
    extra += ',';
    extra += field_string("params", params);
    const bool parsed = impl_->feed_call("browse", extra, params, page);
    if (!parsed)
        page->error = impl_->error;
    return parsed;
}

long InnerTube::last_status() const
{
    return impl_->status;
}

const std::string &InnerTube::last_error() const
{
    return impl_->error;
}

} // namespace yt

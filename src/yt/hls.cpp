// youtube-tv - HLS playlists for live streams.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// YouTube serves a live broadcast as HLS: a master playlist
// (streamingData.hlsManifestUrl) names one media playlist per quality, and
// each media playlist lists the newest few seconds-long MPEG-TS segments.
// The adaptive formats of a live answer only work through a player that
// speaks YouTube's segment protocol, so live streams play from here instead.

#include "yt/hls.hpp"

#include "yt/log.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace yt::hls
{

namespace
{

bool starts_with(const std::string &text, const char *prefix)
{
    return text.rfind(prefix, 0) == 0;
}

// The value of KEY in an attribute list (KEY=value,KEY2="quoted, value").
std::string attribute(const std::string &list, const std::string &key)
{
    std::size_t at = 0;
    while (at < list.size())
    {
        const std::size_t equals = list.find('=', at);
        if (equals == std::string::npos)
            return {};
        const std::string name = list.substr(at, equals - at);
        std::size_t value_start = equals + 1;
        std::size_t value_end;
        std::string value;
        if (value_start < list.size() && list[value_start] == '"')
        {
            const std::size_t close = list.find('"', value_start + 1);
            value_end = close == std::string::npos ? list.size() : close + 1;
            value =
                list.substr(value_start + 1,
                            (close == std::string::npos ? list.size() : close) - value_start - 1);
        }
        else
        {
            const std::size_t comma = list.find(',', value_start);
            value_end = comma == std::string::npos ? list.size() : comma;
            value = list.substr(value_start, value_end - value_start);
        }
        if (name == key)
            return value;
        at = value_end;
        if (at < list.size() && list[at] == ',')
            ++at;
    }
    return {};
}

std::vector<std::string> lines_of(const std::string &text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        std::size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t'))
            ++lead;
        lines.push_back(line.substr(lead));
        if (end == text.size())
            break;
        start = end + 1;
    }
    return lines;
}

int itag_of(const std::string &url)
{
    for (const char *key : {"/itag/", "itag="})
    {
        const std::size_t at = url.find(key);
        if (at != std::string::npos)
        {
            const int value = std::atoi(url.c_str() + at + std::string(key).size());
            if (value > 0)
                return value;
        }
    }
    return 0;
}

} // namespace

std::string resolve(const std::string &base, const std::string &ref)
{
    if (starts_with(ref, "http://") || starts_with(ref, "https://"))
        return ref;
    const std::size_t scheme = base.find("://");
    if (scheme == std::string::npos)
        return ref;
    if (starts_with(ref, "//"))
        return base.substr(0, scheme + 1) + ref;
    if (starts_with(ref, "/"))
    {
        const std::size_t path = base.find('/', scheme + 3);
        return (path == std::string::npos ? base : base.substr(0, path)) + ref;
    }
    std::string directory = base;
    const std::size_t query = directory.find('?');
    if (query != std::string::npos)
        directory.resize(query);
    const std::size_t slash = directory.rfind('/');
    if (slash != std::string::npos && slash > scheme + 2)
        directory.resize(slash + 1);
    else
        directory += '/';
    return directory + ref;
}

Codec Variant::video_codec() const
{
    if (codecs.find("avc1") != std::string::npos || codecs.find("avc3") != std::string::npos)
        return Codec::avc;
    if (codecs.find("vp09") != std::string::npos || codecs.find("vp9") != std::string::npos)
        return Codec::vp9;
    if (codecs.find("av01") != std::string::npos)
        return Codec::av1;
    // No CODECS attribute: YouTube's live variants are H.264.
    return codecs.empty() && height > 0 ? Codec::avc : Codec::unknown;
}

bool Variant::has_video() const
{
    return height > 0 && video_codec() != Codec::unknown;
}

std::vector<Variant> parse_master(const std::string &text, const std::string &base_url)
{
    std::vector<Variant> variants;
    struct Group
    {
        std::string id;
        std::string url;
        bool is_default = false;
    };
    std::vector<Group> groups;
    const std::vector<std::string> lines = lines_of(text);
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::string &line = lines[i];
        if (starts_with(line, "#EXT-X-MEDIA:"))
        {
            const std::string list = line.substr(13);
            if (attribute(list, "TYPE") != "AUDIO")
                continue;
            const std::string uri = attribute(list, "URI");
            if (uri.empty())
                continue;
            groups.push_back({attribute(list, "GROUP-ID"), resolve(base_url, uri),
                              attribute(list, "DEFAULT") == "YES"});
            continue;
        }
        if (!starts_with(line, "#EXT-X-STREAM-INF:"))
            continue;
        const std::string list = line.substr(18);
        Variant variant;
        variant.bandwidth = std::atoll(attribute(list, "BANDWIDTH").c_str());
        const std::string resolution = attribute(list, "RESOLUTION");
        const std::size_t x = resolution.find('x');
        if (x != std::string::npos)
        {
            variant.width = std::atoi(resolution.c_str());
            variant.height = std::atoi(resolution.c_str() + x + 1);
        }
        variant.fps = std::atof(attribute(list, "FRAME-RATE").c_str());
        variant.codecs = attribute(list, "CODECS");
        variant.audio_group = attribute(list, "AUDIO");
        // The URI is the next line that is not a tag or blank.
        std::size_t next = i + 1;
        while (next < lines.size() && (lines[next].empty() || lines[next][0] == '#'))
            ++next;
        if (next >= lines.size())
            break;
        variant.url = resolve(base_url, lines[next]);
        variant.itag = itag_of(variant.url);
        variants.push_back(std::move(variant));
        i = next;
    }
    for (Variant &variant : variants)
    {
        if (variant.audio_group.empty())
            continue;
        const Group *pick = nullptr;
        for (const Group &group : groups)
        {
            if (group.id != variant.audio_group)
                continue;
            if (pick == nullptr || (group.is_default && !pick->is_default))
                pick = &group;
        }
        if (pick != nullptr)
            variant.audio_url = pick->url;
    }
    return variants;
}

MediaPlaylist parse_media(const std::string &text, const std::string &base_url)
{
    MediaPlaylist playlist;
    const std::vector<std::string> lines = lines_of(text);
    if (lines.empty() || !starts_with(lines.front(), "#EXTM3U"))
        return playlist;
    std::int64_t sequence = 0;
    bool sequence_known = false;
    double duration = 0.0;
    for (const std::string &line : lines)
    {
        if (line.empty())
            continue;
        if (starts_with(line, "#EXT-X-STREAM-INF:"))
            return MediaPlaylist{}; // a master playlist, not a media one
        if (starts_with(line, "#EXT-X-TARGETDURATION:"))
            playlist.target_duration = std::atof(line.c_str() + 22);
        else if (starts_with(line, "#EXT-X-MEDIA-SEQUENCE:"))
        {
            playlist.media_sequence = std::atoll(line.c_str() + 22);
            if (!sequence_known)
                sequence = playlist.media_sequence;
            sequence_known = true;
        }
        else if (starts_with(line, "#EXT-X-ENDLIST"))
            playlist.ended = true;
        else if (starts_with(line, "#EXT-X-MAP:"))
        {
            const std::string uri = attribute(line.substr(11), "URI");
            if (!uri.empty())
                playlist.map_url = resolve(base_url, uri);
        }
        else if (starts_with(line, "#EXTINF:"))
            duration = std::atof(line.c_str() + 8);
        else if (line[0] != '#')
        {
            Segment segment;
            segment.sequence = sequence++;
            segment.duration = duration;
            segment.url = resolve(base_url, line);
            playlist.segments.push_back(std::move(segment));
            duration = 0.0;
        }
    }
    playlist.valid = true;
    return playlist;
}

std::vector<Format> variant_formats(const std::vector<Variant> &variants)
{
    std::vector<Format> formats;
    for (std::size_t i = 0; i < variants.size(); ++i)
    {
        const Variant &variant = variants[i];
        if (!variant.has_video())
            continue;
        Format format;
        // An itag the stream selection can tell apart even when the URL names
        // none (1000 + the variant's index).
        format.itag = variant.itag > 0 ? variant.itag : 1000 + static_cast<int>(i);
        format.url = variant.url;
        format.codec = variant.video_codec();
        format.width = variant.width;
        format.height = variant.height;
        format.fps = variant.fps > 0.0 ? variant.fps : 30.0;
        format.bitrate = variant.bandwidth;
        formats.push_back(std::move(format));
    }
    return formats;
}

LiveFeed::LiveFeed(std::string playlist_url, Fetch fetch, int back)
    : url_(std::move(playlist_url)), fetch_(std::move(fetch)), back_(back < 1 ? 1 : back)
{
}

double LiveFeed::fail(std::string text, long status)
{
    error_ = std::move(text);
    error_status_ = status;
    return -1.0;
}

double LiveFeed::round(const Emit &emit)
{
    constexpr int kMaxFailures = 6;
    const http::Response response = fetch_(url_);
    MediaPlaylist playlist;
    if (response.http_ok())
        playlist = parse_media(response.body, url_);
    if (!playlist.valid)
    {
        const long status = response.status;
        if (status == 403 || status == 404 || status == 410)
            return fail("live playlist: HTTP " + std::to_string(status), status);
        if (++failures_ > kMaxFailures)
        {
            return fail(response.ok() ? "live playlist: HTTP " + std::to_string(status)
                                      : "live playlist: " + response.error,
                        status);
        }
        log::line("live: playlist %s (HTTP %ld), retry %d",
                  response.ok() ? "unreadable" : response.error.c_str(), status, failures_);
        return 0.5 * failures_;
    }
    failures_ = 0;

    if (!playlist.map_url.empty() && !map_sent_)
    {
        const http::Response init = fetch_(playlist.map_url);
        if (!init.http_ok())
            return fail("live init segment: HTTP " + std::to_string(init.status), init.status);
        if (!emit(init.body))
            return -1.0;
        map_sent_ = true;
    }

    const std::int64_t last =
        playlist.segments.empty() ? playlist.media_sequence - 1 : playlist.segments.back().sequence;
    if (next_ < 0)
    {
        // A finished broadcast plays from its start; a running one from a
        // few segments behind its edge.
        next_ = playlist.ended ? playlist.media_sequence
                               : std::max(playlist.media_sequence, last - back_ + 1);
        log::line("live: start at segment %lld of %lld..%lld (target %.0f s%s)",
                  static_cast<long long>(next_), static_cast<long long>(playlist.media_sequence),
                  static_cast<long long>(last), playlist.target_duration,
                  playlist.ended ? ", ended" : "");
    }
    else if (next_ < playlist.media_sequence)
    {
        // The playlist moved on while we were away: the missed segments are
        // gone from the server, so carry on from the oldest one it still has.
        log::line("live: skipped %lld segments that left the playlist",
                  static_cast<long long>(playlist.media_sequence - next_));
        next_ = playlist.media_sequence;
    }

    for (const Segment &segment : playlist.segments)
    {
        if (segment.sequence < next_)
            continue;
        http::Response body;
        int tries = 0;
        for (;;)
        {
            body = fetch_(segment.url);
            if (body.http_ok() || body.status == 403 || body.status == 404 || body.status == 410 ||
                ++tries >= 3)
                break;
        }
        if (body.status == 404 || body.status == 410)
        {
            log::line("live: segment %lld gone (HTTP %ld), skipped",
                      static_cast<long long>(segment.sequence), body.status);
            next_ = segment.sequence + 1;
            continue;
        }
        if (!body.http_ok())
        {
            return fail(body.ok() ? "live segment: HTTP " + std::to_string(body.status)
                                  : "live segment: " + body.error,
                        body.status);
        }
        if (!emit(body.body))
            return -1.0;
        next_ = segment.sequence + 1;
    }

    if (playlist.ended && next_ > last)
    {
        ended_ = true;
        return -1.0;
    }
    // A new segment shows up about once per target duration; checking at
    // half of it keeps the picture close to the edge without hammering.
    const double target = playlist.target_duration > 0.0 ? playlist.target_duration : 2.0;
    return std::max(0.5, target * 0.5);
}

} // namespace yt::hls

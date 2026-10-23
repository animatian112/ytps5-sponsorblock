// youtube-tv - HLS playlists: master and media parsing, and the live feed.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/hls.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{

constexpr const char *kMaster = R"(#EXTM3U
#EXT-X-INDEPENDENT-SEGMENTS
#EXT-X-STREAM-INF:BANDWIDTH=290400,CODECS="avc1.4d400c,mp4a.40.5",RESOLUTION=256x144,FRAME-RATE=30
https://manifest.googlevideo.com/api/manifest/hls_playlist/expire/1/id/abc/itag/91/playlist/index.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=4686000,CODECS="avc1.4d4020,mp4a.40.2",RESOLUTION=1280x720,FRAME-RATE=60
https://manifest.googlevideo.com/api/manifest/hls_playlist/expire/1/id/abc/itag/300/playlist/index.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=7224000,CODECS="avc1.64002a,mp4a.40.2",RESOLUTION=1920x1080,FRAME-RATE=60
https://manifest.googlevideo.com/api/manifest/hls_playlist/expire/1/id/abc/itag/301/playlist/index.m3u8
)";

constexpr const char *kMedia = R"(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:5
#EXT-X-MEDIA-SEQUENCE:100
#EXTINF:5.005,
https://rr1.googlevideo.com/videoplayback/id/abc/itag/301/sq/100/file/seg.ts
#EXTINF:5.005,
https://rr1.googlevideo.com/videoplayback/id/abc/itag/301/sq/101/file/seg.ts
#EXTINF:5.005,
https://rr1.googlevideo.com/videoplayback/id/abc/itag/301/sq/102/file/seg.ts
#EXTINF:5.005,
https://rr1.googlevideo.com/videoplayback/id/abc/itag/301/sq/103/file/seg.ts
)";

} // namespace

TEST(Hls, ReadsTheMasterPlaylistVariants)
{
    const auto variants = yt::hls::parse_master(kMaster, "https://manifest.googlevideo.com/x");
    ASSERT_EQ(variants.size(), 3u);
    EXPECT_EQ(variants[2].height, 1080);
    EXPECT_EQ(variants[2].width, 1920);
    EXPECT_DOUBLE_EQ(variants[2].fps, 60.0);
    EXPECT_EQ(variants[2].itag, 301);
    EXPECT_EQ(variants[2].codecs, "avc1.64002a,mp4a.40.2");
    EXPECT_EQ(variants[2].video_codec(), yt::Codec::avc);
    EXPECT_TRUE(variants[2].audio_url.empty()); // muxed sound

    const auto formats = yt::hls::variant_formats(variants);
    ASSERT_EQ(formats.size(), 3u);
    EXPECT_EQ(formats[1].itag, 300);
    EXPECT_EQ(formats[1].height, 720);
    EXPECT_EQ(formats[1].codec, yt::Codec::avc);
    EXPECT_EQ(formats[1].url, variants[1].url);
}

TEST(Hls, SeparateAudioGroupsAndRelativeUrls)
{
    const auto variants = yt::hls::parse_master(
        "#EXTM3U\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"a\",DEFAULT=YES,URI=\"audio/index.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1000,RESOLUTION=640x360,CODECS=\"avc1.4d401e\",AUDIO=\"aud\"\n"
        "video/360.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=64000,CODECS=\"mp4a.40.2\",AUDIO=\"aud\"\n"
        "audio/index.m3u8\n",
        "https://host.example/live/master.m3u8?token=1");
    ASSERT_EQ(variants.size(), 2u);
    EXPECT_EQ(variants[0].url, "https://host.example/live/video/360.m3u8");
    EXPECT_EQ(variants[0].audio_url, "https://host.example/live/audio/index.m3u8");
    EXPECT_FALSE(variants[1].has_video()); // audio only
    EXPECT_EQ(yt::hls::variant_formats(variants).size(), 1u);

    EXPECT_EQ(yt::hls::resolve("https://a.b/c/d.m3u8", "/e.ts"), "https://a.b/e.ts");
    EXPECT_EQ(yt::hls::resolve("https://a.b/c/d.m3u8", "//x.y/z.ts"), "https://x.y/z.ts");
    EXPECT_EQ(yt::hls::resolve("https://a.b/c/d.m3u8", "https://q.r/s"), "https://q.r/s");
}

TEST(Hls, ReadsTheMediaPlaylist)
{
    const auto playlist = yt::hls::parse_media(kMedia, "https://manifest.googlevideo.com/x");
    ASSERT_TRUE(playlist.valid);
    EXPECT_DOUBLE_EQ(playlist.target_duration, 5.0);
    EXPECT_EQ(playlist.media_sequence, 100);
    EXPECT_FALSE(playlist.ended);
    ASSERT_EQ(playlist.segments.size(), 4u);
    EXPECT_EQ(playlist.segments[3].sequence, 103);
    EXPECT_NEAR(playlist.segments[0].duration, 5.005, 1e-9);

    EXPECT_FALSE(yt::hls::parse_media("<html>", "https://a.b/").valid);
    EXPECT_FALSE(yt::hls::parse_media(kMaster, "https://a.b/").valid);
}

TEST(Hls, LiveFeedStartsBehindTheEdgeAndFollowsIt)
{
    std::string playlist = kMedia;
    auto fetch = [&](const std::string &url)
    {
        yt::http::Response response;
        if (url == "https://pl")
        {
            response.status = 200;
            response.body = playlist;
            return response;
        }
        const std::size_t sq = url.find("/sq/");
        response.status = 200;
        response.body = "seg" + url.substr(sq + 4, 3);
        return response;
    };
    std::vector<std::string> got;
    auto emit = [&](const std::string &bytes)
    {
        got.push_back(bytes);
        return true;
    };

    yt::hls::LiveFeed feed("https://pl", fetch, 3);
    EXPECT_GT(feed.round(emit), 0.0);
    ASSERT_EQ(got.size(), 3u); // 101, 102, 103: three behind the edge
    EXPECT_EQ(got[0], "seg101");
    EXPECT_EQ(got[2], "seg103");

    // Nothing new: nothing emitted.
    EXPECT_GT(feed.round(emit), 0.0);
    EXPECT_EQ(got.size(), 3u);

    // The edge moves on by one, and the broadcast ends after it.
    playlist += "#EXTINF:5.005,\nhttps://rr1.googlevideo.com/videoplayback/sq/104/file/seg.ts\n"
                "#EXT-X-ENDLIST\n";
    EXPECT_LT(feed.round(emit), 0.0);
    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got[3], "seg104");
    EXPECT_TRUE(feed.ended());
    EXPECT_TRUE(feed.error().empty());
}

TEST(Hls, LiveFeedStopsOnAForbiddenPlaylist)
{
    auto fetch = [](const std::string &)
    {
        yt::http::Response response;
        response.status = 403;
        return response;
    };
    yt::hls::LiveFeed feed("https://pl", fetch);
    EXPECT_LT(feed.round([](const std::string &) { return true; }), 0.0);
    EXPECT_FALSE(feed.ended());
    EXPECT_EQ(feed.error_status(), 403);
}

// youtube-tv - InnerTube parsing: the shapes the TV client answers with.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fixtures are hand-written copies of the fields the client reads
// (tests/fixtures/*.json): the walker has to survive renamed renderers, and
// it must never surface an ad.

#include "yt/codec.hpp"
#include "yt/innertube.hpp"

#include "core/save_file.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace
{

std::string fixture(const char *name)
{
    std::string data;
    const std::string path = std::string(HUI_SOURCE_DIR) + "/tests/fixtures/" + name;
    EXPECT_TRUE(hui::save::read_file(path, &data)) << path;
    return data;
}

TEST(InnerTubeFeed, ReadsEveryVideoShapeAndSkipsTheAd)
{
    yt::FeedPage page;
    ASSERT_TRUE(yt::parse_feed_page(fixture("feed_home.json"), &page)) << page.error;
    ASSERT_EQ(page.items.size(), 3u);

    EXPECT_EQ(page.items[0].video_id, "aqz-KE-bpKQ");
    EXPECT_EQ(page.items[0].title, "Big Buck Bunny");
    EXPECT_EQ(page.items[0].author, "Blender Foundation");
    EXPECT_EQ(page.items[0].length_s, 634.0);
    EXPECT_FALSE(page.items[0].live);
    EXPECT_EQ(page.items[0].thumbnail_url, "https://i.ytimg.com/vi/aqz-KE-bpKQ/maxresdefault.jpg");

    EXPECT_EQ(page.items[1].video_id, "LIVE-EXAMPLE1");
    EXPECT_EQ(page.items[1].author, "StudioKanal");
    EXPECT_TRUE(page.items[1].live);
    EXPECT_EQ(page.items[1].title, "Live aus dem Studio");

    // compactVideoRenderer, length in seconds as a string.
    EXPECT_EQ(page.items[2].video_id, "HISTORY-0001");
    EXPECT_EQ(page.items[2].length_s, 1830.0);

    for (const yt::FeedItem &item : page.items)
        EXPECT_EQ(item.video_id.find("AD-VIDEO"), std::string::npos) << "an ad reached the feed";

    EXPECT_EQ(page.continuation, "CBQSC2hvbWUtcGFnZS0x");
}

TEST(InnerTubeFeed, ReadsSearchChannelsAndKeepsThemListsApart)
{
    yt::FeedPage page;
    ASSERT_TRUE(yt::parse_feed_page(fixture("feed_search_channels.json"), &page)) << page.error;

    // Only the two videos: the ad subtree never reaches either list, and a
    // channel renderer never becomes a video.
    ASSERT_EQ(page.items.size(), 2u);
    EXPECT_EQ(page.items[0].video_id, "SEARCH-VID-01");
    EXPECT_EQ(page.items[0].title, "Kanalvideo Eins");
    EXPECT_EQ(page.items[1].video_id, "SEARCH-VID-02");
    EXPECT_EQ(page.items[1].title, "Kanalvideo Zwei");
    for (const yt::FeedItem &item : page.items)
        EXPECT_EQ(item.video_id.find("SEARCH-VID"), 0u) << "only videos belong in items";

    // Three unique channels in encounter order; the repeat of the first
    // channelId collapses.
    ASSERT_EQ(page.channels.size(), 3u);
    EXPECT_EQ(page.channels[0].channel_id, "UCaaaaaaaaaaaaaaaaaaaaaa");
    EXPECT_EQ(page.channels[1].channel_id, "UCbbbbbbbbbbbbbbbbbbbbbb");
    EXPECT_EQ(page.channels[2].channel_id, "UCcccccccccccccccccccccc");

    // videoCountText.simpleText, and the widest of the two thumbnails.
    EXPECT_EQ(page.channels[0].name, "Alpha Kanal");
    EXPECT_EQ(page.channels[0].subscribers, "1.2M subscribers");
    EXPECT_EQ(page.channels[0].thumbnail_url, "https://yt3.ggpht.com/alpha-hq.jpg");

    // title.runs[0].text and subscriberCountText.simpleText.
    EXPECT_EQ(page.channels[1].name, "Beta Kanal");
    EXPECT_EQ(page.channels[1].subscribers, "45K subscribers");
    EXPECT_EQ(page.channels[1].thumbnail_url, "https://yt3.ggpht.com/beta.jpg");

    // subscriberCountText.runs[0].text.
    EXPECT_EQ(page.channels[2].name, "Gamma Kanal");
    EXPECT_EQ(page.channels[2].subscribers, "999 subscribers");
    EXPECT_EQ(page.channels[2].thumbnail_url, "https://yt3.ggpht.com/gamma.jpg");

    EXPECT_EQ(page.continuation, "CBQSCHNlYXJjaC1jaGFubmVscy0x");
}

TEST(InnerTubeFeed, ReadsTvSearchChannelLockupsAndKeepsThemListsApart)
{
    yt::FeedPage page;
    ASSERT_TRUE(yt::parse_feed_page(fixture("feed_search_channels_tv.json"), &page)) << page.error;

    // The video lockup fills the grid; the channel lockup must not leak into
    // it, exactly like a channelRenderer.
    ASSERT_EQ(page.items.size(), 1u);
    EXPECT_EQ(page.items[0].video_id, "TVVID000001");
    EXPECT_EQ(page.items[0].title, "TV Video Eins");
    EXPECT_EQ(page.items[0].author, "Alpha Kanal");
    EXPECT_EQ(page.items[0].length_s, 581.0);
    for (const yt::FeedItem &item : page.items)
        EXPECT_EQ(item.video_id.find("UCm9COMxXKm05BQWNv-IpyPg"), std::string::npos)
            << "a channel lockup reached the video grid";

    // The channel lockup takes the channelRenderer slot.
    ASSERT_EQ(page.channels.size(), 1u);
    EXPECT_EQ(page.channels[0].channel_id, "UCm9COMxXKm05BQWNv-IpyPg");
    EXPECT_EQ(page.channels[0].name, "MODDED WARFARE");
    EXPECT_EQ(page.channels[0].subscribers, "565K subscribers");
    EXPECT_EQ(page.channels[0].thumbnail_url,
              "https://yt3.ggpht.com/w3mTlQRg=s176-c-k-c0x00ffffff-no-rj-mo");
}

TEST(InnerTubeFeed, GarbageIsAnErrorAndAnEmptyListIsNot)
{
    yt::FeedPage page;
    EXPECT_FALSE(yt::parse_feed_page("not json at all", &page));
    EXPECT_FALSE(page.error.empty());

    page = yt::FeedPage{};
    EXPECT_TRUE(yt::parse_feed_page("{}", &page));
    EXPECT_TRUE(page.items.empty());
    EXPECT_TRUE(page.error.empty());
}

TEST(InnerTubePlayer, ReadsTheAdaptiveFormatsAndTheirCodecs)
{
    yt::VideoInfo info;
    ASSERT_TRUE(yt::parse_video_info(fixture("player_ok.json"), &info)) << info.error;
    EXPECT_EQ(info.video_id, "aqz-KE-bpKQ");
    EXPECT_EQ(info.title, "Big Buck Bunny");
    EXPECT_EQ(info.author, "Blender Foundation");
    EXPECT_EQ(info.length_s, 634.0);
    EXPECT_FALSE(info.live);
    ASSERT_EQ(info.formats.size(), 5u);

    EXPECT_EQ(info.formats[0].codec, yt::Codec::avc);
    EXPECT_EQ(info.formats[0].itag, 137);
    EXPECT_EQ(info.formats[0].height, 1080);
    EXPECT_EQ(info.formats[1].codec, yt::Codec::vp9);
    EXPECT_EQ(info.formats[2].codec, yt::Codec::av1);
    EXPECT_EQ(info.formats[2].height, 1080);
    EXPECT_EQ(info.formats[3].codec, yt::Codec::opus);
    EXPECT_EQ(info.formats[3].sample_rate, 48000);
    EXPECT_EQ(info.formats[4].codec, yt::Codec::aac);
    EXPECT_EQ(info.formats[4].sample_rate, 44100);

    // The stream selection then picks a codec the settings allow.
    yt::CodecChoice choice;
    const yt::Format *picked = yt::select_video_format(info.formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, yt::Codec::av1);
    const yt::Format *sound = yt::select_audio_format(info.formats);
    ASSERT_NE(sound, nullptr);
    EXPECT_EQ(sound->codec, yt::Codec::opus);
}

TEST(InnerTubePlayer, ARefusedPlaybackBecomesAnError)
{
    yt::VideoInfo info;
    EXPECT_FALSE(yt::parse_video_info(
        R"({"playabilityStatus":{"status":"UNPLAYABLE","reason":"Video unavailable"}})", &info));
    EXPECT_NE(info.error.find("Video unavailable"), std::string::npos) << info.error;
}

TEST(InnerTubePlayer, ASabrOnlyAnswerIsNamedAndKeptRaw)
{
    // The SABR-only shape (yt-dlp issue #12482): an OK status and a
    // serverAbrStreamingUrl, adaptive entries with DRM metadata but no url and
    // no cipher. Nothing is playable, and the answer must say why.
    const std::string body = fixture("player_sabr.json");
    yt::VideoInfo info;
    ASSERT_TRUE(yt::parse_video_info(body, &info)) << info.error;
    EXPECT_EQ(info.video_id, "SABR-ONLY-01");
    EXPECT_TRUE(info.formats.empty());
    EXPECT_TRUE(info.sabr_only);
    EXPECT_EQ(info.census, "3 adaptive, 0 with url, 0 with cipher, 3 drm, sabr-url present");
    // The untouched body is kept verbatim so the failure can be reproduced.
    EXPECT_EQ(info.raw_body, body);
}

TEST(InnerTubePlayer, AUsableAnswerIsNotSabrOnlyAndKeepsNoRawBody)
{
    yt::VideoInfo info;
    ASSERT_TRUE(yt::parse_video_info(fixture("player_ok.json"), &info)) << info.error;
    ASSERT_EQ(info.formats.size(), 5u);
    EXPECT_FALSE(info.sabr_only);
    EXPECT_EQ(info.census, "5 adaptive, 5 with url, 0 with cipher, 0 drm, sabr-url absent");
    EXPECT_TRUE(info.raw_body.empty());
}

TEST(InnerTubePlayer, ReadsTheSignatureTimestampFromPlayerJs)
{
    // yt-dlp's `(?:signatureTimestamp|sts)\s*:\s*([0-9]{5})` over a snippet.
    EXPECT_EQ(yt::signature_timestamp("var a={signatureTimestamp:20732};"), "20732");
    EXPECT_EQ(yt::signature_timestamp("x.sts : 12345,"), "12345");
    EXPECT_EQ(yt::signature_timestamp("\"signatureTimestamp\":20732"), "20732");
    // Unanchored like the regex: a longer digit run still yields five digits.
    EXPECT_EQ(yt::signature_timestamp("signatureTimestamp:123456"), "12345");
    EXPECT_EQ(yt::signature_timestamp("no timestamp here"), "");
    EXPECT_EQ(yt::signature_timestamp(""), "");
}

TEST(InnerTubeChannel, ReadsTheTabsTheHeaderAndTheSelectedTabItems)
{
    yt::ChannelPage page;
    ASSERT_TRUE(yt::parse_channel_page(fixture("channel_page.json"), &page)) << page.error;

    // Home, Videos and Live, in encounter order; only Home says it is selected.
    ASSERT_EQ(page.tabs.size(), 3u);
    EXPECT_EQ(page.tabs[0].title, "Home");
    EXPECT_EQ(page.tabs[0].params, "EgZob21l");
    EXPECT_TRUE(page.tabs[0].selected);
    EXPECT_EQ(page.tabs[1].title, "Videos");
    EXPECT_EQ(page.tabs[1].params, "EgZ2aWRlb3M");
    EXPECT_FALSE(page.tabs[1].selected);
    EXPECT_EQ(page.tabs[2].title, "Live");
    EXPECT_EQ(page.tabs[2].params, "EglsaXZlc3RyZWFt");
    EXPECT_FALSE(page.tabs[2].selected);

    // The name comes from pageHeaderViewModel.title.dynamicTextViewModel, the
    // avatar is the widest thumbnail, the subscriber line lives in the
    // metadata rows.
    EXPECT_EQ(page.channel.name, "Alpha Kanal");
    EXPECT_EQ(page.channel.thumbnail_url, "https://yt3.ggpht.com/alpha-hq.jpg");
    EXPECT_EQ(page.channel.subscribers, "1.2M subscribers");

    // The same response already carries the selected tab's videos.
    ASSERT_EQ(page.items.size(), 2u);
    EXPECT_EQ(page.items[0].video_id, "CHAN-VID-01");
    EXPECT_EQ(page.items[0].title, "Kanalvideo Eins");
    EXPECT_EQ(page.items[0].length_s, 754.0);
    EXPECT_EQ(page.items[1].video_id, "CHAN-VID-02");
    EXPECT_EQ(page.items[1].title, "Kanalvideo Zwei");
}

TEST(InnerTubeChannel, AFirstTabWithoutASelectedFlagBecomesSelected)
{
    yt::ChannelPage page;
    const std::string json =
        R"({"contents":{"tabs":[{"tabRenderer":{"title":{"simpleText":"Home"}}},)"
        R"({"tabRenderer":{"title":{"simpleText":"Videos"},"selected":true}}]}})";
    ASSERT_TRUE(yt::parse_channel_page(json, &page)) << page.error;
    ASSERT_EQ(page.tabs.size(), 2u);
    EXPECT_FALSE(page.tabs[0].selected);
    EXPECT_TRUE(page.tabs[1].selected);

    yt::ChannelPage other;
    const std::string none =
        R"({"contents":{"tabs":[{"tabRenderer":{"title":{"simpleText":"Home"}}},)"
        R"({"tabRenderer":{"title":{"simpleText":"Videos"}}}]}})";
    ASSERT_TRUE(yt::parse_channel_page(none, &other)) << other.error;
    ASSERT_EQ(other.tabs.size(), 2u);
    EXPECT_TRUE(other.tabs[0].selected) << "the first tab is the fallback";
    EXPECT_FALSE(other.tabs[1].selected);
}

TEST(InnerTubeFeed, ProtocolRelativeThumbnailsGetAScheme)
{
    // Search channel avatars arrive protocol-relative ("//yt3.ggpht.com/..."),
    // which libcurl cannot open, so the parser hands every media URL out
    // through absolute_url().
    yt::FeedPage page;
    ASSERT_TRUE(yt::parse_feed_page(fixture("player_variants.json"), &page)) << page.error;

    ASSERT_EQ(page.channels.size(), 1u);
    EXPECT_EQ(page.channels[0].thumbnail_url, "https://yt3.ggpht.com/proto-hq.jpg");

    // A classic videoRenderer thumbnail and a lockupViewModel image.sources
    // both leave with a scheme too.
    ASSERT_EQ(page.items.size(), 2u);
    EXPECT_EQ(page.items[0].thumbnail_url, "https://i.ytimg.com/vi/PROTO-VID-01/hqdefault.jpg");
    EXPECT_EQ(page.items[1].video_id, "PROTO-LOCK1");
    EXPECT_EQ(page.items[1].thumbnail_url, "https://i.ytimg.com/vi/PROTO-LOCK1/hqdefault.jpg");
}

TEST(InnerTubeChannel, HeaderAvatarFallsBackThroughThreeShapes)
{
    // The decorated avatar is the only shape on the page; its protocol-relative
    // URL still leaves through absolute_url().
    yt::ChannelPage decorated;
    const std::string with_decorated =
        R"({"pageHeaderViewModel":{"title":{"simpleText":"Alpha Kanal"},"image":)"
        R"({"decoratedAvatarViewModel":{"avatar":{"avatarViewModel":{"image":{"sources":)"
        R"([{"url":"//yt3.ggpht.com/deco.jpg","width":176}]}}}}}}})";
    ASSERT_TRUE(yt::parse_channel_page(with_decorated, &decorated)) << decorated.error;
    EXPECT_EQ(decorated.channel.thumbnail_url, "https://yt3.ggpht.com/deco.jpg");

    // Both shapes are present but the channelMetadataRenderer avatar publishes
    // the larger width, so it wins over the decorated avatar.
    yt::ChannelPage metadata;
    const std::string with_metadata =
        R"({"pageHeaderViewModel":{"title":{"simpleText":"Beta Kanal"},"image":)"
        R"({"decoratedAvatarViewModel":{"avatar":{"avatarViewModel":{"image":{"sources":)"
        R"([{"url":"//yt3.ggpht.com/deco-small.jpg","width":72}]}}}}}},)"
        R"("channelMetadataRenderer":{"title":"Beta Kanal","avatar":{"thumbnails":)"
        R"([{"url":"//yt3.ggpht.com/meta-large.jpg","width":900}]}}})";
    ASSERT_TRUE(yt::parse_channel_page(with_metadata, &metadata)) << metadata.error;
    EXPECT_EQ(metadata.channel.thumbnail_url, "https://yt3.ggpht.com/meta-large.jpg");

    // On equal widths the first shape encountered wins, the decorated avatar.
    yt::ChannelPage tie;
    const std::string with_tie =
        R"({"pageHeaderViewModel":{"title":{"simpleText":"Gamma Kanal"},"image":)"
        R"({"decoratedAvatarViewModel":{"avatar":{"avatarViewModel":{"image":{"sources":)"
        R"([{"url":"//yt3.ggpht.com/deco.jpg","width":176}]}}}}}},)"
        R"("channelMetadataRenderer":{"title":"Gamma Kanal","avatar":{"thumbnails":)"
        R"([{"url":"//yt3.ggpht.com/meta.jpg","width":176}]}}})";
    ASSERT_TRUE(yt::parse_channel_page(with_tie, &tie)) << tie.error;
    EXPECT_EQ(tie.channel.thumbnail_url, "https://yt3.ggpht.com/deco.jpg");
}

} // namespace

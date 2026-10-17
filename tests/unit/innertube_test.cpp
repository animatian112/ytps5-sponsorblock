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
#include <vector>

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

    // The stream selection then picks a codec the settings allow. This answer
    // is 1080p throughout, where only AVC is inside its software-decode ceiling
    // (VP9 stops at 720p, AV1 at 480p), so the AVC entry wins.
    yt::CodecChoice choice;
    const yt::Format *picked = yt::select_video_format(info.formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, yt::Codec::avc);
    EXPECT_EQ(picked->itag, 137);
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

TEST(InnerTubePlayer, ReadsThePrerollWaitFromAdPlacementsAndSlots)
{
    // Two start placements (one skippable after 5 s, one 15 s long), a mid-roll
    // that does not count, and a before-content slot with a sequence of 6 s and
    // 20 s ads: googlevideo refuses the URLs for 5 + 15 + 6 + 20 = 46 s.
    const std::string body =
        R"({"playabilityStatus":{"status":"OK"},)"
        R"("videoDetails":{"videoId":"ADS-AHEAD-01","title":"t","lengthSeconds":"60"},)"
        R"("streamingData":{"adaptiveFormats":[]},)"
        R"("adPlacements":[)"
        R"({"adPlacementRenderer":{"config":{"adPlacementConfig":{"kind":"AD_PLACEMENT_KIND_START"}},)"
        R"("renderer":{"instreamVideoAdRenderer":{"skipOffsetMilliseconds":5000,"playerVars":"length_seconds=30"}}}},)"
        R"({"adPlacementRenderer":{"config":{"adPlacementConfig":{"kind":"AD_PLACEMENT_KIND_START"}},)"
        R"("renderer":{"instreamVideoAdRenderer":{"playerVars":"video_id=x&length_seconds=15&a=b"}}}},)"
        R"({"adPlacementRenderer":{"config":{"adPlacementConfig":{"kind":"AD_PLACEMENT_KIND_MILLISECONDS"}},)"
        R"("renderer":{"instreamVideoAdRenderer":{"playerVars":"length_seconds=99"}}}}],)"
        R"("adSlots":[{"adSlotRenderer":{"adSlotMetadata":{"triggerEvent":"SLOT_TRIGGER_EVENT_BEFORE_CONTENT"},)"
        R"("fulfillmentContent":{"fulfilledLayout":{"playerBytesAdLayoutRenderer":{"renderingContent":)"
        R"({"playerBytesSequentialLayoutRenderer":{"sequentialLayouts":[)"
        R"({"playerBytesAdLayoutRenderer":{"renderingContent":{"instreamVideoAdRenderer":{"playerVars":"length_seconds=6"}}}},)"
        R"({"playerBytesAdLayoutRenderer":{"renderingContent":{"instreamVideoAdRenderer":{"playerVars":"length_seconds=20"}}}})"
        R"(]}}}}}}}]})";
    yt::VideoInfo info;
    ASSERT_TRUE(yt::parse_video_info(body, &info)) << info.error;
    EXPECT_EQ(info.preroll_ads, 4);
    EXPECT_DOUBLE_EQ(info.preroll_wait_s, 46.0);

    yt::VideoInfo plain;
    ASSERT_TRUE(yt::parse_video_info(fixture("player_ok.json"), &plain)) << plain.error;
    EXPECT_EQ(plain.preroll_ads, 0);
    EXPECT_DOUBLE_EQ(plain.preroll_wait_s, 0.0);
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

// The signed-in TV client (TVHTML5) answers browse with tileRenderers inside
// shelves (layout as in SmartTube's TileItem); channel tiles carry no video.
TEST(InnerTube, ReadsTheTvClientsTiles)
{
    const char *json = R"({"contents":{"tvBrowseRenderer":{"content":{"tvSurfaceContentRenderer":
      {"content":{"sectionListRenderer":{"contents":[{"shelfRenderer":{"content":
      {"horizontalListRenderer":{"items":[
        {"tileRenderer":{"style":"TILE_STYLE_YTLR_DEFAULT","contentType":"TILE_CONTENT_TYPE_VIDEO",
          "header":{"tileHeaderRenderer":{"thumbnail":{"thumbnails":[
              {"url":"https://i.ytimg.com/vi/aaaaaaaaaaa/default.jpg","width":120,"height":90},
              {"url":"https://i.ytimg.com/vi/aaaaaaaaaaa/hq720.jpg","width":1280,"height":720}]},
            "thumbnailOverlays":[{"thumbnailOverlayTimeStatusRenderer":
              {"text":{"simpleText":"12:34"},"style":"DEFAULT"}}]}},
          "metadata":{"tileMetadataRenderer":{"title":{"simpleText":"A tile video"},
            "lines":[{"lineRenderer":{"items":[{"lineItemRenderer":{"text":{"runs":[{"text":"Some Channel"}]}}},
                                               {"lineItemRenderer":{"text":{"simpleText":"1M views"}}}]}}]}},
          "onSelectCommand":{"watchEndpoint":{"videoId":"aaaaaaaaaaa"}}}},
        {"tileRenderer":{"contentType":"TILE_CONTENT_TYPE_VIDEO",
          "header":{"tileHeaderRenderer":{"thumbnailOverlays":[{"thumbnailOverlayTimeStatusRenderer":
              {"text":{"simpleText":"LIVE"},"style":"LIVE"}}]}},
          "metadata":{"tileMetadataRenderer":{"title":{"runs":[{"text":"Live now"}]}}},
          "onSelectCommand":{"watchEndpoint":{"videoId":"bbbbbbbbbbb"}}}},
        {"tileRenderer":{"contentType":"TILE_CONTENT_TYPE_CHANNEL",
          "metadata":{"tileMetadataRenderer":{"title":{"simpleText":"A channel"}}},
          "onSelectCommand":{"browseEndpoint":{"browseId":"UCxxxx"}}}}
      ]}}}}]}}}}}}})";
    yt::FeedPage page;
    ASSERT_TRUE(yt::parse_feed_page(json, &page));
    ASSERT_EQ(page.items.size(), 2u);
    EXPECT_EQ(page.items[0].video_id, "aaaaaaaaaaa");
    EXPECT_EQ(page.items[0].title, "A tile video");
    EXPECT_EQ(page.items[0].author, "Some Channel");
    EXPECT_EQ(page.items[0].thumbnail_url, "https://i.ytimg.com/vi/aaaaaaaaaaa/hq720.jpg");
    EXPECT_DOUBLE_EQ(page.items[0].length_s, 754.0);
    EXPECT_FALSE(page.items[0].live);
    EXPECT_EQ(page.items[1].video_id, "bbbbbbbbbbb");
    EXPECT_TRUE(page.items[1].live);
    EXPECT_EQ(page.items[0].detail, "1M views");
}

TEST(InnerTube, ShelvesNameTheirItemsAndTheWatchedShareIsRead)
{
    const char *json = R"({"contents":{"sectionListRenderer":{"contents":[
      {"shelfRenderer":{"headerRenderer":{"shelfHeaderRenderer":{"title":{"simpleText":"Trending"}}},
        "content":{"horizontalListRenderer":{"items":[
        {"tileRenderer":{"header":{"tileHeaderRenderer":{"thumbnailOverlays":[
            {"thumbnailOverlayResumePlaybackRenderer":{"percentDurationWatched":40}}]}},
          "metadata":{"tileMetadataRenderer":{"title":{"simpleText":"One"},
            "lines":[{"lineRenderer":{"items":[{"lineItemRenderer":{"text":{"simpleText":"Chan"}}}]}},
                     {"lineRenderer":{"items":[{"lineItemRenderer":{"text":{"simpleText":"5K views"}}},
                                               {"lineItemRenderer":{"text":{"simpleText":"\u2022"}}},
                                               {"lineItemRenderer":{"text":{"simpleText":"2 days ago"}}}]}}]}},
          "onSelectCommand":{"watchEndpoint":{"videoId":"ccccccccccc"}}}}]}}}},
      {"shelfRenderer":{"title":{"runs":[{"text":"Music"}]},
        "content":{"horizontalListRenderer":{"items":[
        {"tileRenderer":{"metadata":{"tileMetadataRenderer":{"title":{"simpleText":"Two"}}},
          "onSelectCommand":{"watchEndpoint":{"videoId":"ddddddddddd"}}}}]}}}}]},
      "subscribeButtonRenderer":{"subscribed":true,"channelId":"UCabc"}}})";
    yt::FeedPage page;
    ASSERT_TRUE(yt::parse_feed_page(json, &page));
    ASSERT_EQ(page.items.size(), 2u);
    EXPECT_EQ(page.items[0].shelf, "Trending");
    EXPECT_EQ(page.items[1].shelf, "Music");
    EXPECT_EQ(page.items[0].detail, "5K views \xE2\x80\xA2 2 days ago");
    EXPECT_FLOAT_EQ(page.items[0].watched, 0.4f);
    EXPECT_FLOAT_EQ(page.items[1].watched, -1.0f);
    EXPECT_TRUE(page.meta_known);
    EXPECT_TRUE(page.subscribed);
    EXPECT_EQ(page.channel_id, "UCabc");
}

TEST(InnerTube, ReadsTheCardMenuOfATile)
{
    // The tile's long-press menu: "Go to channel", "Not interested" and
    // "Don't recommend channel", each a labelled entry around its endpoint.
    const char *json = R"({"contents":[{"tileRenderer":{"contentType":"TILE_CONTENT_TYPE_VIDEO",
      "metadata":{"tileMetadataRenderer":{"title":{"simpleText":"Menu video"}}},
      "onSelectCommand":{"watchEndpoint":{"videoId":"ccccccccccc"}},
      "onLongPressCommand":{"showMenuCommand":{"menu":{"menuRenderer":{"items":[
        {"menuNavigationItemRenderer":{"text":{"runs":[{"text":"Go to channel"}]},
          "navigationEndpoint":{"browseEndpoint":{"browseId":"UCabcdefghijklmnopqrstuv"}}}},
        {"menuServiceItemRenderer":{"text":{"runs":[{"text":"Not interested"}]},
          "serviceEndpoint":{"feedbackEndpoint":{"feedbackToken":"tok-ni"}}}},
        {"menuServiceItemRenderer":{"text":{"simpleText":"Don't recommend channel"},
          "serviceEndpoint":{"feedbackEndpoint":{"feedbackToken":"tok-dr"}}}}
      ]}}}}}}]})";
    yt::FeedPage page;
    ASSERT_TRUE(yt::parse_feed_page(json, &page));
    ASSERT_EQ(page.items.size(), 1u);
    EXPECT_EQ(page.items[0].channel_id, "UCabcdefghijklmnopqrstuv");
    EXPECT_EQ(page.items[0].not_interested_token, "tok-ni");
    EXPECT_EQ(page.items[0].dont_recommend_token, "tok-dr");
}

TEST(InnerTube, ReadsTheTvSectionContinuationAndChannelTiles)
{
    // The TV Home pages with nextContinuationData on its section list (a
    // shelf's own token is not the next page), and "Top channels you watch"
    // tiles open channels.
    const char *json =
        R"({"contents":{"tvBrowseRenderer":{"content":{"tvSurfaceContentRenderer":{"content":{"sectionListRenderer":{"contents":[{"shelfRenderer":{"headerRenderer":{"shelfHeaderRenderer":{"title":{"simpleText":"Top channels you watch"}}},"content":{"horizontalListRenderer":{"items":[{"tileRenderer":{"contentType":"TILE_CONTENT_TYPE_CHANNEL","metadata":{"tileMetadataRenderer":{"title":{"simpleText":"Scott The Woz"}}},"onSelectCommand":{"browseEndpoint":{"browseId":"UC4rqhyiTs7XyuODcECvuiiQ"}}}}],"continuations":[{"nextContinuationData":{"continuation":"shelf-token"}}]}}}}],"continuations":[{"nextContinuationData":{"continuation":"page-token"}}]}}}}}}})";
    yt::FeedPage page;
    ASSERT_TRUE(yt::parse_feed_page(json, &page));
    ASSERT_EQ(page.items.size(), 1u);
    EXPECT_TRUE(page.items[0].is_channel());
    EXPECT_EQ(page.items[0].shelf, "Top channels you watch");
    EXPECT_EQ(page.continuation, "page-token");
}

TEST(InnerTube, ReadsAReelItemAndItsSequenceToken)
{
    // reel_item_watch: the Short in replacementEndpoint, its title from the
    // description panel, the channel from its navigation endpoint, and the
    // token of the sequence that follows.
    const char *json = R"({"replacementEndpoint":{"reelWatchEndpoint":{"videoId":"sssssssssss",
      "thumbnail":{"thumbnails":[{"url":"https://i.ytimg.com/vi/sssssssssss/frame0.jpg","width":1080,"height":1920},
                                 {"url":"https://i.ytimg.com/vi/sssssssssss/oar2.jpg","width":405,"height":720}]}}},
      "sequenceContinuation":"seq-token",
      "engagementPanels":[{"engagementPanelSectionListRenderer":{"content":{"structuredDescriptionContentRenderer":{"items":[
        {"videoDescriptionHeaderRenderer":{"title":{"runs":[{"text":"A short title"}]},
          "channel":{"simpleText":"Short Maker"},"views":{"simpleText":"12K views"},
          "publishDate":{"simpleText":"2 days ago"},
          "channelNavigationEndpoint":{"browseEndpoint":{"browseId":"UCabcdefghijklmnopqrstuv"}}}}]}}}}]})";
    yt::FeedItem item;
    std::string sequence;
    ASSERT_TRUE(yt::parse_reel_item(json, &item, &sequence));
    EXPECT_EQ(item.video_id, "sssssssssss");
    EXPECT_EQ(item.title, "A short title");
    EXPECT_EQ(item.author, "Short Maker");
    EXPECT_EQ(item.channel_id, "UCabcdefghijklmnopqrstuv");
    EXPECT_EQ(item.thumbnail_url, "https://i.ytimg.com/vi/sssssssssss/oar2.jpg");
    EXPECT_EQ(sequence, "seq-token");
}

TEST(InnerTube, ReadsAReelSequence)
{
    const char *json = R"({"entries":[
      {"command":{"reelWatchEndpoint":{"videoId":"aaaaaaaaaaa","params":"p-a"}}},
      {"command":{"reelWatchEndpoint":{"videoId":"bad"}}},
      {"command":{"reelWatchEndpoint":{"videoId":"bbbbbbbbbbb","params":"p-b"}}}],
      "continuationEndpoint":{"continuationCommand":{"token":"next-token"}}})";
    std::vector<yt::ReelEntry> entries;
    std::string next;
    ASSERT_TRUE(yt::parse_reel_sequence(json, &entries, &next));
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].video_id, "aaaaaaaaaaa");
    EXPECT_EQ(entries[1].params, "p-b");
    EXPECT_EQ(next, "next-token");
}

TEST(InnerTube, AShowsSeasonsAreALabelNotSeconds)
{
    // A show's tile: its overlay says "8" + " seasons" in two runs.
    const char *json = R"({"contents":[{"tileRenderer":{"contentType":"TILE_CONTENT_TYPE_PLAYLIST",
      "metadata":{"tileMetadataRenderer":{"title":{"simpleText":"A show"}}},
      "header":{"tileHeaderRenderer":{"thumbnailOverlays":[{"thumbnailOverlayTimeStatusRenderer":
        {"text":{"runs":[{"text":"8"},{"text":" seasons"}]},"style":"DEFAULT"}}]}},
      "onSelectCommand":{"watchEndpoint":{"videoId":"sssssssssss"}}}}]})";
    yt::FeedPage page;
    ASSERT_TRUE(yt::parse_feed_page(json, &page));
    ASSERT_EQ(page.items.size(), 1u);
    EXPECT_DOUBLE_EQ(page.items[0].length_s, 0.0);
    EXPECT_EQ(page.items[0].badge, "8 seasons");
}

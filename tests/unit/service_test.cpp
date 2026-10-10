// youtube-tv - The background service's frame-side bookkeeping: the video
// watchdog budget, request resets, the offline injection helpers and the
// login cancel path. The worker is never started, so nothing here opens a
// socket or spawns a thread.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/service.hpp"
#include "yt/types.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace
{

using yt::ChannelItem;
using yt::Codec;
using yt::CodecChoice;
using yt::FeedItem;
using yt::FeedKind;
using yt::FeedView;
using yt::Format;
using yt::Load;
using yt::LoginView;
using yt::Service;
using yt::VideoView;

std::string token_path()
{
    return ::testing::TempDir() + "yt_service_test_token.json";
}

TEST(Service, VideoRequestStartsLoadingAndTheWatchdogTimesOut)
{
    const std::string path = token_path();
    Service service(path, "", nullptr);

    const std::uint64_t before = service.video_revision();
    service.request_video("aqz-KE-bpKQ", CodecChoice{});

    const VideoView loading = service.video();
    EXPECT_EQ(loading.state, Load::loading);
    EXPECT_FALSE(loading.stage.empty());
    EXPECT_GT(service.video_revision(), before);

    service.video_watchdog(5.0f);
    const VideoView waiting = service.video();
    EXPECT_EQ(waiting.state, Load::loading);
    EXPECT_FLOAT_EQ(waiting.waiting_s, 5.0f);

    service.video_watchdog(16.0f);
    const VideoView timed_out = service.video();
    EXPECT_EQ(timed_out.state, Load::failed);
    EXPECT_EQ(timed_out.error, "timeout");

    const std::uint64_t after_timeout = service.video_revision();
    service.video_watchdog(30.0f);
    EXPECT_EQ(service.video_revision(), after_timeout);

    std::remove(path.c_str());
}

TEST(Service, ANewVideoRequestResetsTheWatchdogAfterATimeout)
{
    const std::string path = token_path();
    Service service(path, "", nullptr);

    service.request_video("aqz-KE-bpKQ", CodecChoice{});
    service.video_watchdog(21.0f);
    ASSERT_EQ(service.video().state, Load::failed);

    const std::uint64_t before = service.video_revision();
    service.request_video("other", CodecChoice{});

    const VideoView fresh = service.video();
    EXPECT_EQ(fresh.state, Load::loading);
    EXPECT_FLOAT_EQ(fresh.waiting_s, 0.0f);
    EXPECT_TRUE(fresh.error.empty());
    EXPECT_GT(service.video_revision(), before);

    std::remove(path.c_str());
}

TEST(Service, InjectedChannelsAndItemsRoundTripThroughTheSearchFeed)
{
    const std::string path = token_path();
    Service service(path, "", nullptr);

    const std::uint64_t before_channels = service.revision(FeedKind::search);
    ChannelItem channel;
    channel.channel_id = "UC123";
    channel.name = "Sample Channel";
    channel.thumbnail_url = "https://example.invalid/avatar.jpg";
    channel.subscribers = "1.2M subscribers";
    service.inject_channels(FeedKind::search, {channel});

    const FeedView with_channels = service.feed(FeedKind::search);
    ASSERT_EQ(with_channels.channels.size(), 1u);
    EXPECT_EQ(with_channels.channels[0].name, "Sample Channel");
    EXPECT_GT(service.revision(FeedKind::search), before_channels);

    FeedItem item;
    item.video_id = "aqz-KE-bpKQ";
    item.title = "Big Buck Bunny";
    item.author = "Blender";
    service.inject_feed(FeedKind::search, {item}, "next-token");

    const FeedView feed = service.feed(FeedKind::search);
    ASSERT_EQ(feed.items.size(), 1u);
    EXPECT_EQ(feed.items[0].video_id, "aqz-KE-bpKQ");
    EXPECT_EQ(feed.continuation, "next-token");

    const FeedView search = service.search_feed();
    ASSERT_EQ(search.items.size(), 1u);
    EXPECT_EQ(search.items[0].video_id, "aqz-KE-bpKQ");
    EXPECT_EQ(search.continuation, "next-token");

    std::remove(path.c_str());
}

TEST(Service, CancelSignInWithNothingInFlightAnswersImmediately)
{
    const std::string path = token_path();
    Service service(path, "", nullptr);

    const std::uint64_t before = service.login_revision();
    service.inject_login(
        LoginView{Load::loading, "ABCD-EFGH", "https://www.google.com/device", {}, 0});
    ASSERT_EQ(service.login().state, Load::loading);

    service.cancel_sign_in();
    const LoginView cancelled = service.login();
    EXPECT_EQ(cancelled.state, Load::idle);
    EXPECT_GT(service.login_revision(), before);

    const std::uint64_t after_cancel = service.login_revision();
    service.cancel_sign_in();
    EXPECT_EQ(service.login_revision(), after_cancel);

    service.request_sign_in();
    service.cancel_sign_in();
    const LoginView idle = service.login();
    EXPECT_EQ(idle.state, Load::idle);
    EXPECT_TRUE(idle.error.empty());

    std::remove(path.c_str());
}

TEST(Service, TheFrameViewDropsFormatsButKeepsTheTitle)
{
    const std::string path = token_path();
    Service service(path, "", nullptr);

    Format format;
    format.itag = 137;
    format.url = "https://example.invalid/stream";
    format.codec = Codec::avc;
    format.width = 1920;
    format.height = 1080;

    VideoView view;
    view.state = Load::ready;
    view.info.video_id = "aqz-KE-bpKQ";
    view.info.title = "Big Buck Bunny";
    view.info.formats = {format};

    service.inject_video(std::move(view), nullptr);

    const VideoView frame = service.video();
    EXPECT_EQ(frame.state, Load::ready);
    EXPECT_EQ(frame.info.title, "Big Buck Bunny");
    EXPECT_TRUE(frame.info.formats.empty());

    std::remove(path.c_str());
}

} // namespace

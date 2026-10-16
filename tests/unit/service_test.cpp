// youtube-tv - The background service: the frame-side bookkeeping (the video
// watchdog budget, request resets, the offline injection helpers and the
// login cancel path) and the token restore and rejection paths. Nothing here
// opens a socket: every request the worker makes is answered by the canned
// transport in http_stub.hpp.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "http_stub.hpp"
#include "yt/service.hpp"
#include "yt/types.hpp"

#include "core/save_file.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace
{

using yt::ChannelItem;
using yt::ChannelView;
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

// Waits (bounded) for the worker to answer; the transport is the canned
// stub, so this is only the thread hop.
bool settle(const std::function<bool()> &done, double seconds = 10.0)
{
    const int steps = static_cast<int>(seconds * 100.0);
    for (int step = 0; step < steps; ++step)
    {
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

std::string fixture(const char *name)
{
    std::string data;
    const std::string path = std::string(HUI_SOURCE_DIR) + "/tests/fixtures/" + name;
    EXPECT_TRUE(hui::save::read_file(path, &data)) << path;
    return data;
}

// The canned answers of a working account: the token exchange succeeds and
// every browse answers with one feed page. Everything else (the identity
// probe) is left unanswered so the auth falls back to its known pair.
void answer_working_account(const std::string &feed_body)
{
    hui::testing::HttpStub::instance().set_handler(
        [feed_body](const yt::http::Request &request) -> yt::http::Response
        {
            yt::http::Response response;
            if (request.url.find("/token") != std::string::npos)
            {
                response.status = 200;
                response.body = R"({"access_token":"access-77","token_type":"Bearer"})";
            }
            else if (request.url.find("/browse") != std::string::npos)
            {
                response.status = 200;
                response.body = feed_body;
            }
            return response;
        });
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

TEST(Service, TheFrameViewKeepsFormatsWithoutTheirUrls)
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
    // The quality menu needs the ladder, not the long stream URLs.
    ASSERT_EQ(frame.info.formats.size(), 1u);
    EXPECT_EQ(frame.info.formats[0].height, 1080);
    EXPECT_EQ(frame.info.formats[0].url, "-");

    std::remove(path.c_str());
}

TEST(Service, ARestoredTokenSignsInAndRequestsTheFeedsWithIt)
{
    const std::string root = ::testing::TempDir() + "yt_service_restore";
    ASSERT_TRUE(hui::save::ensure_directory(root));
    const std::string file = root + "/yt-token.json";
    ASSERT_EQ(hui::save::write_atomic(file, R"({"v":1,"refresh_token":"restored-refresh"})"), "");
    const std::string sandbox = root + "/auth.json";
    std::remove(sandbox.c_str());

    answer_working_account(fixture("feed_home.json"));

    Service service(sandbox, root, nullptr);
    service.start();
    // Only the restore fires: the feeds must be requested by it, with the
    // token the backup carried.
    service.import_token_file();

    ASSERT_TRUE(settle(
        [&]
        {
            return service.feed(FeedKind::home).state == Load::ready &&
                   service.feed(FeedKind::subscriptions).state == Load::ready;
        }))
        << "the restore never loaded Home and Subscriptions";
    EXPECT_TRUE(service.signed_in());
    EXPECT_FALSE(service.feed(FeedKind::home).items.empty());
    EXPECT_FALSE(service.feed(FeedKind::subscriptions).items.empty());

    bool with_token = false;
    for (const hui::testing::HttpExchange &exchange :
         hui::testing::HttpStub::instance().exchanges())
    {
        if (exchange.request.url.find("/browse") == std::string::npos)
            continue;
        for (const yt::http::Header &header : exchange.request.headers)
        {
            if (header.name == "Authorization" && header.value == "Bearer access-77")
                with_token = true;
        }
    }
    EXPECT_TRUE(with_token) << "the feed request must carry the restored token";

    // The restore message runs off this, and only once per restore.
    EXPECT_TRUE(service.take_token_restored());
    EXPECT_FALSE(service.take_token_restored());

    hui::testing::HttpStub::instance().clear();
    std::remove(file.c_str());
    std::remove(sandbox.c_str());
}

TEST(Service, ARejectedTokenEndsSignedOutWithAnHonestState)
{
    const std::string root = ::testing::TempDir() + "yt_service_reject";
    ASSERT_TRUE(hui::save::ensure_directory(root));
    const std::string file = root + "/yt-token.json";
    ASSERT_EQ(hui::save::write_atomic(file, R"({"v":1,"refresh_token":"dead-refresh"})"), "");
    const std::string sandbox = root + "/auth.json";
    std::remove(sandbox.c_str());

    hui::testing::HttpStub::instance().set_handler(
        [](const yt::http::Request &request) -> yt::http::Response
        {
            yt::http::Response response;
            if (request.url.find("/token") != std::string::npos)
            {
                response.status = 400;
                response.body = R"({"error":"invalid_grant",)"
                                R"("error_description":"Token has been expired or revoked."})";
            }
            return response;
        });

    Service service(sandbox, root, nullptr);
    service.start();
    service.import_token_file();

    ASSERT_TRUE(settle([&] { return !service.last_error().empty(); }))
        << "the rejected token was never reported";

    // The signed-out state and the honest reason: the screens key their
    // "Sign in to see ..." text off signed_in(), and no screen may claim
    // "no subscriptions" for a missing account.
    EXPECT_FALSE(service.signed_in());
    EXPECT_NE(service.last_error().find("expired or revoked"), std::string::npos)
        << service.last_error();
    EXPECT_FALSE(service.take_token_restored()) << "a rejected token is not a restore";

    // The dead token is never used: no feed request went out at all.
    for (const hui::testing::HttpExchange &exchange :
         hui::testing::HttpStub::instance().exchanges())
        EXPECT_EQ(exchange.request.url.find("/browse"), std::string::npos)
            << "the feeds must not be requested with a rejected token";

    hui::testing::HttpStub::instance().clear();
    std::remove(file.c_str());
    std::remove(sandbox.c_str());
}

TEST(Service, ATransportFailedRefreshKeepsTheSession)
{
    const std::string path = token_path();
    std::remove(path.c_str());
    ASSERT_EQ(hui::save::write_atomic(path, R"({"v":1,"refresh_token":"living-refresh"})"), "");

    answer_working_account(fixture("feed_home.json"));
    Service service(path, "", nullptr);
    service.start();
    service.refresh_token(); // the startup exchange
    ASSERT_TRUE(settle([&] { return service.signed_in(); })) << "the startup exchange failed";

    // The access token is stale and the refresh cannot reach the network at
    // all.
    auto browse_calls = std::make_shared<std::atomic<int>>(0);
    hui::testing::HttpStub::instance().set_handler(
        [browse_calls](const yt::http::Request &request) -> yt::http::Response
        {
            yt::http::Response response;
            if (request.url.find("/token") != std::string::npos)
            {
                response.error = "connection reset by peer";
                return response;
            }
            if (request.url.find("/browse") != std::string::npos)
            {
                (*browse_calls)++;
                response.status = 401;
                response.body = R"({"error":{"code":401,"message":"Unauthorized"}})";
            }
            return response;
        });

    service.request_home(true);
    ASSERT_TRUE(settle([&] { return service.feed(FeedKind::home).state == Load::failed; }))
        << "the refused request never published its failure";

    // The session stands and the error names the network problem: no retry
    // ever ran, so the 401 says nothing about the token and the player may
    // not be pushed into the sign-in state over a network hiccup.
    EXPECT_TRUE(service.signed_in());
    EXPECT_NE(service.last_error().find("connection reset"), std::string::npos)
        << service.last_error();
    EXPECT_EQ(browse_calls->load(), 1) << "no retry ran, so no sign-out may follow";
    EXPECT_FALSE(service.feed(FeedKind::home).error.empty());

    hui::testing::HttpStub::instance().clear();
    std::remove(path.c_str());
}

TEST(Service, AFeedThatFailsAfterAGrantRejectionKeepsTheNamedCause)
{
    const std::string path = token_path();
    std::remove(path.c_str());
    ASSERT_EQ(hui::save::write_atomic(path, R"({"v":1,"refresh_token":"dead-refresh"})"), "");

    answer_working_account(fixture("feed_home.json"));
    Service service(path, "", nullptr);
    service.start();
    service.refresh_token(); // the startup exchange
    ASSERT_TRUE(settle([&] { return service.signed_in(); })) << "the startup exchange failed";

    // The grant dies while the feed is being loaded: refresh_token_locked()
    // signs out and names the cause, and the failed feed may not sign out
    // again over it or overwrite the reason with its own raw HTTP refusal.
    hui::testing::HttpStub::instance().set_handler(
        [](const yt::http::Request &request) -> yt::http::Response
        {
            yt::http::Response response;
            if (request.url.find("/token") != std::string::npos)
            {
                response.status = 400;
                response.body = R"({"error":"invalid_grant",)"
                                R"("error_description":"Token has been expired or revoked."})";
                return response;
            }
            if (request.url.find("/browse") != std::string::npos)
            {
                response.status = 401;
                response.body = R"({"error":{"code":401,"message":"Unauthorized"}})";
            }
            return response;
        });

    service.request_home(true);
    ASSERT_TRUE(settle([&] { return service.feed(FeedKind::home).state == Load::failed; }))
        << "the refused request never published its failure";

    EXPECT_FALSE(service.signed_in()) << "the rejected grant ends the session";
    EXPECT_NE(service.last_error().find("expired or revoked"), std::string::npos)
        << service.last_error();
    EXPECT_EQ(service.last_error().find("401"), std::string::npos)
        << "the grant's own reason, not the raw feed refusal: " << service.last_error();

    hui::testing::HttpStub::instance().clear();
    std::remove(path.c_str());
}

TEST(Service, AChannelPageRecoversAfterAStaleTokenRefresh)
{
    const std::string path = token_path();
    std::remove(path.c_str());
    ASSERT_EQ(hui::save::write_atomic(path, R"({"v":1,"refresh_token":"stale-refresh"})"), "");
    Service service(path, "", nullptr);

    // The first /token is the startup exchange, the second the refresh that
    // recovers; the first /browse is the stale access token being refused.
    const std::string channel_body = fixture("channel_page.json");
    auto token_calls = std::make_shared<std::atomic<int>>(0);
    auto browse_calls = std::make_shared<std::atomic<int>>(0);
    hui::testing::HttpStub::instance().set_handler(
        [channel_body, token_calls,
         browse_calls](const yt::http::Request &request) -> yt::http::Response
        {
            yt::http::Response response;
            if (request.url.find("/token") != std::string::npos)
            {
                response.status = 200;
                response.body = (*token_calls)++ == 0
                                    ? R"({"access_token":"stale-77","token_type":"Bearer"})"
                                    : R"({"access_token":"fresh-88","token_type":"Bearer"})";
                return response;
            }
            if (request.url.find("/browse") != std::string::npos)
            {
                if ((*browse_calls)++ == 0)
                {
                    response.status = 401;
                    response.body = R"({"error":{"code":401,"message":"Unauthorized"}})";
                }
                else
                {
                    response.status = 200;
                    response.body = channel_body;
                }
            }
            return response;
        });

    service.start();
    service.refresh_token(); // the startup exchange
    ASSERT_TRUE(settle([&] { return service.signed_in(); })) << "the startup exchange failed";
    service.request_channel("UCaaaaaaaaaaaaaaaaaaaaaa");

    ASSERT_TRUE(settle(
        [&]
        {
            return service.channel_view().state == Load::ready &&
                   service.feed(FeedKind::channel).state == Load::ready;
        }))
        << "the channel never recovered: " << service.channel_view().error;

    const ChannelView view = service.channel_view();
    EXPECT_TRUE(view.error.empty());
    EXPECT_EQ(view.channel.name, "Alpha Kanal");
    ASSERT_EQ(view.tabs.size(), 3u);
    const FeedView videos = service.feed(FeedKind::channel);
    EXPECT_EQ(videos.state, Load::ready);
    EXPECT_EQ(videos.items.size(), 2u) << "the page's own videos, not the raw 401";
    EXPECT_TRUE(videos.error.empty());

    // Exactly one refresh recovered it: the retry carried the fresh token.
    EXPECT_EQ(token_calls->load(), 2) << "one refresh, not one per request";
    EXPECT_EQ(browse_calls->load(), 2) << "the refused request and its retry";
    bool retried_with_fresh = false;
    for (const hui::testing::HttpExchange &exchange :
         hui::testing::HttpStub::instance().exchanges())
    {
        if (exchange.request.url.find("/browse") == std::string::npos)
            continue;
        for (const yt::http::Header &header : exchange.request.headers)
        {
            if (header.name == "Authorization" && header.value == "Bearer fresh-88")
                retried_with_fresh = true;
        }
    }
    EXPECT_TRUE(retried_with_fresh) << "the retry must carry the refreshed token";

    hui::testing::HttpStub::instance().clear();
    std::remove(path.c_str());
}

TEST(Service, AChannelTabRecoversAfterAStaleTokenRefresh)
{
    const std::string path = token_path();
    std::remove(path.c_str());
    ASSERT_EQ(hui::save::write_atomic(path, R"({"v":1,"refresh_token":"stale-refresh"})"), "");
    Service service(path, "", nullptr);

    // Phase one: an open channel page, so a tab has a channel to switch to.
    const std::string channel_body = fixture("channel_page.json");
    hui::testing::HttpStub::instance().set_handler(
        [channel_body](const yt::http::Request &request) -> yt::http::Response
        {
            yt::http::Response response;
            if (request.url.find("/token") != std::string::npos)
            {
                response.status = 200;
                response.body = R"({"access_token":"stale-77","token_type":"Bearer"})";
                return response;
            }
            if (request.url.find("/browse") != std::string::npos)
            {
                response.status = 200;
                response.body = channel_body;
            }
            return response;
        });

    service.start();
    service.refresh_token(); // the startup exchange
    ASSERT_TRUE(settle([&] { return service.signed_in(); })) << "the startup exchange failed";
    service.request_channel("UCaaaaaaaaaaaaaaaaaaaaaa");
    ASSERT_TRUE(settle([&] { return service.channel_view().state == Load::ready; }))
        << "the channel page never loaded";

    // Phase two: the tab request runs into the stale token.
    const std::string home_body = fixture("feed_home.json");
    auto token_calls = std::make_shared<std::atomic<int>>(0);
    auto browse_calls = std::make_shared<std::atomic<int>>(0);
    hui::testing::HttpStub::instance().set_handler(
        [home_body, token_calls,
         browse_calls](const yt::http::Request &request) -> yt::http::Response
        {
            yt::http::Response response;
            if (request.url.find("/token") != std::string::npos)
            {
                (*token_calls)++;
                response.status = 200;
                response.body = R"({"access_token":"fresh-88","token_type":"Bearer"})";
                return response;
            }
            if (request.url.find("/browse") != std::string::npos)
            {
                EXPECT_NE(request.body.find("\"params\""), std::string::npos) << request.body;
                if ((*browse_calls)++ == 0)
                {
                    response.status = 401;
                    response.body = R"({"error":{"code":401,"message":"Unauthorized"}})";
                }
                else
                {
                    response.status = 200;
                    response.body = home_body;
                }
            }
            return response;
        });

    // The revision guard keeps the settle off the tab publish: the items
    // read afterwards are the tab's answer, not the open page's leftovers.
    const std::uint64_t before_tab = service.feed(FeedKind::channel).revision;
    service.request_channel_tab(1);

    ASSERT_TRUE(settle(
        [&]
        {
            return service.channel_view().selected_tab == 1 &&
                   service.feed(FeedKind::channel).revision > before_tab;
        }))
        << "the tab never recovered: " << service.feed(FeedKind::channel).error;

    EXPECT_EQ(service.channel_view().selected_tab, 1);
    const FeedView videos = service.feed(FeedKind::channel);
    EXPECT_FALSE(videos.items.empty()) << "the tab's own videos, not the raw 401";
    for (const FeedItem &item : videos.items)
    {
        EXPECT_EQ(item.video_id.find("CHAN-VID"), std::string::npos)
            << "the page's items leaked into the tab: " << item.video_id;
    }
    EXPECT_TRUE(videos.error.empty());
    EXPECT_EQ(token_calls->load(), 1) << "one refresh recovered the tab";
    EXPECT_EQ(browse_calls->load(), 2) << "the refused tab request and its retry";

    hui::testing::HttpStub::instance().clear();
    std::remove(path.c_str());
}

TEST(Service, TheRestoreFlagIsVisibleBeforeTheSignInFlipAndSurvivesIt)
{
    const std::string root = ::testing::TempDir() + "yt_service_restore_flag";
    ASSERT_TRUE(hui::save::ensure_directory(root));
    const std::string file = root + "/yt-token.json";
    ASSERT_EQ(hui::save::write_atomic(file, R"({"v":1,"refresh_token":"restored-refresh"})"), "");
    const std::string sandbox = root + "/auth.json";
    std::remove(sandbox.c_str());

    Service service(sandbox, root, nullptr);

    // What a frame would see at the worst possible moment, read while the
    // token exchange is still in flight: strictly before signed_in() flips.
    std::atomic<bool> signed_in_during_exchange{true};
    std::atomic<bool> flag_before_flip{false};
    const std::string home_body = fixture("feed_home.json");
    auto token_calls = std::make_shared<std::atomic<int>>(0);
    hui::testing::HttpStub::instance().set_handler(
        [&](const yt::http::Request &request) -> yt::http::Response
        {
            yt::http::Response response;
            if (request.url.find("/token") != std::string::npos)
            {
                if ((*token_calls)++ == 0)
                {
                    // The restore's exchange is running: signed_in() has not
                    // flipped yet, but the one-shot flag must already be
                    // published - the frame reads it the moment the flip is
                    // visible, and a flag that arrives after it is a lost
                    // "Account stored" toast.
                    signed_in_during_exchange = service.signed_in();
                    flag_before_flip = service.take_token_restored();
                }
                response.status = 200;
                response.body = R"({"access_token":"access-77","token_type":"Bearer"})";
                return response;
            }
            if (request.url.find("/browse") != std::string::npos)
            {
                response.status = 200;
                response.body = home_body;
            }
            return response;
        });

    service.start();
    service.import_token_file();
    ASSERT_TRUE(settle(
        [&]
        {
            return service.feed(FeedKind::home).state == Load::ready &&
                   service.feed(FeedKind::subscriptions).state == Load::ready;
        }))
        << "the restore never loaded the feeds";

    EXPECT_FALSE(signed_in_during_exchange.load());
    EXPECT_TRUE(flag_before_flip.load()) << "the restore flag must be published before the flip";
    EXPECT_TRUE(service.signed_in());
    // The instrumented read above consumed the one-shot flag, exactly once.
    EXPECT_FALSE(service.take_token_restored());

    // The same restore through the frame's own timing: the read happens the
    // moment the flip is visible and must find the toast.
    service.sign_out();
    ASSERT_TRUE(settle([&] { return !service.signed_in(); })) << "the sign-out never landed";
    service.import_token_file();
    ASSERT_TRUE(settle(
        [&]
        {
            return service.signed_in() && service.feed(FeedKind::home).state == Load::ready &&
                   service.feed(FeedKind::subscriptions).state == Load::ready;
        }))
        << "the second restore never landed";
    EXPECT_TRUE(service.take_token_restored()) << "the toast cannot be lost to the one-shot read";
    EXPECT_FALSE(service.take_token_restored());

    hui::testing::HttpStub::instance().clear();
    std::remove(file.c_str());
    std::remove(sandbox.c_str());
}

} // namespace

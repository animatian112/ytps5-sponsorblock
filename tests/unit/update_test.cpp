// youtube-tv - Auto-update check: how the release feed's answers map to states.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The whole check runs on its worker thread against the transport seam declared in
// src/update/update_check.h. This test supplies that seam itself (the host build's
// real stub, host/update_http_stub.cpp, is left out of sources.txt), so a check can
// be driven against a canned feed answer without touching the network.
//
// The repository has no published release yet, so the real feed answers 404: that is
// "nothing to update to", not a failure. A 500 stays a failure with its message.

#include "yt/update.hpp"

#include "update/update_check.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace
{

// ---- The canned feed ----------------------------------------------------------------------

std::mutex g_feed_mutex;
long g_feed_status = 200;
std::string g_feed_body;

void set_feed(long status, std::string body = {})
{
    std::lock_guard<std::mutex> lock(g_feed_mutex);
    g_feed_status = status;
    g_feed_body = std::move(body);
}

} // namespace

extern "C" int update_transport_get_json(const char *url, const char *user_agent,
                                         const char *accept, char **body, std::size_t *size,
                                         long *status)
{
    (void)url;
    (void)user_agent;
    (void)accept;
    std::lock_guard<std::mutex> lock(g_feed_mutex);
    if (body != nullptr)
    {
        *body = nullptr;
        if (!g_feed_body.empty())
        {
            char *copy = static_cast<char *>(std::malloc(g_feed_body.size() + 1));
            std::memcpy(copy, g_feed_body.c_str(), g_feed_body.size() + 1);
            *body = copy;
        }
    }
    if (size != nullptr)
        *size = g_feed_body.size();
    if (status != nullptr)
        *status = g_feed_status;
    return 0;
}

extern "C" int update_transport_download(const char *url, std::uint64_t limit,
                                         self_update_sink sink, void *user)
{
    (void)url;
    (void)limit;
    (void)sink;
    (void)user;
    return -1; /* the download is not part of these tests */
}

namespace
{

using yt::update::Snapshot;
using yt::update::State;

// Starts one check against the canned feed and waits for it to settle. The worker is
// long-lived, so a previous job may still be finishing when check() is called; the
// outer loop retries until the job is accepted (check() is a no-op while one runs).
Snapshot run_check(long status, std::string body = {})
{
    set_feed(status, std::move(body));
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const std::uint64_t base = yt::update::revision();
        yt::update::check();
        for (int i = 0; i < 100; ++i)
        {
            const Snapshot snapshot = yt::update::snapshot();
            if (snapshot.revision != base && snapshot.state != State::checking)
                return snapshot;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    ADD_FAILURE() << "the update check did not settle";
    return yt::update::snapshot();
}

TEST(Update, A404FromTheReleaseFeedMeansNoReleaseYet)
{
    yt::update::set_current_version("01.000.001");
    const Snapshot snapshot = run_check(404);
    // Not a failure: the repository simply has no published release, and the row says
    // so with no error text to nag about.
    EXPECT_EQ(snapshot.state, State::no_release);
    EXPECT_NE(snapshot.state, State::failed);
    EXPECT_TRUE(snapshot.error.empty()) << snapshot.error;
}

TEST(Update, A500FromTheReleaseFeedIsStillAFailure)
{
    yt::update::set_current_version("01.000.001");
    const Snapshot snapshot = run_check(500);
    EXPECT_EQ(snapshot.state, State::failed);
    EXPECT_EQ(snapshot.error, "the release feed answered 500");
}

TEST(Update, AReleaseEqualToTheRunningVersionIsUpToDate)
{
    yt::update::set_current_version("01.000.001");
    const Snapshot snapshot = run_check(200, R"({"tag_name":"01.000.001"})");
    EXPECT_EQ(snapshot.state, State::up_to_date);
    EXPECT_TRUE(snapshot.error.empty()) << snapshot.error;
}

} // namespace

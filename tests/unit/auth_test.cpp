// youtube-tv - The saved account: writing it, reading it back, and the file
// a user carries to a new install. No network is involved.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "http_stub.hpp"
#include "yt/auth.hpp"

#include "core/save_file.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace
{

std::string temp_path(const char *name)
{
    const std::string path = ::testing::TempDir() + "yt_auth_" + name + ".json";
    std::remove(path.c_str());
    return path;
}

TEST(Auth, AnAdoptedTokenIsSignedInAndExportedVerbatim)
{
    const std::string sandbox = temp_path("adopt");
    yt::Auth auth(sandbox);
    EXPECT_FALSE(auth.signed_in()) << "nothing signed in without a file";

    std::string error;
    EXPECT_TRUE(
        auth.adopt_token_json(R"({"v":1,"refresh_token":"1//0eXAMPLE-token_value"})", &error))
        << error;
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(auth.signed_in());

    // The file the app would hand the user carries the same token.
    const std::string exported = auth.token_json();
    EXPECT_NE(exported.find("1//0eXAMPLE-token_value"), std::string::npos) << exported;

    // A fresh object with the same path is signed in again: this is what
    // happens on the next launch, and after a re-jailbreak.
    yt::Auth reopened(sandbox);
    EXPECT_TRUE(reopened.signed_in());
    EXPECT_EQ(reopened.token_json(), exported);

    std::remove(sandbox.c_str());
}

TEST(Auth, GarbageNeverPretendsToBeAnAccount)
{
    const std::string sandbox = temp_path("garbage");
    yt::Auth auth(sandbox);
    std::string error;
    EXPECT_FALSE(auth.adopt_token_json("", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(auth.adopt_token_json("not json", &error));
    EXPECT_FALSE(auth.adopt_token_json(R"({"v":1})", &error)) << "no refresh_token field";
    EXPECT_FALSE(auth.adopt_token_json(R"({"refresh_token":42})", &error));
    EXPECT_FALSE(auth.signed_in());

    // Signing out removes the file, so the next launch starts signed out.
    ASSERT_TRUE(auth.adopt_token_json(R"({"refresh_token":"abc"})", &error));
    ASSERT_TRUE(auth.signed_in());
    auth.sign_out();
    EXPECT_FALSE(auth.signed_in());
    EXPECT_TRUE(auth.token_json().empty());
    yt::Auth reopened(sandbox);
    EXPECT_FALSE(reopened.signed_in());
    std::remove(sandbox.c_str());
}

TEST(Auth, TheBundledFileIsReadThroughTheKitsReader)
{
    // The import path the console uses: the user drops yt-token.json into the
    // install folder, which the app reads as /app0/yt-token.json.
    const std::string folder = ::testing::TempDir() + "yt_install";
    ASSERT_TRUE(hui::save::ensure_directory(folder));
    const std::string file = folder + "/yt-token.json";
    ASSERT_EQ(hui::save::write_atomic(file, R"({"v":1,"refresh_token":"bundled-token"})"), "");

    std::string data;
    ASSERT_TRUE(hui::save::read_file(file, &data, 64u << 10));
    yt::Auth auth(temp_path("bundled_sandbox"));
    std::string error;
    ASSERT_TRUE(auth.adopt_token_json(data, &error)) << error;
    EXPECT_TRUE(auth.signed_in());
    std::remove(file.c_str());
}

TEST(Auth, ARefreshSendsTheGrantAndKeepsTheRotatedToken)
{
    const std::string sandbox = temp_path("rotated");
    yt::Auth auth(sandbox);
    std::string error;
    ASSERT_TRUE(auth.adopt_token_json(R"({"v":1,"refresh_token":"old-refresh"})", &error)) << error;

    hui::testing::HttpStub::instance().set_handler(
        [](const yt::http::Request &request) -> yt::http::Response
        {
            yt::http::Response response;
            if (request.url.find("/token") == std::string::npos)
                return response; // the identity probe: no bundle, fallback pair
            EXPECT_NE(request.body.find("grant_type=refresh_token"), std::string::npos)
                << request.body;
            EXPECT_NE(request.body.find("refresh_token=old-refresh"), std::string::npos)
                << request.body;
            response.status = 200;
            response.body = R"({"access_token":"access-1","refresh_token":"rotated-refresh"})";
            return response;
        });

    EXPECT_TRUE(auth.refresh(&error)) << error;
    EXPECT_EQ(auth.access_token(), "access-1");
    // The rotated refresh token is kept on disk: the next launch (and the
    // next restore) must not present a grant the server no longer knows.
    EXPECT_NE(auth.token_json().find("rotated-refresh"), std::string::npos) << auth.token_json();
    yt::Auth reopened(sandbox);
    EXPECT_TRUE(reopened.signed_in());
    EXPECT_NE(reopened.token_json().find("rotated-refresh"), std::string::npos);

    hui::testing::HttpStub::instance().clear();
    std::remove(sandbox.c_str());
}

TEST(Auth, ARejectedTokenEndsSignedOutAndNamesTheCause)
{
    const std::string sandbox = temp_path("rejected");
    yt::Auth auth(sandbox);
    std::string error;
    ASSERT_TRUE(auth.adopt_token_json(R"({"v":1,"refresh_token":"dead-refresh"})", &error))
        << error;
    ASSERT_TRUE(auth.signed_in());

    hui::testing::HttpStub::instance().set_handler(
        [](const yt::http::Request &request) -> yt::http::Response
        {
            yt::http::Response response;
            if (request.url.find("/token") == std::string::npos)
                return response;
            response.status = 400;
            response.body = R"({"error":"invalid_grant",)"
                            R"("error_description":"Token has been expired or revoked."})";
            return response;
        });

    EXPECT_FALSE(auth.refresh(&error));
    // The server rejected the grant: the account is signed out and the error
    // names the cause - not a token kept around that can never work again.
    EXPECT_FALSE(auth.signed_in());
    EXPECT_NE(error.find("expired or revoked"), std::string::npos) << error;
    // The dead token is gone from disk too, so the next launch starts signed
    // out instead of presenting a rejected grant again.
    yt::Auth reopened(sandbox);
    EXPECT_FALSE(reopened.signed_in());

    hui::testing::HttpStub::instance().clear();
    std::remove(sandbox.c_str());
}

TEST(Auth, ARefusalThatNamesNoDeadGrantKeepsTheToken)
{
    const std::string sandbox = temp_path("kept");
    yt::Auth auth(sandbox);
    std::string error;
    ASSERT_TRUE(auth.adopt_token_json(R"({"v":1,"refresh_token":"kept-refresh"})", &error))
        << error;
    ASSERT_TRUE(auth.signed_in());

    hui::testing::HttpStub::instance().set_handler(
        [](const yt::http::Request &request) -> yt::http::Response
        {
            yt::http::Response response;
            if (request.url.find("/token") == std::string::npos)
                return response;
            // RFC 6749: a dead grant is answered 400 + invalid_grant; this
            // endpoint's 401 names the client pair (invalid_client), which
            // comes from a scraped TV bundle with a stale hard-coded fallback.
            response.status = 401;
            response.body = R"({"error":"invalid_client","error_description":"Unauthorized"})";
            return response;
        });

    EXPECT_FALSE(auth.refresh(&error));
    // Nothing named the grant as dead: the token stays, in memory and on
    // disk, so a stale client pair (or a captive portal or proxy answering
    // 401) cannot force an account the server never rejected back through
    // the link flow.
    EXPECT_TRUE(auth.signed_in());
    EXPECT_NE(error.find("Unauthorized"), std::string::npos) << error;
    EXPECT_NE(auth.token_json().find("kept-refresh"), std::string::npos) << auth.token_json();
    yt::Auth reopened(sandbox);
    EXPECT_TRUE(reopened.signed_in());
    EXPECT_NE(reopened.token_json().find("kept-refresh"), std::string::npos);

    // A bare 401 with no body names nothing at all, so it keeps the token too.
    hui::testing::HttpStub::instance().set_handler(
        [](const yt::http::Request &request) -> yt::http::Response
        {
            yt::http::Response response;
            if (request.url.find("/token") == std::string::npos)
                return response;
            response.status = 401;
            return response;
        });
    EXPECT_FALSE(auth.refresh(&error));
    EXPECT_TRUE(auth.signed_in());
    EXPECT_NE(auth.token_json().find("kept-refresh"), std::string::npos) << auth.token_json();

    hui::testing::HttpStub::instance().clear();
    std::remove(sandbox.c_str());
}

} // namespace

// youtube-tv - The saved account: writing it, reading it back, and the file
// a user carries to a new install. No network is involved.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

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

} // namespace

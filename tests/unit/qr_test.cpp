// youtube-tv - The sign-in QR: module count, scale and bitmap invariants.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/qr.hpp"

#include <gtest/gtest.h>

namespace
{

constexpr const char *kUrl = "https://www.google.com/device";

TEST(Qr, ScalesToThePixelsPerModuleItIsAskedFor)
{
    const int modules = yt::qr_modules(kUrl);
    ASSERT_GT(modules, 0);

    yt::QrImage small;
    ASSERT_TRUE(yt::make_qr(kUrl, 1, &small));
    EXPECT_EQ(small.width, modules + 8) << "four-module quiet zone on each side";
    EXPECT_EQ(small.pixels_per_module, 1);

    yt::QrImage large;
    ASSERT_TRUE(yt::make_qr(kUrl, 12, &large));
    EXPECT_EQ(large.width, (modules + 8) * 12);
    EXPECT_EQ(large.rgba.size(), static_cast<std::size_t>(large.width) * large.height * 4);
    EXPECT_TRUE(large.valid());

    // Every module is a solid square of `scale` pixels: the quiet zone stays
    // white, the top-left finder pattern is dark, and its edges are hard.
    const auto pixel = [&large](int x, int y)
    {
        const std::size_t offset = (static_cast<std::size_t>(y) * large.width + x) * 4;
        return large.rgba[offset];
    };
    EXPECT_EQ(pixel(10, 10), 0xff) << "inside the quiet zone";
    EXPECT_EQ(pixel(4 * 12 + 6, 4 * 12 + 6), 0x00) << "finder pattern corner";
    EXPECT_EQ(pixel(7 * 12 + 6, 7 * 12 + 6), 0x00) << "finder pattern centre";
    EXPECT_EQ(pixel(4 * 12, 4 * 12), 0x00) << "first dark pixel of the module";
    EXPECT_EQ(pixel(4 * 12 - 1, 4 * 12), 0xff) << "last light pixel before it";
}

TEST(Qr, RefusesImpossibleRequests)
{
    yt::QrImage image;
    EXPECT_FALSE(yt::make_qr(kUrl, 0, &image)) << "no pixels per module";
    EXPECT_FALSE(yt::make_qr(kUrl, 65, &image)) << "past the cap";
    EXPECT_FALSE(yt::make_qr(kUrl, 8, nullptr)) << "no bitmap to fill";

    // The login screen asks for the module count before it picks the scale, so
    // even an empty code has to answer with something drawable.
    EXPECT_GT(yt::qr_modules(""), 0);
}

} // namespace

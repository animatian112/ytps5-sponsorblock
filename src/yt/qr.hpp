// youtube-tv - QR code bitmap for the sign-in screen.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace yt
{

struct QrImage
{
    int width = 0; // (modules + quiet zone) * pixels_per_module
    int height = 0;
    // How many pixels one module is drawn with. Building the bitmap at the
    // size it is shown keeps it sharp: a one-pixel-per-module texture scaled
    // up by the renderer's linear filter is what made it blurry.
    int pixels_per_module = 1;
    std::vector<std::uint8_t> rgba; // width * height * 4, black/white, opaque

    bool valid() const
    {
        return width > 0 && height > 0 &&
               rgba.size() ==
                   static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    }
};

// Encodes text (error correction M), adds a four-module quiet zone and draws
// every module as pixels_per_module x pixels_per_module pixels. Returns false
// when the text cannot be encoded.
bool make_qr(const std::string &text, int pixels_per_module, QrImage *image);
// The module count (without the quiet zone) for a text, 0 on failure: the
// caller uses it to pick the pixels_per_module that fills its panel.
int qr_modules(const std::string &text);

} // namespace yt

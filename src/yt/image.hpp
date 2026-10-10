// youtube-tv - RGBA image decoding for thumbnails and artwork.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One function over stb_image: bytes in (JPEG, PNG, WebP, ...), tightly
// packed RGBA8 out. Host and console both compile this file, so callers can
// decode on a worker thread before handing the pixels to OpenGL.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace yt
{

// Decodes bytes into a width * height * 4 RGBA buffer (top row first).
// Returns false and fills error on any failure. All out parameters are
// required and are cleared or zeroed before the work starts.
bool decode_image(const std::string &bytes, int *width, int *height,
                  std::vector<std::uint8_t> *rgba, std::string *error);

} // namespace yt

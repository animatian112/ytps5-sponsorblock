// youtube-tv - RGBA image decoding for thumbnails and artwork.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/image.hpp"

// The single translation unit that carries stb_image's implementation. The
// vendor header lives at the repo root; the components let it work no matter
// whether the build points at src/third_party or at third_party/.
//
// stb_image is upstream code, so it must not inherit the project's strict
// warning set (-Wextra -Wpedantic -Werror in the unit tests).
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <climits>
#include <cstddef>

namespace yt
{

bool decode_image(const std::string &bytes, int *width, int *height,
                  std::vector<std::uint8_t> *rgba, std::string *error)
{
    if (width != nullptr)
    {
        *width = 0;
    }
    if (height != nullptr)
    {
        *height = 0;
    }
    if (rgba != nullptr)
    {
        rgba->clear();
    }
    if (error == nullptr)
        return false; // every failure below reports through it
    error->clear();
    if (width == nullptr || height == nullptr || rgba == nullptr)
    {
        *error = "decode_image: null output";
        return false;
    }
    if (bytes.empty())
    {
        *error = "decode_image: empty input";
        return false;
    }
    if (bytes.size() > static_cast<std::size_t>(INT_MAX))
    {
        *error = "decode_image: input too large";
        return false;
    }

    int decoded_width = 0;
    int decoded_height = 0;
    stbi_uc *pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()),
                                            static_cast<int>(bytes.size()), &decoded_width,
                                            &decoded_height, nullptr, 4);
    if (pixels == nullptr || decoded_width <= 0 || decoded_height <= 0)
    {
        const char *reason = stbi_failure_reason();
        *error = reason != nullptr ? std::string("decode_image: ") + reason
                                   : "decode_image: not an image";
        if (pixels != nullptr)
        {
            stbi_image_free(pixels);
        }
        return false;
    }

    const std::size_t size =
        static_cast<std::size_t>(decoded_width) * static_cast<std::size_t>(decoded_height) * 4u;
    rgba->assign(pixels, pixels + size);
    stbi_image_free(pixels);

    *width = decoded_width;
    *height = decoded_height;
    return true;
}

} // namespace yt

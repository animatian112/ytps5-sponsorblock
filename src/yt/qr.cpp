// youtube-tv - QR code bitmap for the sign-in screen.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One pixel per module, error correction M, a four-module quiet zone on each
// side, row-major with the top row first so the caller can upload the buffer
// straight into a GL texture.

#include "yt/qr.hpp"

#include "third_party/qrcodegen/qrcodegen.h"

#include <cstddef>
#include <cstdint>

namespace yt
{

namespace
{

// Both entry points share the encoding step (no allocation, C library).
bool encode(const std::string &text, std::uint8_t *code)
{
    std::uint8_t scratch[qrcodegen_BUFFER_LEN_MAX];
    return qrcodegen_encodeText(text.c_str(), scratch, code, qrcodegen_Ecc_MEDIUM,
                                qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO,
                                true);
}

} // namespace

int qr_modules(const std::string &text)
{
    std::uint8_t code[qrcodegen_BUFFER_LEN_MAX];
    if (!encode(text, code))
        return 0;
    return qrcodegen_getSize(code);
}

bool make_qr(const std::string &text, int pixels_per_module, QrImage *image)
{
    if (image == nullptr || pixels_per_module < 1 || pixels_per_module > 64)
        return false;
    *image = QrImage{};

    std::uint8_t code[qrcodegen_BUFFER_LEN_MAX];
    if (!encode(text, code))
        return false;

    const int module_count = qrcodegen_getSize(code);
    const int quiet = 4;
    const int side = module_count + quiet * 2; // modules including the quiet zone
    if (side <= 0)
        return false;

    const int scale = pixels_per_module;
    const int width = side * scale;
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(width);
    image->width = width;
    image->height = width;
    image->pixels_per_module = scale;
    image->rgba.assign(pixels * 4, 0xff); // all white, opaque

    for (int y = 0; y < module_count; ++y)
    {
        for (int x = 0; x < module_count; ++x)
        {
            if (!qrcodegen_getModule(code, x, y))
                continue; // light: the white fill already did it
            for (int dy = 0; dy < scale; ++dy)
            {
                const int row = (y + quiet) * scale + dy;
                for (int dx = 0; dx < scale; ++dx)
                {
                    const int column = (x + quiet) * scale + dx;
                    const std::size_t offset =
                        (static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                         static_cast<std::size_t>(column)) *
                        4;
                    image->rgba[offset + 0] = 0x00;
                    image->rgba[offset + 1] = 0x00;
                    image->rgba[offset + 2] = 0x00;
                    image->rgba[offset + 3] = 0xff;
                }
            }
        }
    }
    return image->valid();
}

} // namespace yt

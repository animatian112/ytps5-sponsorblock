// youtube-tv - Codec classification helpers.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/types.hpp"

namespace yt
{

std::string absolute_url(std::string_view url)
{
    if (url.empty())
        return {};
    if (url.compare(0, 2, "//") == 0)
        return "https:" + std::string(url);
    if (url.find("://") != std::string_view::npos)
        return std::string(url);
    if (url.compare(0, 1, "/") == 0)
        return "https://www.youtube.com" + std::string(url);
    return std::string(url);
}

bool is_video_codec(Codec codec)
{
    switch (codec)
    {
    case Codec::avc:
    case Codec::vp9:
    case Codec::av1:
        return true;
    default:
        return false;
    }
}

bool is_audio_codec(Codec codec)
{
    switch (codec)
    {
    case Codec::aac:
    case Codec::opus:
        return true;
    default:
        return false;
    }
}

const char *codec_label(Codec codec)
{
    switch (codec)
    {
    case Codec::avc:
        return "AVC";
    case Codec::vp9:
        return "VP9";
    case Codec::av1:
        return "AV1";
    case Codec::aac:
        return "AAC";
    case Codec::opus:
        return "Opus";
    default:
        return "?";
    }
}

} // namespace yt

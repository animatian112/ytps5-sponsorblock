// youtube-tv - Stream selection: which adaptive format plays.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/codec.hpp"

namespace yt
{

bool codec_enabled(const CodecChoice &choice, Codec codec)
{
    switch (codec)
    {
    case Codec::avc:
        return choice.avc;
    case Codec::vp9:
        return choice.vp9;
    case Codec::av1:
        return choice.av1;
    default:
        return false;
    }
}

namespace
{

// Preference among equal heights: AV1, then VP9, then AVC - the order the
// YouTube TV client uses. Ties inside one codec go to the higher bitrate.
int codec_rank(Codec codec)
{
    switch (codec)
    {
    case Codec::av1:
        return 3;
    case Codec::vp9:
        return 2;
    case Codec::avc:
        return 1;
    default:
        return 0;
    }
}

} // namespace

const Format *select_video_format(const std::vector<Format> &formats, const CodecChoice &choice)
{
    const Format *best = nullptr;
    for (const Format &format : formats)
    {
        if (!is_video_codec(format.codec) || !codec_enabled(choice, format.codec))
            continue;
        if (format.url.empty() || format.height <= 0)
            continue;
        if (choice.max_height > 0 && format.height > choice.max_height)
            continue;
        if (best == nullptr)
        {
            best = &format;
            continue;
        }
        if (format.height > best->height)
            best = &format;
        else if (format.height == best->height)
        {
            const int rank = codec_rank(format.codec);
            const int best_rank = codec_rank(best->codec);
            if (rank > best_rank || (rank == best_rank && format.bitrate > best->bitrate))
                best = &format;
        }
    }
    return best;
}

const Format *select_audio_format(const std::vector<Format> &formats)
{
    const Format *best = nullptr;
    for (const Format &format : formats)
    {
        if (!is_audio_codec(format.codec) || format.url.empty())
            continue;
        if (best == nullptr)
        {
            best = &format;
            continue;
        }
        const bool opus = format.codec == Codec::opus;
        const bool best_opus = best->codec == Codec::opus;
        if (opus != best_opus)
        {
            if (opus)
                best = &format;
            continue;
        }
        if (format.bitrate > best->bitrate)
            best = &format;
    }
    return best;
}

} // namespace yt

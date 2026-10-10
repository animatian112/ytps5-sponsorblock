// youtube-tv - Stream selection: the codec switches decide what plays.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/codec.hpp"

#include <gtest/gtest.h>

namespace
{

using yt::Codec;
using yt::CodecChoice;
using yt::Format;

Format video(Codec codec, int height, long long bitrate)
{
    Format format;
    format.url = "https://example.invalid/stream";
    format.codec = codec;
    format.width = height * 16 / 9;
    format.height = height;
    format.bitrate = bitrate;
    return format;
}

Format audio(Codec codec, int sample_rate, long long bitrate)
{
    Format format;
    format.url = "https://example.invalid/audio";
    format.codec = codec;
    format.sample_rate = sample_rate;
    format.bitrate = bitrate;
    return format;
}

std::vector<Format> mixed()
{
    return {video(Codec::avc, 1080, 4'000'000), video(Codec::vp9, 1080, 3'000'000),
            video(Codec::av1, 1080, 2'000'000), video(Codec::avc, 720, 1'500'000),
            video(Codec::vp9, 2160, 9'000'000), video(Codec::avc, 2160, 12'000'000)};
}

TEST(Codec, PrefersAv1ThenVp9ThenAvcAtTheHighestAllowedHeight)
{
    const std::vector<Format> formats = mixed();
    CodecChoice choice; // 1080p, everything on
    const Format *picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::av1);
    EXPECT_EQ(picked->height, 1080);

    choice.av1 = false;
    picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::vp9);

    choice.vp9 = false;
    picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::avc);

    choice.avc = false;
    EXPECT_EQ(yt::select_video_format(formats, choice), nullptr) << "all codecs off";

    // With only VP9 enabled the 2160p entry becomes reachable.
    choice.vp9 = true;
    choice.max_height = 2160;
    picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::vp9);
    EXPECT_EQ(picked->height, 2160);
}

TEST(Codec, TheResolutionCapWinsOverCodecPreference)
{
    const std::vector<Format> formats = mixed();
    CodecChoice choice;
    choice.max_height = 720;
    const Format *picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->height, 720);
    EXPECT_EQ(picked->codec, Codec::avc);

    // Nothing at or below 240p: no stream, and the caller shows the hint.
    choice.max_height = 240;
    EXPECT_EQ(yt::select_video_format(formats, choice), nullptr);
}

TEST(Codec, AFormatWithoutUrlIsNeverPicked)
{
    std::vector<Format> formats = mixed();
    formats[2].url.clear(); // the 1080p AV1 entry
    CodecChoice choice;
    const Format *picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::vp9);
}

TEST(Codec, OpusBeatsAacAndTheHigherBitrateWinsInsideOneCodec)
{
    std::vector<Format> formats = {audio(Codec::aac, 48000, 128'000),
                                   audio(Codec::opus, 48000, 160'000),
                                   audio(Codec::opus, 48000, 90'000)};
    const Format *picked = yt::select_audio_format(formats);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::opus);
    EXPECT_EQ(picked->bitrate, 160'000);

    formats.clear();
    formats.push_back(audio(Codec::aac, 44100, 128'000));
    picked = yt::select_audio_format(formats);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::aac);

    EXPECT_EQ(yt::select_audio_format({}), nullptr);
}

TEST(Codec, CodecEnabledFollowsTheSwitches)
{
    CodecChoice choice;
    EXPECT_TRUE(yt::codec_enabled(choice, Codec::av1));
    choice.av1 = false;
    EXPECT_FALSE(yt::codec_enabled(choice, Codec::av1));
    EXPECT_FALSE(yt::codec_enabled(choice, Codec::unknown));
    EXPECT_FALSE(yt::codec_enabled(choice, Codec::opus));
}

} // namespace

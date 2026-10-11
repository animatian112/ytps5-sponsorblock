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

TEST(Codec, PrefersAvcThenVp9ThenAv1AtEqualHeight)
{
    const std::vector<Format> formats = {video(Codec::av1, 480, 1'000'000),
                                         video(Codec::vp9, 480, 2'000'000),
                                         video(Codec::avc, 480, 3'000'000)};
    CodecChoice choice;
    const Format *picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::avc) << "software decoding prefers AVC at equal height";

    choice.avc = false;
    picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::vp9);

    choice.vp9 = false;
    picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::av1) << "AV1 is last, not first";
}

TEST(Codec, ACodecCeilingSkipsItsOwnTallerFormat)
{
    CodecChoice choice;

    // VP9 tops out at 720p, so its 1080p entry loses to the 720p one.
    const std::vector<Format> vp9 = {video(Codec::vp9, 1080, 3'000'000),
                                     video(Codec::vp9, 720, 2'000'000)};
    const Format *picked = yt::select_video_format(vp9, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::vp9);
    EXPECT_EQ(picked->height, 720);

    // AV1 tops out at 480p, so its 1080p entry loses to the 480p one.
    const std::vector<Format> av1 = {video(Codec::av1, 1080, 2'000'000),
                                     video(Codec::av1, 480, 1'000'000)};
    picked = yt::select_video_format(av1, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::av1);
    EXPECT_EQ(picked->height, 480);
}

TEST(Codec, TheUserCapLowersTheCeilingFurther)
{
    const std::vector<Format> formats = {video(Codec::avc, 1080, 4'000'000),
                                         video(Codec::avc, 720, 2'000'000)};
    CodecChoice choice;
    const Format *picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->height, 1080) << "AVC may use 1080p on its own";

    // The cap wins over the codec's own ceiling.
    choice.max_height = 720;
    picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->height, 720);

    // And it also lowers a ceiling that is already lower than the cap.
    const std::vector<Format> vp9 = {video(Codec::vp9, 720, 3'000'000),
                                     video(Codec::vp9, 480, 2'000'000)};
    choice.max_height = 480;
    picked = yt::select_video_format(vp9, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->height, 480);
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

TEST(Codec, OnlyOverCeilingFormatsFallBackToTheLowestEnabledOne)
{
    CodecChoice choice;

    // Both entries sit above their ceiling; the more manageable one plays.
    std::vector<Format> formats = {video(Codec::vp9, 1080, 3'000'000),
                                   video(Codec::av1, 720, 2'000'000)};
    const Format *picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::av1);
    EXPECT_EQ(picked->height, 720) << "the lowest over-ceiling entry wins";

    // A single over-ceiling stream is still better than a black screen.
    formats = {video(Codec::av1, 1080, 2'000'000)};
    picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::av1);
    EXPECT_EQ(picked->height, 1080);

    // The fallback never crosses the user's own cap.
    choice.max_height = 240;
    EXPECT_EQ(yt::select_video_format(formats, choice), nullptr);
}

TEST(Codec, ADisabledCodecIsNeverReturned)
{
    CodecChoice choice;

    // In-ceiling but switched off: nothing.
    std::vector<Format> formats = {video(Codec::av1, 480, 1'000'000)};
    choice.av1 = false;
    EXPECT_EQ(yt::select_video_format(formats, choice), nullptr);

    // The fallback respects the switches too.
    choice = CodecChoice();
    choice.vp9 = false;
    formats = {video(Codec::vp9, 1080, 3'000'000)};
    EXPECT_EQ(yt::select_video_format(formats, choice), nullptr);

    // All codecs off still yields nothing.
    choice = CodecChoice();
    choice.avc = false;
    choice.vp9 = false;
    choice.av1 = false;
    formats = mixed();
    EXPECT_EQ(yt::select_video_format(formats, choice), nullptr) << "all codecs off";
}

TEST(Codec, AFormatWithoutUrlIsNeverPicked)
{
    std::vector<Format> formats = {video(Codec::avc, 1080, 4'000'000),
                                   video(Codec::avc, 720, 2'000'000)};
    formats[0].url.clear(); // the 1080p AVC entry would otherwise win
    CodecChoice choice;
    const Format *picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->height, 720);
}

TEST(Codec, DescribeSelectionNamesTheFormatAndTheAppliedCeiling)
{
    CodecChoice choice;
    Format format = video(Codec::avc, 1080, 4'000'000);
    format.itag = 137;
    const std::string line = yt::describe_selection(&format, choice);
    EXPECT_NE(line.find("itag=137"), std::string::npos);
    EXPECT_NE(line.find("1920x1080"), std::string::npos);
    EXPECT_NE(line.find("AVC"), std::string::npos);
    EXPECT_NE(line.find("ceiling=1080p"), std::string::npos);

    // Over the ceiling: the fallback is called out.
    Format over = video(Codec::av1, 720, 2'000'000);
    const std::string fallback_line = yt::describe_selection(&over, choice);
    EXPECT_NE(fallback_line.find("ceiling=480p"), std::string::npos);
    EXPECT_NE(fallback_line.find("fallback=above ceiling"), std::string::npos);

    EXPECT_EQ(yt::describe_selection(nullptr, choice), "none");
}

TEST(Codec, SoftwareCeilingsArePerCodecAndTheCapWins)
{
    CodecChoice choice;
    EXPECT_EQ(yt::codec_height_ceiling(Codec::avc, choice), 1080);
    EXPECT_EQ(yt::codec_height_ceiling(Codec::vp9, choice), 720);
    EXPECT_EQ(yt::codec_height_ceiling(Codec::av1, choice), 480);

    choice.max_height = 480;
    EXPECT_EQ(yt::codec_height_ceiling(Codec::avc, choice), 480);
    EXPECT_EQ(yt::codec_height_ceiling(Codec::vp9, choice), 480);

    choice.max_height = 2160; // the cap alone never raises a ceiling
    EXPECT_EQ(yt::codec_height_ceiling(Codec::avc, choice), 1080);
    EXPECT_EQ(yt::codec_height_ceiling(Codec::opus, choice), 0);
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

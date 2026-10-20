// youtube-tv - Stream selection: the codec switches decide what plays.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/codec.hpp"
#include "yt/innertube.hpp"

#include <gtest/gtest.h>

namespace
{

using yt::Codec;
using yt::CodecChoice;
using yt::DecodeMode;
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

// The same entry gated behind the player's signature: the unsigned base URL
// plus the scrambled signature the descrambler would have to descramble before
// the format can be opened.
Format ciphered_video(Codec codec, int height, long long bitrate)
{
    Format format = video(codec, height, bitrate);
    format.ciphered = true;
    format.cipher_s = "abcdef";
    format.cipher_sp = "sig";
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
    choice.decode = DecodeMode::software; // the CPU ceilings under test
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
    choice.decode = DecodeMode::software; // the CPU ceilings under test

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
    choice.decode = DecodeMode::software; // the CPU ceilings under test
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
    choice.decode = DecodeMode::software; // the CPU ceilings under test
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
    choice.decode = DecodeMode::software; // the CPU ceilings under test

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
    choice.decode = DecodeMode::software; // the CPU ceilings under test

    // In-ceiling but switched off: nothing.
    std::vector<Format> formats = {video(Codec::av1, 480, 1'000'000)};
    choice.av1 = false;
    EXPECT_EQ(yt::select_video_format(formats, choice), nullptr);

    // The fallback respects the switches too.
    choice = CodecChoice();
    choice.decode = DecodeMode::software; // the CPU ceilings under test
    choice.vp9 = false;
    formats = {video(Codec::vp9, 1080, 3'000'000)};
    EXPECT_EQ(yt::select_video_format(formats, choice), nullptr);

    // All codecs off still yields nothing.
    choice = CodecChoice();
    choice.decode = DecodeMode::software; // the CPU ceilings under test
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
    choice.decode = DecodeMode::software; // the CPU ceilings under test
    const Format *picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->height, 720);
}

TEST(Codec, APlainUrlBeatsACipheredOne)
{
    // A plain URL opens as it stands; a ciphered one needs the descrambler
    // first (an unproven heuristic) whose output googlevideo can refuse with
    // 403. So the plain 720p VP9 entry beats the ciphered 1080p VP9 one under
    // the same codec ceiling even though the ciphered entry is taller.
    std::vector<Format> formats = {video(Codec::vp9, 720, 2'000'000),
                                   ciphered_video(Codec::vp9, 1080, 3'000'000)};
    CodecChoice choice;
    choice.decode = DecodeMode::software; // the CPU ceilings under test
    const Format *picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_FALSE(picked->ciphered);
    EXPECT_EQ(picked->height, 720);

    // At equal height the plain entry wins over a higher bitrate as well.
    formats = {video(Codec::avc, 720, 1'500'000), ciphered_video(Codec::avc, 720, 4'000'000)};
    picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_FALSE(picked->ciphered);
    EXPECT_EQ(picked->bitrate, 1'500'000);

    // Even a ciphered entry that fits its own ceiling loses to an enabled
    // plain one: AVC may use 1080p and 1080 beats 480 on height alone, yet the
    // plain URL wins while a plain format survives the caps and the ceilings.
    formats = {video(Codec::avc, 480, 1'000'000), ciphered_video(Codec::avc, 1080, 4'000'000)};
    picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_FALSE(picked->ciphered);
    EXPECT_EQ(picked->height, 480);
}

TEST(Codec, ACipheredFormatIsPickedWhenNoPlainOneCanPlay)
{
    // Nothing plain at all: the ciphered entry is the only candidate and still
    // beats no stream.
    std::vector<Format> formats = {ciphered_video(Codec::avc, 720, 2'000'000)};
    CodecChoice choice;
    choice.decode = DecodeMode::software; // the CPU ceilings under test
    const Format *picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_TRUE(picked->ciphered);
    EXPECT_EQ(picked->height, 720);

    // The plain entry exists but its codec is switched off: nothing plain is
    // enabled, so the ciphered AVC is picked.
    std::vector<Format> mixed_kinds = {video(Codec::vp9, 720, 2'000'000),
                                       ciphered_video(Codec::avc, 720, 1'000'000)};
    choice.vp9 = false;
    picked = yt::select_video_format(mixed_kinds, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_TRUE(picked->ciphered);
    EXPECT_EQ(picked->codec, Codec::avc);

    // A plain entry that can never play (no URL) does not block the pick
    // either.
    std::vector<Format> broken = {video(Codec::avc, 720, 2'000'000),
                                  ciphered_video(Codec::avc, 1080, 4'000'000)};
    broken[0].url.clear();
    choice = CodecChoice();
    choice.decode = DecodeMode::software; // the CPU ceilings under test
    picked = yt::select_video_format(broken, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_TRUE(picked->ciphered);
    EXPECT_EQ(picked->height, 1080);

    // The over-ceiling fallback keeps its rules inside the ciphered pool: the
    // lowest above-ceiling entry plays.
    std::vector<Format> tall = {ciphered_video(Codec::vp9, 1080, 3'000'000),
                                ciphered_video(Codec::av1, 720, 2'000'000)};
    picked = yt::select_video_format(tall, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_TRUE(picked->ciphered);
    EXPECT_EQ(picked->codec, Codec::av1);
    EXPECT_EQ(picked->height, 720);
}

TEST(Codec, ACipheredFormatPlaysWhenNoPlainOneFitsTheCapsAndCeilings)
{
    // The plain entry sits far above the user's cap: nothing plain survives
    // it, so the ciphered 720p entry under the cap is picked - a refused
    // ciphered URL still beats no stream, and the caller retries past it.
    std::vector<Format> formats = {video(Codec::avc, 2160, 12'000'000),
                                   ciphered_video(Codec::avc, 720, 2'000'000)};
    CodecChoice choice;
    choice.decode = DecodeMode::software; // the CPU ceilings under test
    choice.max_height = 1080;
    const Format *picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_TRUE(picked->ciphered);
    EXPECT_EQ(picked->height, 720);

    // The same when the plain entry only fails its codec ceiling: 1080p VP9
    // cannot stay real time on the CPU, so the in-ceiling ciphered 720p entry
    // is tried first.
    std::vector<Format> over = {video(Codec::vp9, 1080, 3'000'000),
                                ciphered_video(Codec::vp9, 720, 2'000'000)};
    choice = CodecChoice();
    choice.decode = DecodeMode::software; // the CPU ceilings under test
    picked = yt::select_video_format(over, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_TRUE(picked->ciphered);
    EXPECT_EQ(picked->height, 720);
}

TEST(Codec, DescribeFormatNamesTheFormatAndItsUrlKind)
{
    Format format = video(Codec::avc, 1080, 4'000'000);
    format.itag = 137;
    format.ciphered = true;
    EXPECT_EQ(yt::describe_format(&format), "itag=137 1080p AVC (ciphered)");

    format = video(Codec::vp9, 720, 2'000'000);
    format.itag = 248;
    EXPECT_EQ(yt::describe_format(&format), "itag=248 720p VP9 (plain)");

    EXPECT_EQ(yt::describe_format(nullptr), "none");
}

TEST(Codec, DescribeSelectionNamesTheFormatAndTheAppliedCeiling)
{
    CodecChoice choice;
    choice.decode = DecodeMode::software; // the CPU ceilings under test
    Format format = video(Codec::avc, 1080, 4'000'000);
    format.itag = 137;
    const std::string line = yt::describe_selection(&format, choice);
    EXPECT_NE(line.find("itag=137"), std::string::npos);
    EXPECT_NE(line.find("1920x1080"), std::string::npos);
    EXPECT_NE(line.find("AVC"), std::string::npos);
    EXPECT_NE(line.find("(plain)"), std::string::npos) << "the URL kind is part of the line";
    EXPECT_NE(line.find("ceiling=1080p"), std::string::npos);

    // A ciphered format says so on the same line.
    Format ciphered = ciphered_video(Codec::vp9, 720, 2'000'000);
    ciphered.itag = 248;
    const std::string ciphered_line = yt::describe_selection(&ciphered, choice);
    EXPECT_NE(ciphered_line.find("itag=248"), std::string::npos);
    EXPECT_NE(ciphered_line.find("(ciphered)"), std::string::npos);

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
    choice.decode = DecodeMode::software; // the CPU ceilings under test
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

TEST(Codec, TheOriginalSoundTrackBeatsDubsAndAnAskedTrackBeatsBoth)
{
    const auto track =
        [](const char *id, const char *name, bool is_default, long long bitrate, bool drc = false)
    {
        Format format = audio(Codec::opus, 48000, bitrate);
        format.audio_track_id = id;
        format.audio_track_name = name;
        format.audio_default = is_default;
        format.drc = drc;
        return format;
    };
    // A German dub is listed first and has the higher bitrate.
    const std::vector<Format> formats = {track("de.3", "German", false, 200'000),
                                         track("en.4", "English original", true, 140'000),
                                         track("en.4", "English original", true, 160'000, true),
                                         track("es.3", "Spanish", false, 150'000)};
    const Format *picked = yt::select_audio_format(formats);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->audio_track_id, "en.4");
    EXPECT_FALSE(picked->drc);

    picked = yt::select_audio_format(formats, "es.3");
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->audio_track_id, "es.3");

    // A track the answer does not offer falls back to the original.
    picked = yt::select_audio_format(formats, "fr.3");
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->audio_track_id, "en.4");

    const std::vector<yt::AudioTrack> tracks = yt::audio_tracks(formats);
    ASSERT_EQ(tracks.size(), 3u);
    EXPECT_EQ(tracks[1].id, "en.4");
    EXPECT_TRUE(tracks[1].is_default);
    EXPECT_EQ(tracks[0].name, "German");
}

TEST(Codec, CodecEnabledFollowsTheSwitches)
{
    CodecChoice choice;
    choice.decode = DecodeMode::software; // the CPU ceilings under test
    EXPECT_TRUE(yt::codec_enabled(choice, Codec::av1));
    choice.av1 = false;
    EXPECT_FALSE(yt::codec_enabled(choice, Codec::av1));
    EXPECT_FALSE(yt::codec_enabled(choice, Codec::unknown));
    EXPECT_FALSE(yt::codec_enabled(choice, Codec::opus));
}

TEST(Codec, HardwareModeNeverSelectsAv1)
{
    CodecChoice choice;
    choice.decode = DecodeMode::hardware;

    // AV1 is the tallest entry that fits its own ceiling here, so it would win
    // on height in either software mode; hardware refuses it.
    const std::vector<Format> formats = {video(Codec::av1, 480, 1'000'000),
                                         video(Codec::avc, 360, 500'000)};
    const Format *picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::avc);
    EXPECT_EQ(picked->height, 360);

    // Only AV1 offered: hardware yields nothing rather than an AV1 frame.
    const std::vector<Format> av1_only = {video(Codec::av1, 480, 1'000'000)};
    EXPECT_EQ(yt::select_video_format(av1_only, choice), nullptr);

    // The over-ceiling fallback does not rescue AV1 either.
    const std::vector<Format> av1_tall = {video(Codec::av1, 1080, 2'000'000)};
    EXPECT_EQ(yt::select_video_format(av1_tall, choice), nullptr);
}

TEST(Codec, AutoDetectPrefersAvcOverEqualHeightAv1)
{
    CodecChoice choice;
    choice.decode = DecodeMode::auto_detect;
    // The AV1 entry doubles AVC's bitrate and still loses: rank comes first.
    const std::vector<Format> formats = {video(Codec::av1, 480, 9'000'000),
                                         video(Codec::avc, 480, 1'000'000)};
    const Format *picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::avc);
}

TEST(Codec, SoftwareModeKeepsTheOldSelection)
{
    const std::vector<Format> formats = mixed();
    CodecChoice software;
    software.decode = DecodeMode::software;
    // Before Auto/Hardware lifted the AVC/VP9 ceilings every mode selected
    // like software does; software itself must keep doing so.
    CodecChoice default_mode;
    default_mode.decode = DecodeMode::software;
    const Format *expected = yt::select_video_format(formats, default_mode);
    const Format *picked = yt::select_video_format(formats, software);
    ASSERT_NE(picked, nullptr);
    ASSERT_NE(expected, nullptr);
    EXPECT_EQ(picked->codec, expected->codec);
    EXPECT_EQ(picked->height, expected->height);

    // AV1 stays reachable in software mode; only hardware drops it.
    CodecChoice av1_only;
    av1_only.decode = DecodeMode::software;
    av1_only.avc = false;
    av1_only.vp9 = false;
    const std::vector<Format> av1_formats = {video(Codec::av1, 480, 1'000'000)};
    picked = yt::select_video_format(av1_formats, av1_only);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->codec, Codec::av1);
}

TEST(Codec, TheConsoleDecoderLiftsTheAvcAndVp9Ceilings)
{
    const std::vector<Format> formats = {video(Codec::vp9, 2160, 9'000'000),
                                         video(Codec::vp9, 720, 2'000'000),
                                         video(Codec::avc, 1080, 4'000'000)};
    CodecChoice software;
    software.decode = DecodeMode::software;
    software.max_height = 2160;
    const Format *picked = yt::select_video_format(formats, software);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->height, 1080); // VP9 capped at 720p, AVC at 1080p

    CodecChoice console = software;
    console.decode = DecodeMode::auto_detect;
    picked = yt::select_video_format(formats, console);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->height, 2160);

    console.max_height = 1440; // the user's cap still binds
    picked = yt::select_video_format(formats, console);
    ASSERT_NE(picked, nullptr);
    EXPECT_LE(picked->height, 1440);
}

TEST(Codec, APickedHeightPlaysPastTheSoftwareCeiling)
{
    // Software decoding caps VP9 at 720p; the quality menu's 1440p pick
    // still plays 1440p, and a pick the video lacks falls back to the cap.
    const std::vector<Format> formats{video(Codec::avc, 1080, 4000000),
                                      video(Codec::vp9, 1440, 9000000),
                                      video(Codec::avc, 480, 900000)};
    CodecChoice choice;
    choice.decode = DecodeMode::software;
    choice.max_height = 1440;
    choice.exact_height = 1440;
    const Format *picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->height, 1440);
    choice.max_height = 480;
    choice.exact_height = 480;
    picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->height, 480);
    choice.max_height = 720;
    choice.exact_height = 720;
    picked = yt::select_video_format(formats, choice);
    ASSERT_NE(picked, nullptr);
    EXPECT_EQ(picked->height, 480);
}

} // namespace

// A format picked from the full "Video formats" list plays as picked, even
// when the codec settings would not have chosen it.
TEST(Codec, AnExactFormatPickWins)
{
    std::vector<yt::Format> formats(2);
    formats[0].itag = 137;
    formats[0].codec = yt::Codec::avc;
    formats[0].height = 1080;
    formats[0].url = "https://a";
    formats[1].itag = 400;
    formats[1].codec = yt::Codec::av1;
    formats[1].height = 1440;
    formats[1].url = "https://b";
    yt::CodecChoice choice;
    choice.av1 = false;
    EXPECT_EQ(yt::select_video_format(formats, choice)->itag, 137);
    choice.exact_itag = 400;
    EXPECT_EQ(yt::select_video_format(formats, choice)->itag, 400);
    choice.exact_itag = 999; // gone from this answer: the usual choice
    EXPECT_EQ(yt::select_video_format(formats, choice)->itag, 137);
}

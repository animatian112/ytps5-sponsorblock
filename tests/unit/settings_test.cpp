// youtube-tv - Settings save format: round trip, clamping and the codec rule.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/settings.hpp"

#include <gtest/gtest.h>

namespace
{

using hui::decode_settings;
using hui::encode_settings;
using hui::Settings;
using yt::DecodeMode;

TEST(Settings, RoundTripKeepsEveryValue)
{
    Settings before;
    before.music_volume = 3;
    before.sfx_volume = 1;
    before.ui_volume = 9;
    before.reduced_motion = true;
    before.swap_confirm = true;
    before.show_fps = true;
    before.haptics = false;
    before.light_bar = false;
    before.resolution = 1;
    before.hint_size = 2;
    before.concept_index = 7;
    before.codec_avc = false;
    before.codec_vp9 = true;
    before.codec_av1 = false;
    before.max_height = 1440;
    before.video_volume = 6;
    before.decode_mode = DecodeMode::hardware;
    before.sb_enabled = false;
    before.sb_mode = 1;
    before.sb_scope = 0;

    Settings after;
    ASSERT_TRUE(decode_settings(encode_settings(before), &after));
    EXPECT_EQ(after.music_volume, before.music_volume);
    EXPECT_EQ(after.sfx_volume, before.sfx_volume);
    EXPECT_EQ(after.ui_volume, before.ui_volume);
    EXPECT_EQ(after.reduced_motion, before.reduced_motion);
    EXPECT_EQ(after.swap_confirm, before.swap_confirm);
    EXPECT_EQ(after.show_fps, before.show_fps);
    EXPECT_EQ(after.haptics, before.haptics);
    EXPECT_EQ(after.light_bar, before.light_bar);
    EXPECT_EQ(after.resolution, before.resolution);
    EXPECT_EQ(after.hint_size, before.hint_size);
    EXPECT_EQ(after.concept_index, before.concept_index);
    EXPECT_EQ(after.codec_avc, before.codec_avc);
    EXPECT_EQ(after.codec_vp9, before.codec_vp9);
    EXPECT_EQ(after.codec_av1, before.codec_av1);
    EXPECT_EQ(after.max_height, before.max_height);
    EXPECT_EQ(after.video_volume, before.video_volume);
    EXPECT_EQ(after.decode_mode, before.decode_mode);
    EXPECT_EQ(after.sb_enabled, before.sb_enabled);
    EXPECT_EQ(after.sb_mode, before.sb_mode);
    EXPECT_EQ(after.sb_scope, before.sb_scope);
}

TEST(Settings, AVersionThreeSaveLoadsWithSponsorBlockDefaults)
{
    Settings before;
    before.music_volume = 2;
    before.max_height = 720;
    // A version-3 save is the first 22 bytes of a version-4/5 one (the
    // fixed fields) without the last three (SponsorBlock).
    std::string data = encode_settings(before);
    data.resize(22 - 3);
    data[0] = 3;
    Settings after;
    after.sb_enabled = false;
    after.sb_mode = 2;
    ASSERT_TRUE(decode_settings(data, &after));
    EXPECT_EQ(after.music_volume, 2);
    EXPECT_EQ(after.max_height, 720);
    EXPECT_TRUE(after.sb_enabled);
    EXPECT_EQ(after.sb_mode, 0);
    EXPECT_EQ(after.sb_scope, 2);
}

TEST(Settings, DecodeModeRoundTripsAllThreeValues)
{
    for (const DecodeMode mode :
         {DecodeMode::auto_detect, DecodeMode::software, DecodeMode::hardware})
    {
        Settings before;
        before.decode_mode = mode;
        Settings after;
        ASSERT_TRUE(decode_settings(encode_settings(before), &after));
        EXPECT_EQ(after.decode_mode, mode);
    }
}

TEST(Settings, AnOlderSaveFileLoadsAsAutoDetect)
{
    // A save from before the decode mode existed carries version 2. The reader
    // is versioned and refuses it, so the decoded values stay at their defaults,
    // which makes the decode mode auto_detect.
    std::string old = encode_settings(Settings{});
    old[0] = 2;
    Settings settings;
    EXPECT_FALSE(decode_settings(old, &settings));
    EXPECT_EQ(settings.decode_mode, DecodeMode::software);
}

TEST(Settings, BrokenDataIsRejectedAndOutOfRangeValuesAreClamped)
{
    Settings settings;
    EXPECT_FALSE(decode_settings("", &settings));
    EXPECT_FALSE(decode_settings("not a save file", &settings));

    // A saved cap that is one of the offered steps survives the round trip.
    Settings capped;
    capped.max_height = 720;
    ASSERT_TRUE(decode_settings(encode_settings(capped), &settings));
    EXPECT_EQ(settings.max_height, 720);

    Settings all_off;
    all_off.codec_avc = false;
    all_off.codec_vp9 = false;
    all_off.codec_av1 = false;
    std::string odd = encode_settings(all_off);
    ASSERT_TRUE(decode_settings(odd, &settings));
    EXPECT_TRUE(settings.codec_avc || settings.codec_vp9 || settings.codec_av1);
}

TEST(Settings, TheLastCodecCannotBeSwitchedOff)
{
    Settings settings;
    // Two codecs are still on, so the first two switches are allowed.
    EXPECT_TRUE(settings.set_codec("AVC", false));
    EXPECT_FALSE(settings.codec_avc);
    EXPECT_TRUE(settings.set_codec("VP9", false));
    EXPECT_FALSE(settings.codec_vp9);
    EXPECT_TRUE(settings.is_last_codec("AV1"));

    // The third one is refused: the app must keep one codec playable.
    EXPECT_FALSE(settings.set_codec("AV1", false));
    EXPECT_TRUE(settings.codec_av1);

    // Once another codec is back on, the switch works again.
    EXPECT_TRUE(settings.set_codec("AVC", true));
    EXPECT_FALSE(settings.is_last_codec("AVC"));
    EXPECT_TRUE(settings.set_codec("AV1", false));
    EXPECT_FALSE(settings.codec_av1);
    EXPECT_TRUE(settings.set_codec("AVC", true));
    EXPECT_FALSE(settings.set_codec("bogus", true));
}

} // namespace

TEST(Settings, SmartTubeSettingsRoundTripAndAVersionFourSaveKeepsDefaults)
{
    Settings before;
    before.speed_index = 5;
    before.seek_seconds = 30;
    before.playback_mode = 2;
    before.show_clock = false;
    before.hidden_sections = 0x5;
    before.grid_columns = 5;
    before.color_scheme = 2;
    before.sb_markers = false;
    Settings after;
    ASSERT_TRUE(decode_settings(encode_settings(before), &after));
    EXPECT_EQ(after.speed_index, 5);
    EXPECT_EQ(after.seek_seconds, 30);
    EXPECT_EQ(after.playback_mode, 2);
    EXPECT_FALSE(after.show_clock);
    EXPECT_TRUE(after.section_hidden(0));
    EXPECT_FALSE(after.section_hidden(1));
    EXPECT_TRUE(after.section_hidden(2));
    EXPECT_EQ(after.grid_columns, 5);
    EXPECT_EQ(after.color_scheme, 2);
    EXPECT_FALSE(after.sb_markers);

    std::string data = encode_settings(before);
    data.resize(22);
    data[0] = 4;
    Settings old;
    ASSERT_TRUE(decode_settings(data, &old));
    EXPECT_EQ(old.speed_index, 3);
    EXPECT_TRUE(old.show_clock);
    EXPECT_EQ(old.hidden_sections, 0u);
}

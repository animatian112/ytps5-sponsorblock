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

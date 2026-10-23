// ps5-homebrew-ui - Player settings and their save format.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/settings.hpp"

#include "core/bytes.hpp"

#include <algorithm>

namespace hui
{

namespace
{
constexpr std::uint8_t kVersion = 5;
constexpr std::uint8_t kVersionWithoutExtras = 4;
constexpr std::uint8_t kVersionWithoutSponsorBlock = 3;

// Version 5 appends SmartTube's settings as (key, value) pairs: a key this
// build does not know is skipped, a missing one keeps its default, so later
// settings need no new version. Keys are never reused.
struct Extra
{
    std::uint8_t key;
    int Settings::*number; // or
    bool Settings::*flag;
    int low;
    int high;
};

constexpr Extra kExtras[] = {
    {1, &Settings::speed_index, nullptr, 0, Settings::kSpeedCount - 1},
    {2, nullptr, &Settings::remember_speed, 0, 1},
    {3, &Settings::seek_seconds, nullptr, 5, 60},
    {4, &Settings::ui_hide_seconds, nullptr, 0, 30},
    {5, &Settings::playback_mode, nullptr, 0, 4},
    {6, nullptr, &Settings::show_clock, 0, 1},
    {7, nullptr, &Settings::show_remaining, 0, 1},
    {8, nullptr, &Settings::show_quality, 0, 1},
    {9, nullptr, &Settings::show_tooltips, 0, 1},
    {10, nullptr, &Settings::suggestions, 0, 1},
    {11, nullptr, &Settings::loop_shorts, 0, 1},
    {12, nullptr, &Settings::resume, 0, 1},
    {13, nullptr, &Settings::resume_short, 0, 1},
    {14, nullptr, &Settings::prefer_avc, 0, 1},
    {15, &Settings::ok_behavior, nullptr, 0, 2},
    {16, &Settings::subtitle_language, nullptr, 0, Settings::kSubtitleLanguageCount - 1},
    {17, &Settings::subtitle_size, nullptr, 0, 2},
    {18, nullptr, &Settings::subtitle_background, 0, 1},
    {19, &Settings::boot_section, nullptr, 0, Settings::kBootSectionCount - 1},
    // 20: hidden_sections (unsigned, written apart below)
    {21, nullptr, &Settings::hide_shorts_home, 0, 1},
    {22, nullptr, &Settings::hide_shorts_subscriptions, 0, 1},
    {23, nullptr, &Settings::hide_shorts_history, 0, 1},
    {24, nullptr, &Settings::hide_shorts_search, 0, 1},
    {25, nullptr, &Settings::hide_watched_home, 0, 1},
    {26, nullptr, &Settings::hide_watched_subscriptions, 0, 1},
    {27, nullptr, &Settings::hide_streams_subscriptions, 0, 1},
    {28, nullptr, &Settings::hide_mixes, 0, 1},
    {29, nullptr, &Settings::history_enabled, 0, 1},
    {30, nullptr, &Settings::time_24h, 0, 1},
    {31, &Settings::dim_minutes, nullptr, 0, 60},
    {32, nullptr, &Settings::corner_clock, 0, 1},
    {33, &Settings::title_lines, nullptr, 1, 3},
    {34, &Settings::grid_columns, nullptr, 3, 5},
    {35, nullptr, &Settings::rounded_cards, 0, 1},
    {36, &Settings::color_scheme, nullptr, 0, Settings::kColorSchemeCount - 1},
    {37, nullptr, &Settings::keyboard_auto, 0, 1},
    {38, nullptr, &Settings::search_history, 0, 1},
    {39, nullptr, &Settings::sb_markers, 0, 1},
    {40, nullptr, &Settings::sb_once, 0, 1},
    {41, &Settings::sb_min_seconds, nullptr, 0, 10},
    {42, nullptr, &Settings::sb_alt_server, 0, 1},
    {43, nullptr, &Settings::sb_stats, 0, 1},
    {44, &Settings::video_zoom, nullptr, 0, 2},
    {45, &Settings::subtitle_color, nullptr, 0, 2},
    {46, nullptr, &Settings::player_double_back, 0, 1},
    {47, &Settings::region, nullptr, 0, Settings::kRegionCount - 1},
    {48, nullptr, &Settings::remote_control, 0, 1},
    {49, &Settings::search_sort, nullptr, 0, 3},
    {50, &Settings::search_date, nullptr, 0, 5},
    {51, &Settings::search_duration, nullptr, 0, 3},
    {52, &Settings::video_brightness, nullptr, 0, 100},
    {53, nullptr, &Settings::dev_stats, 0, 1},
    {54, nullptr, &Settings::dev_log, 0, 1},
    {55, &Settings::dev_threads, nullptr, 0, 8},
    {56, &Settings::dev_convert_threads, nullptr, 1, 8},
    {57, &Settings::dev_deblock, nullptr, 0, 2},
    {58, nullptr, &Settings::dev_auto_quality, 0, 1},
    {59, &Settings::dev_av_offset_ms, nullptr, -500, 500},
    // 60..69: sb_act (below).
    {80, &Settings::dev_upload, nullptr, 0, 5},
    {81, nullptr, &Settings::dearrow_titles, 0, 1},
    {82, nullptr, &Settings::dearrow_thumbnails, 0, 1},
};
// 60 + i: Settings::sb_act[i].
constexpr std::uint8_t kSbActionKey = 60;
constexpr std::uint8_t kHiddenSectionsKey = 20;

} // namespace

std::string encode_settings(const Settings &settings)
{
    bytes::Writer w;
    w.put(kVersion);
    w.put(static_cast<std::uint8_t>(settings.music_volume));
    w.put(static_cast<std::uint8_t>(settings.sfx_volume));
    w.put(static_cast<std::uint8_t>(settings.ui_volume));
    w.put_bool(settings.reduced_motion);
    w.put_bool(settings.swap_confirm);
    w.put_bool(settings.show_fps);
    w.put_bool(settings.haptics);
    w.put_bool(settings.light_bar);
    w.put(static_cast<std::uint8_t>(settings.resolution));
    w.put(static_cast<std::uint8_t>(settings.hint_size));
    w.put(static_cast<std::uint8_t>(settings.concept_index));
    w.put_bool(settings.codec_avc);
    w.put_bool(settings.codec_vp9);
    w.put_bool(settings.codec_av1);
    w.put(static_cast<std::uint16_t>(settings.max_height));
    w.put(static_cast<std::uint8_t>(settings.video_volume));
    w.put(static_cast<std::uint8_t>(settings.decode_mode));
    w.put_bool(settings.sb_enabled);
    w.put(static_cast<std::uint8_t>(settings.sb_mode));
    w.put(static_cast<std::uint8_t>(settings.sb_scope));
    constexpr std::size_t count = sizeof(kExtras) / sizeof(kExtras[0]);
    w.put(static_cast<std::uint16_t>(count + 1 + Settings::kSbCategories));
    for (const Extra &extra : kExtras)
    {
        w.put(extra.key);
        const int value =
            extra.number != nullptr ? settings.*extra.number : (settings.*extra.flag ? 1 : 0);
        w.put(static_cast<std::int32_t>(value));
    }
    w.put(kHiddenSectionsKey);
    w.put(static_cast<std::int32_t>(settings.hidden_sections));
    for (int i = 0; i < Settings::kSbCategories; ++i)
    {
        w.put(static_cast<std::uint8_t>(kSbActionKey + i));
        w.put(static_cast<std::int32_t>(settings.sb_act[i]));
    }
    return w.data();
}

bool decode_settings(std::string_view data, Settings *settings)
{
    bytes::Reader r(data);
    const std::uint8_t version = r.get<std::uint8_t>();
    if (version != kVersion && version != kVersionWithoutExtras &&
        version != kVersionWithoutSponsorBlock)
        return false;
    Settings s;
    s.music_volume = std::clamp<int>(r.get<std::uint8_t>(), 0, 10);
    s.sfx_volume = std::clamp<int>(r.get<std::uint8_t>(), 0, 10);
    s.ui_volume = std::clamp<int>(r.get<std::uint8_t>(), 0, 10);
    s.reduced_motion = r.get_bool();
    s.swap_confirm = r.get_bool();
    s.show_fps = r.get_bool();
    s.haptics = r.get_bool();
    s.light_bar = r.get_bool();
    s.resolution = std::clamp<int>(r.get<std::uint8_t>(), 0, Settings::kResolutionCount - 1);
    s.hint_size = std::clamp<int>(r.get<std::uint8_t>(), 0, 2);
    s.concept_index = r.get<std::uint8_t>();
    s.codec_avc = r.get_bool();
    s.codec_vp9 = r.get_bool();
    s.codec_av1 = r.get_bool();
    s.max_height = r.get<std::uint16_t>();
    if (s.max_height != 720 && s.max_height != 1080 && s.max_height != 1440 &&
        s.max_height != 2160 && s.max_height != 4320)
        s.max_height = 1080;
    if (!s.codec_avc && !s.codec_vp9 && !s.codec_av1)
        s.codec_avc = true;
    s.video_volume = std::clamp<int>(r.get<std::uint8_t>(), 0, 10);
    {
        const int mode = r.get<std::uint8_t>();
        s.decode_mode = mode <= static_cast<int>(yt::DecodeMode::hardware)
                            ? static_cast<yt::DecodeMode>(mode)
                            : yt::DecodeMode::software;
    }
    if (version >= 4)
    {
        s.sb_enabled = r.get_bool();
        s.sb_mode = std::clamp<int>(r.get<std::uint8_t>(), 0, 2);
        s.sb_scope = std::clamp<int>(r.get<std::uint8_t>(), 0, 2);
    }
    if (version >= 5)
    {
        const int count = r.get<std::uint16_t>();
        for (int i = 0; i < count && r.ok(); ++i)
        {
            const std::uint8_t key = r.get<std::uint8_t>();
            const int value = static_cast<int>(r.get<std::int32_t>());
            if (key >= kSbActionKey && key < kSbActionKey + Settings::kSbCategories)
            {
                s.sb_act[key - kSbActionKey] = std::clamp(value, 0, 4);
                continue;
            }
            if (key == kHiddenSectionsKey)
            {
                s.hidden_sections =
                    static_cast<std::uint32_t>(value) & ((1u << Settings::kSectionCount) - 1u);
                continue;
            }
            for (const Extra &extra : kExtras)
            {
                if (extra.key != key)
                    continue;
                const int clamped = std::clamp(value, extra.low, extra.high);
                if (extra.number != nullptr)
                    s.*extra.number = clamped;
                else
                    s.*extra.flag = clamped != 0;
                break;
            }
        }
    }
    if (!r.finished())
        return false;
    *settings = s;
    return true;
}

bool Settings::codec_on(const char *id) const
{
    if (id == nullptr)
        return false;
    if (std::string_view(id) == "AVC")
        return codec_avc;
    if (std::string_view(id) == "VP9")
        return codec_vp9;
    if (std::string_view(id) == "AV1")
        return codec_av1;
    return false;
}

bool Settings::is_last_codec(const char *id) const
{
    const int enabled = (codec_avc ? 1 : 0) + (codec_vp9 ? 1 : 0) + (codec_av1 ? 1 : 0);
    return enabled <= 1 && codec_on(id);
}

bool Settings::set_codec(const char *id, bool on)
{
    if (id == nullptr)
        return false;
    if (!on && is_last_codec(id))
        return false;
    if (std::string_view(id) == "AVC")
        codec_avc = on;
    else if (std::string_view(id) == "VP9")
        codec_vp9 = on;
    else if (std::string_view(id) == "AV1")
        codec_av1 = on;
    else
        return false;
    return true;
}

} // namespace hui

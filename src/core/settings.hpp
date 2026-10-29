// ps5-homebrew-ui - Player settings and their save format.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "yt/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace hui
{

// Everything the settings screen edits. The values are live: the app applies
// them the frame they change (see Shell::take_settings_changed) and stores
// them when the screen is left.
struct Settings
{
    int music_volume = 4; // 0..10
    int sfx_volume = 8;   // 0..10
    int ui_volume = 7;    // 0..10
    bool reduced_motion = false;
    bool swap_confirm = false; // Circle confirms, Cross goes back
    bool show_fps = false;
    bool haptics = true;   // controller rumble on errors and big moments
    bool light_bar = true; // the controller's light follows the design's accent
    // Display resolution, an index into kResolutions; applies at the next start.
    int resolution = 2;
    // YouTube playback: which stream codecs may play, the resolution cap of
    // the adaptive format and the video volume (0..10).
    bool codec_avc = true;
    bool codec_vp9 = true;
    bool codec_av1 = true;
    int max_height = 1080; // 720, 1080, 1440 or 2160
    int video_volume = 10;
    // Which decoder plays the selected stream (see yt::DecodeMode). Stored as a
    // byte, like the codec switches; an older save without it loads as the
    // default through the versioned reader.
    // Software by default: the console decoder's argument layouts are still
    // unverified research (docs/HARDWARE_DECODE.md), so Auto/Hardware are
    // opt-in.
    yt::DecodeMode decode_mode = yt::DecodeMode::software;
    // SponsorBlock (yt/sponsorblock.hpp): on/off, what happens when a segment
    // starts (0 skip + toast, 1 ask, 2 only mark it) and which categories are
    // fetched (0 sponsors, 1 + self-promotion/reminders, 2 everything but filler).
    // Added in save version 4; a version-3 save loads with these defaults.
    bool sb_enabled = true;
    int sb_mode = 0;
    int sb_scope = 2;
    // How large on-screen controller hints are: 0 small, 1 medium, 2 large.
    int hint_size = 1;
    // The design shown when the app opens (index into the concept list).
    int concept_index = 0;

    // ---- SmartTube's settings (save version 5, stored as key/value pairs so
    // later additions need no new version) ----
    // Player
    int speed_index = 3;        // default speed, index into kSpeeds (1x)
    bool remember_speed = true; // keep a changed speed for the next video
    int seek_seconds = 10;      // left/right on the seek bar and L1/R1
    int ui_hide_seconds = 4;    // controls hide after this long (0: never)
    int playback_mode = 0;      // at the end: 0 next, 1 repeat, 2 pause, 3 close, 4 shuffle
    bool show_clock = true;     // the clock in the controls
    bool show_remaining = true; // "Remaining: 4:21" in the controls
    bool show_quality = true;   // "1080p" in the controls
    bool show_tooltips = true;  // the name of the focused button
    bool suggestions = true;    // the suggestions row under the controls
    bool loop_shorts = false;   // a Short starts again instead of moving on
    bool resume = true;         // continue where a video was left
    bool resume_short = false;  // ... also videos under 5 minutes
    bool prefer_avc = false;    // AVC before VP9/AV1 (1080p at most)
    int ok_behavior = 0;        // Cross while hidden: 0 controls, 1 pause, 2 both
    // Subtitles
    int subtitle_language = 0; // index into kSubtitleLanguages (0: off)
    int subtitle_size = 1;     // 0 small, 1 medium, 2 large
    bool subtitle_background = true;
    // General
    int boot_section = 0;              // index into kBootSections
    std::uint32_t hidden_sections = 0; // bit per kSectionNames entry
    bool hide_shorts_home = false;
    bool hide_shorts_subscriptions = false;
    bool hide_shorts_history = false;
    bool hide_shorts_search = false;
    bool hide_watched_home = false;
    bool hide_watched_subscriptions = false;
    bool hide_streams_subscriptions = false;
    bool hide_mixes = false;
    bool history_enabled = true; // tell YouTube what was watched
    bool time_24h = false;
    int dim_minutes = 0; // dim the screen after this idle time (0: never)
    // User interface
    bool corner_clock = true; // the clock top right of the sections
    int title_lines = 2;      // card title lines, 1..3
    int grid_columns = 4;     // cards a row in the grids, 3..5
    bool rounded_cards = true;
    int ui_layout = 0;    // 0 SmartTube, 1 Glow (the focused video behind a large header)
    int color_scheme = 0; // index into kColorSchemes
    // Search
    bool keyboard_auto = true;   // the keyboard opens with the search
    bool system_keyboard = true; // type with the PlayStation's own keyboard dialog
    bool search_history = true;  // keep the last searches as suggestions
    // SponsorBlock
    bool sb_markers = true; // the segments on the seek bar
    bool sb_once = false;   // a segment skipped once plays the next time
    // SmartTube's per-category actions, in yt::sponsorblock order (sponsor,
    // self promotion, interaction, intro, outro, preview, non-music, filler,
    // highlight, exclusive access): 0 off, 1 skip, 2 skip + notify, 3 ask,
    // 4 show only.
    static constexpr int kSbCategories = 10;
    int sb_act[kSbCategories] = {2, 2, 2, 2, 2, 2, 2, 0, 4, 4};
    int sb_min_seconds = 1;     // segments shorter than this are not skipped
    bool sb_alt_server = false; // api.sponsor.ajay.app
    // DeArrow (SmartTube's): community titles and thumbnails on the cards.
    bool dearrow_titles = false;
    bool dearrow_thumbnails = false;
    bool sb_stats = true; // tell the database which segments were skipped
    // More of SmartTube's options.
    int video_zoom = 0;              // 0 fit, 1 fill (crop the bars), 2 stretch
    int subtitle_color = 0;          // 0 white, 1 yellow, 2 cyan
    bool player_double_back = false; // Circle twice leaves the player
    bool remote_control = true;      // Cast from the phone (Link with TV code)
    // Search filters (yt::search_params): sort, upload date, duration.
    int search_sort = 0;     // 0 relevance, 1 upload date, 2 view count, 3 rating
    int search_date = 0;     // 0 any, 1 hour, 2 today, 3 week, 4 month, 5 year
    int search_duration = 0; // 0 any, 1 under 4 min, 2 4-20 min, 3 over 20 min
    // The player's brightness, 100 (no dimming) .. 0 (a black screen).
    int video_brightness = 100;
    // Settings > Developer.
    bool dev_stats = false;       // stats for nerds over the video
    bool dev_log = false;         // the log's last lines on the player and Home
    int dev_threads = 0;          // decoder threads, 0 automatic
    int dev_convert_threads = 4;  // RGBA conversion threads
    int dev_deblock = 1;          // skip the deblocking filter: 0 never, 1 above 1080p, 2 always
    bool dev_auto_quality = true; // step down to 1080p when a higher quality cannot keep up
    int dev_av_offset_ms = 0;     // picture later (+) or earlier (-) than the sound
    int dev_upload = 0;           // gfx::UploadMethod for video pictures; 0 measures once
    bool dev_gpu_convert = false; // hardware pictures go to the GPU as NV12 (no CPU conversion)
    int dev_hw_depth = 1;         // pictures the hardware decoder works on at once (1 or 2)
    int region = 0;               // index into kRegions: the feeds' country and language
    static constexpr int kRegionCount = 14;
    static constexpr const char *kRegions[kRegionCount][3] = {
        {"US", "en", "United States"}, {"GB", "en", "United Kingdom"}, {"CA", "en", "Canada"},
        {"AU", "en", "Australia"},     {"IN", "en", "India"},          {"DE", "de", "Germany"},
        {"FR", "fr", "France"},        {"ES", "es", "Spain"},          {"IT", "it", "Italy"},
        {"BR", "pt", "Brazil"},        {"MX", "es", "Mexico"},         {"JP", "ja", "Japan"},
        {"KR", "ko", "South Korea"},   {"NL", "nl", "Netherlands"}};

    static constexpr int kSpeedCount = 8;
    static constexpr double kSpeeds[kSpeedCount] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0};
    static constexpr int kSubtitleLanguageCount = 10;
    static constexpr const char *kSubtitleLanguages[kSubtitleLanguageCount][2] = {
        {"", "Off"},      {"en", "English"},    {"es", "Spanish"}, {"fr", "French"},
        {"de", "German"}, {"pt", "Portuguese"}, {"it", "Italian"}, {"ja", "Japanese"},
        {"ko", "Korean"}, {"ru", "Russian"}};
    // The sections the sidebar can hide, in its order.
    static constexpr int kSectionCount = 12;
    static constexpr const char *kSectionNames[kSectionCount] = {
        "Shorts",   "Sports",        "Live",    "Gaming",    "News",      "Music",
        "Channels", "Subscriptions", "History", "Playlists", "My videos", "Blocked channels"};
    static constexpr int kBootSectionCount = 5;
    static constexpr const char *kBootSections[kBootSectionCount] = {
        "Home", "Subscriptions", "Shorts", "History", "Playlists"};
    static constexpr int kColorSchemeCount = 4;
    static constexpr const char *kColorSchemes[kColorSchemeCount] = {"Dark Grey", "Dark", "Red",
                                                                     "Blue Grey"};
    bool section_hidden(int index) const
    {
        return index >= 0 && index < kSectionCount && ((hidden_sections >> index) & 1u) != 0;
    }
    double speed() const
    {
        return kSpeeds[speed_index >= 0 && speed_index < kSpeedCount ? speed_index : 3];
    }

    struct Resolution
    {
        int width;
        int height;
        const char *label;
    };
    static constexpr int kResolutionCount = 3;
    static constexpr Resolution kResolutions[kResolutionCount] = {
        {1920, 1080, "1080p"}, {2560, 1440, "1440p"}, {3840, 2160, "4K"}};

    static float gain(int volume)
    {
        // Perceptual curve: 0..10 steps map to roughly -40..0 dB.
        const float t = static_cast<float>(volume) / 10.0f;
        return t * t;
    }

    // The three codec switches by name ("AVC", "VP9", "AV1"); the settings
    // screen and the tests use them, the player reads the fields.
    bool codec_on(const char *id) const;
    // True when switching this codec off would leave none enabled.
    bool is_last_codec(const char *id) const;
    // Refuses to switch the last enabled codec off; false when it did not
    // change anything.
    bool set_codec(const char *id, bool on);
};

std::string encode_settings(const Settings &settings);
// Out-of-range values are clamped. Returns false (leaving settings
// untouched) for malformed data or an unknown version.
bool decode_settings(std::string_view data, Settings *settings);

} // namespace hui

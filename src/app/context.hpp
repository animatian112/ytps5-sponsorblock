// youtube-tv - What a screen receives: fonts, settings, the worker service,
// thumbnails, toasts and the way to ask the app for something.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/resume_store.hpp"
#include "app/telemetry.hpp"
#include "core/settings.hpp"
#include "ui/fonts.hpp"
#include "yt/types.hpp"

#include <string>
#include <vector>

namespace yt
{
class Service;
class Lounge;
} // namespace yt

namespace hui::gfx
{
class GlBatch;
}

namespace hui
{
namespace ui
{
class ToastStack;
}
namespace app
{

class ThumbCache;

// What a screen asks the app to do; the app clears it after every update.
struct Nav
{
    bool open_video = false;   // start playing video_id on the player screen
    bool open_channel = false; // show context.pending_channel on the search screen
    std::string video_id;
    bool leave_player = false;  // close the player and go back to the tab
    bool leave_browse = false;  // close a playlist page and go back to the tab
    bool leave_channel = false; // close a channel page opened from elsewhere
    bool go_to = false;         // switch to `screen`
    int screen_index = 0;       // index into App::screen_count()
    bool start_login = false;
    bool cancel_login = false;
    bool sign_out = false;
    bool install_update = false; // Settings > Updates > Install update
    bool add_account = false;    // keep this account, sign in one more
    int switch_account = -1;     // index into Service::saved_accounts()
    // "Add to playlist" from the player: the app's playlist picker for this video.
    bool add_to_playlist = false;
    yt::FeedItem playlist_item;
    bool backup_settings = false;  // Settings > General > Back up settings
    bool restore_settings = false; // ... and Restore
    bool new_tv_code = false;      // Settings > Remote control > New TV code
    bool reset_settings = false;   // Settings > Developer > Reset all settings
    bool quit = false;             // leave the app
};

} // namespace app
} // namespace hui

namespace hui::app
{

// Cast from the phone: what the linked phone asked for (the app fills it
// in, the player screen takes it), and what the player reports back.
struct Remote
{
    // Asked: -1 nothing; set by the app, cleared by the player.
    std::string start_video; // the next request_video of this id starts at start_s
    double start_s = -1.0;
    int paused = -1; // 0 play, 1 pause
    double seek_s = -1.0;
    bool next = false;
    bool previous = false;
    // Reported by the player every frame (state: 1 playing, 2 paused,
    // 3 buffering, 0 ended, -1 nothing open).
    std::string video;
    double position_s = 0.0;
    double duration_s = 0.0;
    int state = -1;
};

struct Context
{
    const ui::Fonts &fonts;
    const Telemetry &telemetry;
    Settings &settings;
    bool settings_changed = false;
    ThumbCache &thumbs;
    // For screens that own a texture of their own (the player's video frame).
    gfx::GlBatch &batch;
    yt::Service &service;
    ui::ToastStack &toasts;
    Nav &nav;
    // The video the player screen should be playing (empty: none). The app
    // keeps it across frames; the player screen reads it every frame.
    std::string &active_video;
    // Where each video was left (the player continues there).
    ResumeStore &resume;
    // A channel to show (the player's Channel button): the search screen opens
    // its page when it enters, then clears it.
    yt::ChannelItem &pending_channel;
    // The playlist ("VL...") the browse screen shows when it enters.
    std::string &pending_browse;
    // "Play next": videos the player plays before its suggestions, in order.
    std::vector<yt::FeedItem> &play_queue;
    // The section the playing video was opened from (Home, a playlist, a
    // channel...): what Shuffle picks from.
    std::vector<yt::FeedItem> &section;
    // Channels SponsorBlock leaves alone ("Exclude this channel"), by id; set
    // sb_excluded_changed after editing so the app stores the list.
    std::vector<std::string> &sb_excluded;
    bool sb_excluded_changed = false;
    Remote remote;
    // The remote control's TV code and status for Settings (null: none).
    const yt::Lounge *lounge = nullptr;
    // Buffer textures work here (the player screen checks once): the
    // hardware path may then hand over NV12 pictures for the GPU to convert.
    bool video_buffers_ok = false;
    // The section above, when it plays in order (a playlist, or a channel's
    // row such as Popular): its name for the player's row, the feed it was
    // taken from (-1: none) and the row inside that feed (a channel page's
    // shelf). section_revision changes whenever the section is replaced.
    bool section_ordered = false;
    std::string section_title;
    int section_kind = -1;
    std::string section_shelf;
    std::uint64_t section_revision = 0;
    // The name of the playlist pending_browse opens (its card's title).
    std::string pending_browse_title;
};

} // namespace hui::app

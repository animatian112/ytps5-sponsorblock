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

namespace yt
{
class Service;
}

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
    bool leave_player = false; // close the player and go back to the tab
    bool go_to = false;        // switch to `screen`
    int screen_index = 0;      // index into App::screen_count()
    bool start_login = false;
    bool cancel_login = false;
    bool sign_out = false;
    bool quit = false; // leave the app
};

} // namespace app
} // namespace hui

namespace hui::app
{

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
};

} // namespace hui::app

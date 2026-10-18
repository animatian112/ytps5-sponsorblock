// youtube-tv - The screen factories, in the app's screen order:
// 0 Home, 1 Search, 2 Subscriptions, 3 Settings, 4 Login, 5 Player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/context.hpp"
#include "app/screen.hpp"

#include <memory>

namespace hui::app
{

// Screen indices the app assumes elsewhere; keep the order in sync.
// SmartTube's rail order: Search on top, then Home, Subscriptions, History,
// and Settings last.
inline constexpr std::size_t kSearchScreen = 0;
inline constexpr std::size_t kHomeScreen = 1;
// The sections in SmartTube's order: Shorts, the topic channels, Channels.
inline constexpr std::size_t kShortsScreen = 2;
inline constexpr std::size_t kSportsScreen = 3;
inline constexpr std::size_t kLiveScreen = 4;
inline constexpr std::size_t kGamingScreen = 5;
inline constexpr std::size_t kNewsScreen = 6;
inline constexpr std::size_t kMusicScreen = 7;
inline constexpr std::size_t kChannelsScreen = 8;
inline constexpr std::size_t kSubscriptionsScreen = 9;
inline constexpr std::size_t kHistoryScreen = 10;
inline constexpr std::size_t kPlaylistsScreen = 11;
inline constexpr std::size_t kSettingsScreen = 12;
inline constexpr std::size_t kLoginScreen = 13;
inline constexpr std::size_t kPlayerScreen = 14;
// One playlist's videos (opened from a playlist card), not a tab.
inline constexpr std::size_t kBrowseScreen = 15;
// A channel's page (opened from any channel card or button), not a tab.
inline constexpr std::size_t kChannelScreen = 16;
inline constexpr std::size_t kTabCount = 13;

std::unique_ptr<Screen> make_home_screen(Context &context);
std::unique_ptr<Screen> make_search_screen(Context &context);
std::unique_ptr<Screen> make_subscriptions_screen(Context &context);
std::unique_ptr<Screen> make_history_screen(Context &context);
std::unique_ptr<Screen> make_playlists_screen(Context &context);
std::unique_ptr<Screen> make_browse_screen(Context &context);
std::unique_ptr<Screen> make_channel_screen(Context &context);
std::unique_ptr<Screen> make_topic_screen(Context &context, const char *id, const char *title,
                                          const char *browse_id);
std::unique_ptr<Screen> make_settings_screen(Context &context);
std::unique_ptr<Screen> make_login_screen(Context &context);
std::unique_ptr<Screen> make_player_screen(Context &context);

} // namespace hui::app

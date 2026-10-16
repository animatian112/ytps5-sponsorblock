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
inline constexpr std::size_t kSubscriptionsScreen = 2;
inline constexpr std::size_t kHistoryScreen = 3;
// The topic sections (YouTube's topic channels), as in SmartTube's rail.
inline constexpr std::size_t kMusicScreen = 4;
inline constexpr std::size_t kGamingScreen = 5;
inline constexpr std::size_t kNewsScreen = 6;
inline constexpr std::size_t kLiveScreen = 7;
inline constexpr std::size_t kSportsScreen = 8;
inline constexpr std::size_t kSettingsScreen = 9;
inline constexpr std::size_t kLoginScreen = 10;
inline constexpr std::size_t kPlayerScreen = 11;
inline constexpr std::size_t kTabCount = 10;

std::unique_ptr<Screen> make_home_screen(Context &context);
std::unique_ptr<Screen> make_search_screen(Context &context);
std::unique_ptr<Screen> make_subscriptions_screen(Context &context);
std::unique_ptr<Screen> make_history_screen(Context &context);
std::unique_ptr<Screen> make_topic_screen(Context &context, const char *id, const char *title,
                                          const char *browse_id);
std::unique_ptr<Screen> make_settings_screen(Context &context);
std::unique_ptr<Screen> make_login_screen(Context &context);
std::unique_ptr<Screen> make_player_screen(Context &context);

} // namespace hui::app

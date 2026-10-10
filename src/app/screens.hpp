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
inline constexpr std::size_t kHomeScreen = 0;
inline constexpr std::size_t kSearchScreen = 1;
inline constexpr std::size_t kSubscriptionsScreen = 2;
inline constexpr std::size_t kSettingsScreen = 3;
inline constexpr std::size_t kLoginScreen = 4;
inline constexpr std::size_t kPlayerScreen = 5;
inline constexpr std::size_t kTabCount = 4;

std::unique_ptr<Screen> make_home_screen(Context &context);
std::unique_ptr<Screen> make_search_screen(Context &context);
std::unique_ptr<Screen> make_subscriptions_screen(Context &context);
std::unique_ptr<Screen> make_settings_screen(Context &context);
std::unique_ptr<Screen> make_login_screen(Context &context);
std::unique_ptr<Screen> make_player_screen(Context &context);

} // namespace hui::app

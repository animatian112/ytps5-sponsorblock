// youtube-tv - The console's own keyboard, when the platform has one.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/system_keyboard.hpp"

namespace hui::app
{

namespace
{
SystemKeyboard *g_keyboard = nullptr;
} // namespace

void set_system_keyboard(SystemKeyboard *keyboard)
{
    g_keyboard = keyboard;
}

SystemKeyboard *system_keyboard()
{
    return g_keyboard;
}

} // namespace hui::app

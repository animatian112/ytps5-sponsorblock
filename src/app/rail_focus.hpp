// youtube-tv - Where the focus goes when Cross picks a sidebar entry.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cross on a section in the sidebar shows it and keeps the sidebar, so the
// player can walk Home -> Subscriptions -> Settings without L1/R1. Search is
// different: nobody opens it to look at it, they open it to type. Keeping the
// sidebar there meant Cross on Search, then Circle (or Right) to leave the
// sidebar, and only then the keyboard took the presses (issue #22).

#pragma once

#include <cstddef>

namespace hui::app
{

// True when Cross on the sidebar entry for `screen` should also hand the
// focus to that screen, so its first input (the keyboard, or the search
// field) takes the next press. Only Search does; every other section keeps
// the sidebar focused.
bool rail_confirm_enters_content(std::size_t screen);

} // namespace hui::app

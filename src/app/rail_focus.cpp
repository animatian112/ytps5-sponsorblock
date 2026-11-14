// youtube-tv - Where the focus goes when Cross picks a sidebar entry.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/rail_focus.hpp"

#include "app/screens.hpp"

namespace hui::app
{

bool rail_confirm_enters_content(std::size_t screen)
{
    return screen == kSearchScreen;
}

} // namespace hui::app

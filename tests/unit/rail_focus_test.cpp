// youtube-tv - Where the focus goes when Cross picks a sidebar entry.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/rail_focus.hpp"
#include "app/screens.hpp"

#include <gtest/gtest.h>

namespace
{

using hui::app::rail_confirm_enters_content;

// Issue #22: Cross on Search goes straight to the keyboard.
TEST(RailFocus, SearchTakesTheFocusFromTheSidebar)
{
    EXPECT_TRUE(rail_confirm_enters_content(hui::app::kSearchScreen));
}

// Every other section keeps the sidebar, so up/down still walk the sections.
TEST(RailFocus, OtherSectionsKeepTheSidebar)
{
    for (std::size_t screen = 0; screen < hui::app::kTabCount; ++screen)
    {
        if (screen == hui::app::kSearchScreen)
            continue;
        EXPECT_FALSE(rail_confirm_enters_content(screen)) << "screen " << screen;
    }
}

} // namespace

// youtube-tv - Tests: the rows' vertical glide keeps the focused row in view.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/row_scroll.hpp"
#include "ui/components/carousel.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace
{

using hui::app::RowScroll;

constexpr float kFrame = 1.0f / 60.0f;

// Glides long enough (two seconds) for the offset to settle.
void settle(RowScroll &scroll, int row)
{
    for (int i = 0; i < 120; ++i)
        scroll.glide(row, kFrame);
}

TEST(RowScroll, FirstPitchAlwaysPlacesTheRows)
{
    RowScroll scroll;
    EXPECT_TRUE(scroll.set_pitch(320.0f));
    EXPECT_FLOAT_EQ(scroll.pitch(), 320.0f);
    // The same height, or one within rounding noise, does not re-place them
    // (that would snap every shelf's sideways glide).
    EXPECT_FALSE(scroll.set_pitch(320.0f));
    EXPECT_FALSE(scroll.set_pitch(320.3f));
    EXPECT_FLOAT_EQ(scroll.pitch(), 320.0f);
}

TEST(RowScroll, GlideSettlesExactlyOnTheFocusRow)
{
    RowScroll scroll;
    scroll.set_pitch(300.0f);
    settle(scroll, 5);
    EXPECT_FLOAT_EQ(scroll.offset(), 1500.0f);
    scroll.jump_to(2);
    EXPECT_FLOAT_EQ(scroll.offset(), 600.0f);
}

// Issue #16: a row placed at i * pitch must come to the top of the view when
// it has the focus, however far down the feed it is, also after the card look
// changed the rows' height.
TEST(RowScroll, DeepRowsStayAlignedAfterTheRowHeightChanges)
{
    RowScroll scroll;
    scroll.set_pitch(330.0f); // the look a new row starts with
    settle(scroll, 3);
    // The settings' look is applied and the rows are now shorter.
    ASSERT_TRUE(scroll.set_pitch(296.0f));
    const float placed_pitch = scroll.pitch();
    for (int row = 0; row < 40; ++row)
    {
        settle(scroll, row);
        const float row_y = static_cast<float>(row) * placed_pitch;
        EXPECT_FLOAT_EQ(scroll.offset() - row_y, 0.0f) << "row " << row;
    }
}

TEST(RowScroll, NewPitchKeepsTheSameRowInView)
{
    RowScroll scroll;
    scroll.set_pitch(300.0f);
    settle(scroll, 4);
    scroll.set_pitch(350.0f);
    EXPECT_FLOAT_EQ(scroll.offset(), 4.0f * 350.0f);
}

// What made the rows drift: the settings' look changes a shelf's height after
// it was placed. The Glow layout's flat cards are shorter than SmartTube's.
TEST(RowScroll, CardLookChangesTheShelfHeight)
{
    hui::ui::Carousel shelf;
    shelf.title = "Top stories";
    shelf.style.item_width = 400.0f;
    shelf.style.card.text = hui::ui::CardText::below;
    shelf.style.card.info_box = true;
    shelf.style.card.title_lines = 2;
    shelf.style.card.focus_scale = 1.1f;
    shelf.style.card.lift = 6.0f;
    shelf.set_bounds({160.0f, 140.0f, 1680.0f, 400.0f});
    const float smarttube = shelf.preferred_height();
    shelf.style.card.title_lines = 1;
    EXPECT_LT(shelf.preferred_height(), smarttube);
    shelf.style.card.title_lines = 2;
    shelf.style.card.info_box = false;
    shelf.style.card.focus_scale = 1.0f;
    shelf.style.card.lift = 0.0f;
    EXPECT_LT(shelf.preferred_height(), smarttube);
}

} // namespace

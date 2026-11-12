// youtube-tv - The vertical glide of a feed's rows.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// RowsView places row i at i * pitch and scrolls the focused row to the top
// by sliding everything up by focus * pitch. Both have to use the same pitch.
// They did not: the rows were placed when the feed arrived, with the card
// look a new row starts with, and every frame afterwards the settings' look
// (title lines, the Glow layout's flat cards) was applied, which changes a
// row's height. The scroll used the new height and the rows kept the old one,
// so each row down put the focused one a few pixels further off its place
// until it slid out of the view (issue #16, the News tab). This keeps the
// pitch the rows were placed with and is the only place the scroll is worked
// out from, and it says when a new height means the rows must be placed again.

#pragma once

namespace hui::app
{

class RowScroll
{
  public:
    // Heights closer than this are the same pitch: re-placing the rows snaps
    // every shelf's sideways glide, so rounding noise must not trigger it.
    static constexpr float kTolerance = 0.5f;

    // The rows' pitch (row height plus gap) as measured now. True when it
    // differs from the one the rows were placed with: the caller places them
    // again. The view stays on the row it was showing.
    bool set_pitch(float pitch);
    float pitch() const
    {
        return pitch_;
    }

    // The offset that puts `row` at the top of the view.
    float target(int row) const
    {
        return static_cast<float>(row) * pitch_;
    }
    // Straight to `row`, no glide: the focus row moved under a new feed.
    void jump_to(int row);
    // Eases the offset toward `row`'s target over `dt` seconds and settles
    // on it once it is within half a pixel.
    void glide(int row, float dt);
    float offset() const
    {
        return offset_;
    }

  private:
    float pitch_ = 0.0f;
    float offset_ = 0.0f;
};

} // namespace hui::app

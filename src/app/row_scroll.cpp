// youtube-tv - The vertical glide of a feed's rows.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/row_scroll.hpp"

#include <cmath>

namespace hui::app
{

bool RowScroll::set_pitch(float pitch)
{
    if (pitch_ > 0.0f && std::fabs(pitch - pitch_) < kTolerance)
        return false;
    // Scale the offset with the rows so the same row stays in view (and a
    // glide in progress carries on from the same place).
    if (pitch_ > 0.0f)
        offset_ *= pitch / pitch_;
    pitch_ = pitch;
    return true;
}

void RowScroll::jump_to(int row)
{
    offset_ = target(row);
}

void RowScroll::glide(int row, float dt)
{
    const float goal = target(row);
    offset_ += (goal - offset_) * (1.0f - std::exp(-14.0f * dt));
    if (std::fabs(goal - offset_) < 0.5f)
        offset_ = goal;
}

} // namespace hui::app

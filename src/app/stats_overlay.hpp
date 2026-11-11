// youtube-tv - Stats for nerds: the slow lines, refreshed a few times a second.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Building the overlay every frame asked the player for its stats block (one
// snprintf over the decoder state and the buffered time), split it and
// measured every line with the mono font, 60 times a second. With the overlay
// on, a frame-time meter showed worse 1% lows (issue #3). Only the clock line
// changes at frame rate: it is drawn first and live, the other lines are kept
// here and rebuilt every kRefreshS, and the frames in between reuse them.

#pragma once

#include <string>
#include <vector>

namespace hui::app
{

class StatsOverlay
{
  public:
    // Four times a second: the counters still read as live, and the queue
    // and buffered figures do not flicker.
    static constexpr float kRefreshS = 0.25f;

    // Advances the timer by `dt` seconds. True when the kept lines are due
    // for a rebuild: the first call after invalidate(), then every kRefreshS.
    bool tick(float dt);
    // The next tick() rebuilds: a new video, or the overlay was turned off
    // (its lines would be stale when it comes back).
    void invalidate();

    // The rebuilt lines and the widest one's width in virtual pixels.
    void set(std::vector<std::string> lines, float width);
    const std::vector<std::string> &lines() const
    {
        return lines_;
    }
    float width() const
    {
        return width_;
    }

    // A player's stats block, one entry per '\n'-separated line. A trailing
    // newline does not add an empty line.
    static std::vector<std::string> split_lines(const std::string &block);

  private:
    std::vector<std::string> lines_;
    float width_ = 0.0f;
    float since_s_ = 0.0f;
    bool valid_ = false;
};

} // namespace hui::app

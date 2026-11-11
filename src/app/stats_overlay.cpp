// youtube-tv - Stats for nerds: the slow lines, refreshed a few times a second.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/stats_overlay.hpp"

#include <utility>

namespace hui::app
{

bool StatsOverlay::tick(float dt)
{
    if (dt > 0.0f)
        since_s_ += dt;
    if (valid_ && since_s_ < kRefreshS)
        return false;
    // Start the next period from zero rather than carrying the overshoot: a
    // long frame (a seek, a stall) must not make the next frames rebuild too.
    since_s_ = 0.0f;
    valid_ = true;
    return true;
}

void StatsOverlay::invalidate()
{
    valid_ = false;
}

void StatsOverlay::set(std::vector<std::string> lines, float width)
{
    lines_ = std::move(lines);
    width_ = width;
}

std::vector<std::string> StatsOverlay::split_lines(const std::string &block)
{
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at < block.size())
    {
        std::size_t end = block.find('\n', at);
        if (end == std::string::npos)
            end = block.size();
        lines.push_back(block.substr(at, end - at));
        at = end + 1;
    }
    return lines;
}

} // namespace hui::app

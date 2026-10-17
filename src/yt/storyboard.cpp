// youtube-tv - Seek previews from YouTube's storyboards.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/storyboard.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace yt
{
namespace
{

std::vector<std::string> split(std::string_view text, char separator)
{
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= text.size())
    {
        const std::size_t end = text.find(separator, at);
        out.emplace_back(
            text.substr(at, end == std::string_view::npos ? text.size() - at : end - at));
        if (end == std::string_view::npos)
            break;
        at = end + 1;
    }
    return out;
}

void replace_all(std::string &text, const std::string &from, const std::string &to)
{
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos)
    {
        text.replace(at, from.size(), to);
        at += to.size();
    }
}

} // namespace

bool Storyboard::parse(std::string_view spec, double duration_s, int want_width)
{
    count_ = 0;
    const std::vector<std::string> parts = split(spec, '|');
    if (parts.size() < 2 || parts[0].find("$L") == std::string::npos)
        return false;
    base_ = parts[0];
    duration_s_ = duration_s;
    int best = -1;
    int best_distance = 1 << 30;
    for (std::size_t i = 1; i < parts.size(); ++i)
    {
        const std::vector<std::string> fields = split(parts[i], '#');
        if (fields.size() < 8)
            continue;
        const int width = std::atoi(fields[0].c_str());
        const int count = std::atoi(fields[2].c_str());
        if (width <= 0 || count <= 0)
            continue;
        const int distance = std::abs(width - want_width);
        if (distance < best_distance)
        {
            best_distance = distance;
            best = static_cast<int>(i);
        }
    }
    if (best < 0)
        return false;
    const std::vector<std::string> fields = split(parts[static_cast<std::size_t>(best)], '#');
    level_ = best - 1;
    width_ = std::atoi(fields[0].c_str());
    height_ = std::atoi(fields[1].c_str());
    count_ = std::atoi(fields[2].c_str());
    columns_ = std::max(1, std::atoi(fields[3].c_str()));
    rows_ = std::max(1, std::atoi(fields[4].c_str()));
    interval_s_ = std::atof(fields[5].c_str()) / 1000.0;
    name_ = fields[6];
    sigh_ = fields[7];
    return count_ > 0 && width_ > 0 && height_ > 0;
}

StoryboardFrame Storyboard::frame_at(double t) const
{
    StoryboardFrame frame;
    if (count_ == 0)
        return frame;
    // No interval (level 0 of some videos): the frames share the length.
    const double interval =
        interval_s_ > 0.0 ? interval_s_ : (duration_s_ > 0.0 ? duration_s_ / count_ : 1.0);
    const int index =
        std::clamp(static_cast<int>(std::floor(std::max(0.0, t) / interval)), 0, count_ - 1);
    const int per_sheet = columns_ * rows_;
    const int sheet = index / per_sheet;
    const int cell = index % per_sheet;
    std::string url = base_;
    replace_all(url, "$L", std::to_string(level_));
    std::string name = name_;
    replace_all(name, "$M", std::to_string(sheet));
    replace_all(url, "$N", name);
    if (!sigh_.empty())
        url += (url.find('?') == std::string::npos ? "?sigh=" : "&sigh=") + sigh_;
    frame.url = url;
    // The last sheet may hold fewer rows than the full grid.
    const int frames_here = std::min(per_sheet, count_ - sheet * per_sheet);
    const int rows_here = std::max(1, (frames_here + columns_ - 1) / columns_);
    frame.w = 1.0f / static_cast<float>(columns_);
    frame.h = 1.0f / static_cast<float>(rows_here);
    frame.u = static_cast<float>(cell % columns_) * frame.w;
    frame.v = static_cast<float>(cell / columns_) * frame.h;
    frame.width = width_;
    frame.height = height_;
    return frame;
}

} // namespace yt

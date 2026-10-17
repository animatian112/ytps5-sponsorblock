// youtube-tv - Chapters from a video description's timestamps.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// YouTube makes chapters out of a description that lists at least three
// timestamps in rising order, the first at 0:00 ("0:00 Intro", "1:23 - Setup").
// The same rule here, for watch pages whose answer carries no chapter list.

#pragma once

#include "yt/types.hpp"

#include <string_view>
#include <vector>

namespace yt
{

std::vector<Chapter> parse_chapters(std::string_view description);

// The index of the chapter playing at `position` (-1 before the first).
int chapter_at(const std::vector<Chapter> &chapters, double position);

} // namespace yt

// youtube-tv - How old a feed item is, read off its "3 hours ago" line.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The TV client's Subscriptions starts with a "Most relevant" row, not the
// newest uploads; the app sorts it by the age each card shows. YouTube writes
// that age in the language asked for, so the units of the app's languages
// are known, in full and short ("2 days ago", "21y ago", "hace 3 horas").

#pragma once

#include <string_view>
#include <vector>

#include "yt/types.hpp"

namespace yt
{

// Seconds since the item went up, from the part of its line after the last
// bullet; -1 when the line names no age (a live stream, "Premieres...").
long long age_seconds(std::string_view detail);

// Newest first, each video once (the "Most relevant" row repeats some), Shorts
// after the rest. Items without an age stay next to the one before them.
void sort_newest_first(std::vector<FeedItem> *items);

} // namespace yt

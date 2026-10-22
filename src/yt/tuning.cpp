// youtube-tv - Settings > Developer: the player's knobs, read when a video opens.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/tuning.hpp"

#include <mutex>

namespace yt
{
namespace
{

std::mutex &tuning_mutex()
{
    static std::mutex mutex;
    return mutex;
}

PlayerTuning &current()
{
    static PlayerTuning tuning;
    return tuning;
}

} // namespace

void set_player_tuning(const PlayerTuning &tuning)
{
    std::lock_guard<std::mutex> lock(tuning_mutex());
    current() = tuning;
}

PlayerTuning player_tuning()
{
    std::lock_guard<std::mutex> lock(tuning_mutex());
    return current();
}

} // namespace yt

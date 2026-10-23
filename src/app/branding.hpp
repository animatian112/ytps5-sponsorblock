// youtube-tv - DeArrow titles and thumbnails on cards and in the player.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/settings.hpp"

#include <string>

namespace hui::app
{

struct CardBrand
{
    std::string title;         // empty: YouTube's
    std::string thumbnail_url; // empty: YouTube's
};

// DeArrow's title and thumbnail for a video when Settings > SponsorBlock >
// DeArrow asks for them. Never waits: an unknown video is asked for in the
// background and its card changes once the answer is in.
CardBrand card_brand(const std::string &video_id, const Settings &settings);

} // namespace hui::app

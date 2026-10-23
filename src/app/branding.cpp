// youtube-tv - DeArrow titles and thumbnails on cards and in the player.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/branding.hpp"

#include "yt/dearrow.hpp"

namespace hui::app
{

namespace
{

yt::DeArrow &dearrow()
{
    // One cache for every screen; its threads start with the first lookup.
    static yt::DeArrow instance;
    return instance;
}

} // namespace

CardBrand card_brand(const std::string &video_id, const Settings &settings)
{
    CardBrand brand;
    if (!settings.dearrow_titles && !settings.dearrow_thumbnails)
        return brand;
    yt::DeArrow &service = dearrow();
    service.set_alt_server(settings.sb_alt_server);
    yt::Branding branding;
    if (!service.lookup(video_id, &branding))
        return brand;
    if (settings.dearrow_titles)
        brand.title = branding.title;
    if (settings.dearrow_thumbnails && branding.has_thumbnail)
        brand.thumbnail_url = yt::dearrow_thumbnail_url(video_id, branding.thumbnail_s);
    return brand;
}

} // namespace hui::app

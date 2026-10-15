// youtube-tv - The app's theme.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/theme.hpp"

#include "ui/components/component.hpp"

namespace hui::app
{

// The YouTube dark palette, as plain data over the kit's theme shape.
const ui::Theme &youtube_theme()
{
    ui::Theme theme = ui::default_theme();
    theme.id = "youtube";
    theme.name = "YouTube";
    theme.family = "dark";
    theme.summary = "YouTube's dark theme: near-black page, red accent";
    theme.dark = true;
    theme.page = gfx::Color::rgb(0x0f0f0f);
    theme.page_text = gfx::Color::rgb(0xf1f1f1);
    theme.page_text_muted = gfx::Color::rgb(0xaaaaaa);
    theme.surface = gfx::Color::rgb(0x1c1c1c);
    theme.surface_high = gfx::Color::rgb(0x272727);
    theme.text = gfx::Color::rgb(0xf1f1f1);
    theme.text_muted = gfx::Color::rgb(0xaaaaaa);
    theme.primary = gfx::Color::rgb(0xff0033);
    theme.on_primary = gfx::Color::rgb(0xffffff);
    theme.secondary = gfx::Color::rgb(0x3f3f3f);
    theme.on_secondary = gfx::Color::rgb(0xf1f1f1);
    theme.accent = gfx::Color::rgb(0xff0033);
    theme.outline = gfx::Color::rgb(0x3f3f3f);
    theme.focus = gfx::Color::rgb(0xffffff);
    theme.shadow = gfx::Color::rgb(0x000000, 0.55f);
    theme.light = gfx::Color::rgb(0xffffff, 0.08f);
    theme.backdrop = {};
    theme.backdrop.mode = gfx::BackdropMode::none;
    theme.radius = 12.0f;
    theme.radius_card = 16.0f;
    theme.style = ui::SurfaceStyle::flat;
    theme.corner = ui::Corner::round;
    theme.omega = 16.0f;
    theme.damping = 1.0f;
    theme.sounds = audio::SoundSet::glass;
    static const ui::Theme value = theme;
    return value;
}

const ui::Theme &card_theme()
{
    static const ui::Theme value = []
    {
        ui::Theme theme = youtube_theme();
        theme.id = "youtube-cards";
        theme.accent = gfx::Color::rgb(0x000000, 0.82f);
        theme.radius_card = 8.0f;
        theme.radius = 8.0f;
        return theme;
    }();
    return value;
}

} // namespace hui::app

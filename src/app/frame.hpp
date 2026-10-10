// youtube-tv - One frame's layers, as the app hands them to the renderer.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"

#include <cstdint>

namespace hui::app
{

// One frame, back to front:
//   backdrop -> scene -> [glass capture] -> overlay -> post
struct Frame
{
    gfx::BackdropSpec backdrop; // procedural shader behind everything
    gfx::DrawList scene;        // the screen itself
    gfx::DrawList overlay;      // dialogs, the tab bar, toasts
    gfx::BackdropSpec post;     // optional overlay shader (scanlines, vignette)
    // Set glass to have the backdrop and scene blurred into glass_texture
    // before the overlay is drawn; overlay.glass(glass_texture, ...) then
    // paints frosted panels.
    bool glass = false;
    std::uint32_t glass_texture = 0;

    void reset()
    {
        backdrop = {};
        post = {};
        scene.clear();
        overlay.clear();
        glass = false;
    }
};

} // namespace hui::app

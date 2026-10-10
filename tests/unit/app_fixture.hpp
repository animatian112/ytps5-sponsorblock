// youtube-tv - Test fixture: real baked fonts and a draw list, no OpenGL.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/input.hpp"
#include "core/save_file.hpp"
#include "gfx/draw_list.hpp"
#include "gfx/font.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"

#include <gtest/gtest.h>

#include <string>

namespace hui::testing
{

// Loads the baked fonts and builds the pieces a screen needs that do not
// touch the network or the GPU. Screens that need the service or thumbnails
// are exercised by the host snapshot run instead.
class AppFixture : public ::testing::Test
{
  protected:
    static InputFrame idle()
    {
        InputFrame input;
        input.connected = true;
        return input;
    }
    static InputFrame nav(Direction direction)
    {
        InputFrame input = idle();
        input.nav = direction;
        return input;
    }
    static InputFrame press(Action action)
    {
        InputFrame input = idle();
        input.pressed = action_bit(action);
        input.held = input.pressed;
        return input;
    }
    // The same direction again while the repeat timer runs (held key).
    static InputFrame repeat(Direction direction)
    {
        InputFrame input = nav(direction);
        input.nav_repeat = true;
        return input;
    }

    gfx::Font regular_, semibold_, display_, mono_;
    ui::Fonts fonts_;
    ui::Feedback feedback_;

    AppFixture()
    {
        load("google-sans-regular", &regular_, &fonts_.regular);
        load("google-sans-medium", &semibold_, &fonts_.semibold);
        load("google-sans-bold", &display_, &fonts_.display);
        load("dejavu-sans-mono", &mono_, &fonts_.mono);
    }

  private:
    static void load(const char *name, gfx::Font *font, ui::FontRef *ref)
    {
        std::string data;
        const std::string path = std::string(HUI_SOURCE_DIR) + "/assets/fonts/" + name + ".huifont";
        ASSERT_TRUE(save::read_file(path, &data)) << path;
        ASSERT_TRUE(font->load(data)) << font->error();
        ref->font = font;
        ref->texture = 1;
    }
};

} // namespace hui::testing

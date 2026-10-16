// youtube-tv - The contract between the app and one screen.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/context.hpp"
#include "app/frame.hpp"
#include "core/input.hpp"
#include "ui/feedback.hpp"

#include <cstdint>
#include <span>

namespace hui::app
{

// One scripted input for the self-driving tour (host snapshots and the
// console validation run): wait, optionally save a picture, then send input.
struct TourStep
{
    float wait = 0.5f;       // seconds before the step acts
    std::uint32_t press = 0; // action bits pressed for one frame
    Direction nav = Direction::none;
    const char *capture = nullptr; // picture name suffix, taken after the wait
    float stick_x = 0.0f;          // left stick held during the wait
    float stick_y = 0.0f;
    std::uint32_t hold = 0; // action bits held during the wait
    float trigger_l = 0.0f; // analog triggers held during the wait
    float trigger_r = 0.0f;
};

class Screen
{
  public:
    virtual ~Screen() = default;
    // "home", "player", ...: picture names and logs.
    virtual const char *id() const = 0;
    virtual const char *title() const = 0; // shown in the tab bar / header
    // The screen became visible: restart its entrance animation.
    virtual void enter()
    {
    }
    // The screen is being left.
    virtual void leave()
    {
    }
    // One line for the bottom-right corner of the chrome.
    virtual const char *hint() const
    {
        return "Options quit";
    }
    // True while this screen uses L1/R1 itself (a channel page switches its
    // tabs with them): the app then leaves the section keys to the screen
    // instead of switching the rail section.
    virtual bool consumes_section_keys() const
    {
        return false;
    }
    // True once, after the frame in which the player pushed left past the
    // leftmost item: the app then moves the focus into the rail. The screen
    // keeps its own focus until the app takes it.
    virtual bool wants_rail_focus() const
    {
        return false;
    }
    // The video card the focus sits on, for the card menu (hold Cross or
    // press Square); nullptr when the focus is elsewhere.
    virtual const yt::FeedItem *focused_item() const
    {
        return nullptr;
    }
    virtual void update(const InputFrame &input, float dt, ui::Feedback &feedback) = 0;
    // Must not change state: the app may draw a screen more than once per
    // frame (glass pass, transitions).
    virtual void draw(const Context &context, Frame &frame) const = 0;
    // Scripted inputs that show the screen off; empty = just look at it.
    virtual std::span<const TourStep> tour() const
    {
        return {};
    }
};

} // namespace hui::app

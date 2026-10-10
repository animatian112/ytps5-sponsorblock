// youtube-tv - Settings: the codec switches, the resolution cap, haptics and
// sign-out, as one scrolling Form.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/context.hpp"
#include "app/screen.hpp"

#include "ui/components.hpp"

#include <cstdint>
#include <memory>

namespace hui::app
{

class SettingsScreen final : public Screen
{
  public:
    explicit SettingsScreen(Context &context);

    const char *id() const override
    {
        return "settings";
    }
    const char *title() const override
    {
        return "Settings";
    }
    const char *hint() const override
    {
        return "Circle back   Left: menu   Options quit";
    }
    // The Form consumes the directions, so a left press never reaches the rail
    // here; the touchpad is the way in.
    bool wants_rail_focus() const override
    {
        return false;
    }
    void enter() override;
    void update(const InputFrame &input, float dt, ui::Feedback &feedback) override;
    void draw(const Context &context, Frame &frame) const override;
    std::span<const TourStep> tour() const override;

  private:
    void apply(int id, ui::Feedback &feedback);
    void activate(int id);

    Context &context_;
    ui::Form form_;
    float clock_ = 0.0f;
    // The "Check for updates" action waits for the background check to settle
    // before it reports the result.
    bool update_watching_ = false;
    std::uint64_t update_watch_revision_ = 0;
};

} // namespace hui::app

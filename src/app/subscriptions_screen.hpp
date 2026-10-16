// youtube-tv - Subscriptions: the channels' newest videos, or a sign-in
// prompt while the app has no account.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/context.hpp"
#include "app/feed_view.hpp"
#include "yt/service.hpp"
#include "app/screen.hpp"

#include "ui/components.hpp"

#include <cstdint>
#include <memory>

namespace hui::app
{

class SubscriptionsScreen final : public Screen
{
  public:
    // An account feed shown as a grid: Subscriptions, or History.
    explicit SubscriptionsScreen(Context &context, yt::FeedKind kind = yt::FeedKind::subscriptions);

    const char *id() const override
    {
        return kind_ == yt::FeedKind::history   ? "history"
               : kind_ == yt::FeedKind::library ? "playlists"
                                                : "subscriptions";
    }
    const char *title() const override
    {
        return kind_ == yt::FeedKind::history   ? "History"
               : kind_ == yt::FeedKind::library ? "Playlists"
                                                : "Subscriptions";
    }
    const char *hint() const override
    {
        return context_.service.signed_in() ? "Circle back   Left: menu   Options quit"
                                            : "Sign in with QR or TV code";
    }
    // The sign-in button state has no grid to walk out of, so no rail grab.
    bool wants_rail_focus() const override
    {
        return context_.service.signed_in() && subs_.wants_rail_focus();
    }
    void enter() override;
    const yt::FeedItem *focused_item() const override
    {
        return subs_.focused_item();
    }
    void update(const InputFrame &input, float dt, ui::Feedback &feedback) override;
    void draw(const Context &context, Frame &frame) const override;
    std::span<const TourStep> tour() const override;

  private:
    void refresh_from_service();

    void request();

    Context &context_;
    yt::FeedKind kind_;
    FeedView subs_;
    ui::PushButton sign_in_;
    std::uint64_t seen_revision_ = 0;
    bool more_requested_ = false;
    bool requested_ = false;
    float clock_ = 0.0f;
};

} // namespace hui::app

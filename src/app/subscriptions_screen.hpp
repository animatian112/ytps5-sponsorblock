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
#include <string>

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
        if (!context_.service.signed_in())
            return "Sign in with QR or TV code";
        return has_groups() ? "Triangle next group   Circle back   Left: menu"
                            : "Circle back   Left: menu   Options quit";
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
    bool glow_hero() const override
    {
        return true;
    }
    void update(const InputFrame &input, float dt, ui::Feedback &feedback) override;
    void draw(const Context &context, Frame &frame) const override;
    std::span<const TourStep> tour() const override;

  private:
    void refresh_from_service();
    // Subscriptions only: the viewer has channel groups to pick from.
    bool has_groups() const;
    // The group tabs ("All", then each group) from the current groups; the
    // shown group falls back to All when it was removed.
    void refresh_groups();
    // Where the grid starts: under the group tabs when they show.
    float grid_top() const;

    void request();

    Context &context_;
    yt::FeedKind kind_;
    FeedView subs_;
    ui::PushButton sign_in_;
    std::uint64_t seen_revision_ = 0;
    bool more_requested_ = false;
    bool requested_ = false;
    float clock_ = 0.0f;
    ui::TabBar groups_;
    int group_ = -1;         // the group shown, -1 for every channel
    std::string group_name_; // kept to find the same group after an edit
    std::uint64_t seen_groups_ = ~std::uint64_t{0};
    // A group's videos can be few in the pages loaded so far: more pages are
    // asked for, up to this many for each group picked, so the screen fills
    // without walking the whole feed for a channel that rarely uploads.
    static constexpr int kGroupPages = 8;
    int group_pages_ = 0;
};

} // namespace hui::app

// youtube-tv - Subscriptions screen.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/subscriptions_screen.hpp"

#include "app/theme.hpp"
#include "yt/service.hpp"
#include "ui/components.hpp"

namespace hui::app
{
namespace
{

const TourStep kTour[] = {
    {1.2f, 0, Direction::none, "start"},
    {0.8f, 0, Direction::right, nullptr},
};

} // namespace

SubscriptionsScreen::SubscriptionsScreen(Context &context) : context_(context)
{
    subs_.set_bounds({kContentX, 150.0f, kContentW, 860.0f});
    subs_.set_active(true);

    sign_in_.style.theme = youtube_theme();
    sign_in_.style.role = ui::ButtonRole::primary;
    sign_in_.label = "Sign in";
    sign_in_.set_bounds({810.0f, 560.0f, 300.0f, 64.0f});
    sign_in_.set_active(true);

    seen_revision_ = context_.service.revision(yt::FeedKind::subscriptions);
    refresh_from_service();
}

void SubscriptionsScreen::enter()
{
    subs_.enter();
    subs_.set_active(true);
    sign_in_.set_active(true);
    refresh_from_service();
}

void SubscriptionsScreen::refresh_from_service()
{
    const yt::FeedView view = context_.service.feed(yt::FeedKind::subscriptions);
    subs_.set_items(view.items, context_);
    if (context_.service.signed_in())
    {
        subs_.set_empty_text("No subscriptions found");
        subs_.set_state(view.state, view.error);
    }
    else
    {
        // Not signed in: the cause is the missing account, and no text may
        // claim there are no subscriptions.
        subs_.set_empty_text("Sign in to see your subscriptions");
        subs_.set_state(yt::Load::idle, {});
    }
    more_requested_ = false;
}

void SubscriptionsScreen::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    clock_ += dt;

    if (!context_.service.signed_in())
    {
        requested_ = false;
        sign_in_.set_active(true);
        if (sign_in_.handle(input, feedback) == ui::Event::activated)
            context_.nav.start_login = true;
        sign_in_.update(dt);
        return;
    }

    const std::uint64_t revision = context_.service.revision(yt::FeedKind::subscriptions);
    if (revision != seen_revision_)
    {
        seen_revision_ = revision;
        refresh_from_service();
    }

    const yt::FeedView view = context_.service.feed(yt::FeedKind::subscriptions);

    // First load once the account is known.
    if (!requested_ && view.state == yt::Load::idle)
    {
        requested_ = true;
        context_.service.request_subscriptions();
    }

    const std::size_t before = subs_.items().size();
    subs_.set_active(true);
    if (subs_.handle(input, feedback, context_) && subs_.has_activation())
    {
        context_.nav.open_video = true;
        context_.nav.video_id = subs_.activated().video_id;
        subs_.clear_activation();
    }
    if (subs_.items().size() != before)
        more_requested_ = false;

    if (!more_requested_ && view.state == yt::Load::ready && !view.continuation.empty() &&
        subs_.at_last_row())
    {
        more_requested_ = true;
        context_.service.request_more(yt::FeedKind::subscriptions);
    }

    subs_.update(dt, context_);
}

void SubscriptionsScreen::draw(const Context &context, Frame &frame) const
{
    ui::Canvas canvas{frame.scene, context.fonts, frame.glass_texture, clock_};

    if (!context.service.signed_in())
    {
        ui::Panel panel;
        panel.style.theme = youtube_theme();
        panel.set_bounds({610.0f, 300.0f, 700.0f, 420.0f});
        panel.draw(canvas);
        ui::text(canvas.list, context.fonts.semibold, "Subscriptions", 960.0f, 400.0f, 40.0f,
                 gfx::Color::rgb(0xf1f1f1), gfx::Align::center);
        ui::text(canvas.list, context.fonts.regular, "Sign in to see your subscriptions.", 960.0f,
                 470.0f, 24.0f, gfx::Color::rgb(0xaaaaaa), gfx::Align::center);
        sign_in_.draw(canvas);
        return;
    }

    ui::text(canvas.list, context.fonts.semibold, "Subscriptions", kContentX, kSectionTitleY,
             kSectionTitleSize, gfx::Color::rgb(0xf1f1f1));
    subs_.draw(canvas, context);
}

std::span<const TourStep> SubscriptionsScreen::tour() const
{
    return kTour;
}

std::unique_ptr<Screen> make_subscriptions_screen(Context &context)
{
    return std::make_unique<SubscriptionsScreen>(context);
}

} // namespace hui::app

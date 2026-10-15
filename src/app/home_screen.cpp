// youtube-tv - Home: what YouTube recommends, as a grid.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The grid is drawn from the copies the service publishes: the screen keeps
// its own item list and only touches the service when the revision changed.

#include "app/home_screen.hpp"
#include "yt/log.hpp"

#include "app/theme.hpp"
#include "yt/service.hpp"

namespace hui::app
{
namespace
{

const TourStep kTour[] = {
    {1.4f, 0, Direction::none, "start", 0, 0, 0, 0, 0},
    // The cursor walk: right and left across the grid, then down/down down
    // the rail to Subscriptions; confirm leaves home for that section.
    {0.9f, 0, Direction::right, nullptr, 0, 0, 0, 0, 0},
    // Two lefts: the first only walks back to column 0, the second leaves the
    // grid at its left edge and hands the focus to the rail.
    {0.9f, 0, Direction::left, nullptr, 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::left, nullptr, 0, 0, 0, 0, 0},
    {1.0f, 0, Direction::none, "rail", 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::down, nullptr, 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::down, nullptr, 0, 0, 0, 0, 0},
    {1.0f, 0, Direction::none, "rail-subs", 0, 0, 0, 0, 0},
    {0.9f, hui::action_bit(Action::confirm), Direction::none, "subscriptions", 0, 0, 0, 0, 0},
};

// The empty state names the cause: "signed out" and "nothing recommended"
// are different states and must not read the same. Signed out YouTube
// answers an empty home feed from most networks; the app says why instead of
// showing nothing.
const char *kSignedOutEmpty = "Sign in to see your Home feed - or use Search";
const char *kSignedInEmpty = "No feed available - use Search";

} // namespace

HomeScreen::HomeScreen(Context &context) : context_(context)
{
    feed_.grid().set_bounds({kContentX, 150.0f, kContentW, 860.0f});
    feed_.set_active(true);
    seen_revision_ = context_.service.revision(yt::FeedKind::home);
    refresh_from_service();
}

void HomeScreen::enter()
{
    feed_.enter();
    feed_.set_active(true);
    refresh_from_service();
}

void HomeScreen::refresh_from_service()
{
    const yt::FeedView view = context_.service.feed(yt::FeedKind::home);
    feed_.set_items(view.items, context_);
    if (context_.service.signed_in())
    {
        feed_.set_empty_text(kSignedInEmpty);
        feed_.set_state(view.state, view.error);
    }
    else
    {
        // Not signed in: there is no feed to load or to fail - the empty
        // text names the missing account, never "no feed".
        feed_.set_empty_text(kSignedOutEmpty);
        feed_.set_state(yt::Load::idle, {});
    }
    more_requested_ = false;
}

void HomeScreen::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    clock_ += dt;
    const std::uint64_t revision = context_.service.revision(yt::FeedKind::home);
    if (revision != seen_revision_)
    {
        seen_revision_ = revision;
        refresh_from_service();
    }

    const std::size_t before = feed_.items().size();
    feed_.set_active(true);
    if (feed_.handle(input, feedback, context_) && feed_.has_activation())
    {
        context_.nav.open_video = true;
        context_.nav.video_id = feed_.activated().video_id;
        feed_.clear_activation();
    }
    if (feed_.items().size() != before)
        more_requested_ = false;

    const yt::FeedView view = context_.service.feed(yt::FeedKind::home);
    if (!more_requested_ && !view.continuation.empty() && feed_.at_last_row())
    {
        more_requested_ = true;
        context_.service.request_more(yt::FeedKind::home);
    }

    feed_.update(dt, context_);
}

void HomeScreen::draw(const Context &context, Frame &frame) const
{
    ui::Canvas canvas{frame.scene, context.fonts, frame.glass_texture, clock_};
    ui::text(canvas.list, context.fonts.semibold, "Home", kContentX, kSectionTitleY,
             kSectionTitleSize, gfx::Color::rgb(0xf1f1f1));
    const yt::FeedView view = context.service.feed(yt::FeedKind::home);
    if (view.state == yt::Load::loading && view.items.empty())
    {
        ui::Skeleton bones;
        bones.style.theme = youtube_theme();
        bones.style.kind = ui::SkeletonKind::card;
        bones.set_bounds({kContentX, 170.0f, kContentW, 700.0f});
        ui::Canvas skeleton{frame.scene, context.fonts, frame.glass_texture, clock_};
        bones.draw(skeleton);
    }
    feed_.draw(canvas, context);

    // Nothing to show: the last log lines name what the network did, so a
    // failure is readable on the television without FTP.
    if (view.items.empty() && view.state != yt::Load::loading)
    {
        std::string lines[8];
        const std::size_t count = yt::log::recent(lines, 8);
        float y = 1000.0f - static_cast<float>(count) * 24.0f;
        for (std::size_t i = 0; i < count; ++i)
        {
            std::string shown = lines[i];
            if (shown.size() > 110)
                shown.resize(110);
            ui::text(canvas.list, context.fonts.mono, shown, kContentX, y, 18.0f,
                     gfx::Color::rgb(0x8a8a8a));
            y += 24.0f;
        }
    }
}

std::span<const TourStep> HomeScreen::tour() const
{
    return kTour;
}

std::unique_ptr<Screen> make_home_screen(Context &context)
{
    return std::make_unique<HomeScreen>(context);
}

} // namespace hui::app

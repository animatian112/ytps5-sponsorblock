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
    // Rows: right along "Recommended", down to "Trending".
    {0.6f, 0, Direction::right, nullptr, 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::down, nullptr, 0, 0, 0, 0, 0},
    {1.0f, 0, Direction::none, "rows", 0, 0, 0, 0, 0},
    // Square on a card: the card menu (holding Cross opens the same one).
    {0.4f, action_bit(Action::west), Direction::none, nullptr, 0, 0, 0, 0, 0},
    {0.8f, action_bit(Action::back), Direction::none, "menu", 0, 0, 0, 0, 0},
    // Left twice: back to the first card, then out into the rail.
    {0.6f, 0, Direction::left, nullptr, 0, 0, 0, 0, 0},
    {0.6f, 0, Direction::left, nullptr, 0, 0, 0, 0, 0},
    {1.0f, 0, Direction::none, "rail", 0, 0, 0, 0, 0},
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
    rows_.set_bounds({kContentX, 140.0f, kContentW, 880.0f});
    feed_.set_active(true);
    seen_revision_ = context_.service.revision(yt::FeedKind::home);
    refresh_from_service();
}

void HomeScreen::enter()
{
    feed_.enter();
    feed_.set_active(true);
    refresh_from_service();
    // The rows from the account: asked for on each visit, so "Continue
    // watching" and the subscriptions stay current.
    if (context_.service.signed_in())
    {
        context_.service.request_history();
        if (context_.service.feed(yt::FeedKind::subscriptions).state == yt::Load::idle)
            context_.service.request_subscriptions();
        extras_requested_ = true;
    }
}

void HomeScreen::refresh_from_service()
{
    const yt::FeedView view = context_.service.feed(yt::FeedKind::home);
    feed_.set_items(view.items, context_);

    // SmartTube's Home is rows: Home's own shelves (or one "Recommended"),
    // then "Continue watching" (history, started but not finished) and the
    // newest from the subscriptions.
    std::vector<yt::FeedItem> rows = view.items;
    const bool shelved = RowsView::shelf_count(rows) >= 1;
    for (yt::FeedItem &item : rows)
    {
        if (!shelved || item.shelf.empty())
            item.shelf = item.shelf.empty() && !shelved ? "Recommended" : item.shelf;
    }
    if (context_.service.signed_in())
    {
        std::size_t added = 0;
        for (yt::FeedItem item : context_.service.feed(yt::FeedKind::history).items)
        {
            if (item.watched < 0.03f || item.watched > 0.95f || added >= 20)
                continue;
            item.shelf = "Continue watching";
            rows.push_back(std::move(item));
            ++added;
        }
        added = 0;
        for (yt::FeedItem item : context_.service.feed(yt::FeedKind::subscriptions).items)
        {
            if (added >= 30)
                break;
            item.shelf = "Subscriptions";
            rows.push_back(std::move(item));
            ++added;
        }
    }
    use_rows_ = !view.items.empty() && RowsView::shelf_count(rows) >= 2;
    if (use_rows_)
        rows_.set_items(rows, context_);
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
    // Triangle loads Home again: fresh recommendations, history and
    // subscriptions rows.
    if (input.is_pressed(Action::north))
    {
        context_.service.request_home(true);
        if (context_.service.signed_in())
        {
            context_.service.request_history();
            context_.service.request_subscriptions(true);
        }
        context_.toasts.push(ui::StatusKind::info, "Refreshing Home", {}, 1.5f);
        feedback.play(audio::Cue::select);
    }
    // The account can arrive after Home opened (the saved token loads in the
    // background): ask for its rows then.
    if (!extras_requested_ && context_.service.signed_in())
    {
        extras_requested_ = true;
        context_.service.request_history();
        context_.service.request_subscriptions();
    }
    const std::uint64_t revision = context_.service.revision(yt::FeedKind::home);
    const std::uint64_t subscriptions = context_.service.revision(yt::FeedKind::subscriptions);
    const std::uint64_t history = context_.service.revision(yt::FeedKind::history);
    if (revision != seen_revision_ || subscriptions != seen_subscriptions_ ||
        history != seen_history_)
    {
        seen_revision_ = revision;
        seen_subscriptions_ = subscriptions;
        seen_history_ = history;
        refresh_from_service();
    }

    const std::size_t before = feed_.items().size();
    const yt::FeedView view = context_.service.feed(yt::FeedKind::home);
    if (use_rows_)
    {
        rows_.set_active(true);
        if (rows_.handle(input, feedback) && rows_.has_activation())
        {
            context_.nav.open_video = true;
            context_.nav.video_id = rows_.activated();
            rows_.clear_activation();
        }
        if (!more_requested_ && !view.continuation.empty() && rows_.near_end())
        {
            more_requested_ = true;
            context_.service.request_more(yt::FeedKind::home);
        }
        rows_.update(dt, context_);
        return;
    }
    feed_.set_active(true);
    if (feed_.handle(input, feedback, context_) && feed_.has_activation())
    {
        context_.nav.open_video = true;
        context_.nav.video_id = feed_.activated().video_id;
        feed_.clear_activation();
    }
    if (feed_.items().size() != before)
        more_requested_ = false;

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
    if (use_rows_)
        rows_.draw(canvas);
    else
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

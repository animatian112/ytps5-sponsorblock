// youtube-tv - Subscriptions screen.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/subscriptions_screen.hpp"

#include "app/channel_groups.hpp"
#include "app/glow.hpp"
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

constexpr float kClassicTop = 150.0f;
// The group tabs' row, and the room the grid leaves for it.
constexpr float kGroupTabsH = 56.0f;
constexpr float kGroupTabsRoom = 70.0f;
// A group showing fewer videos than this asks for more pages (two rows of
// the grid and a little more).
constexpr std::size_t kGroupFill = 12;

} // namespace

SubscriptionsScreen::SubscriptionsScreen(Context &context, yt::FeedKind kind)
    : context_(context), kind_(kind)
{
    subs_.set_bounds({kContentX, 150.0f, kContentW, 860.0f});
    subs_.set_active(true);

    sign_in_.style.theme = youtube_theme();
    sign_in_.style.role = ui::ButtonRole::primary;
    sign_in_.label = "Sign in";
    sign_in_.set_bounds({810.0f, 560.0f, 300.0f, 64.0f});
    sign_in_.set_active(true);

    // The group tabs only show; Triangle turns them (Left and Right belong
    // to the grid, L1 and R1 to the sections).
    groups_.style.theme = youtube_theme();
    groups_.style.kind = ui::TabKind::underline;
    groups_.style.on_page = true;
    groups_.style.height = kGroupTabsH;
    groups_.set_focused(false);
    refresh_groups();

    seen_revision_ = context_.service.revision(kind_);
    refresh_from_service();
}

void SubscriptionsScreen::enter()
{
    subs_.enter();
    subs_.set_active(true);
    sign_in_.set_active(true);
    refresh_from_service();
    // History changes with every video watched: fetch it fresh on each visit.
    if ((kind_ == yt::FeedKind::history || kind_ == yt::FeedKind::library) &&
        context_.service.signed_in())
        request();
}

bool SubscriptionsScreen::has_groups() const
{
    return kind_ == yt::FeedKind::subscriptions && context_.channel_groups != nullptr &&
           !context_.channel_groups->empty();
}

float SubscriptionsScreen::grid_top() const
{
    return content_top(context_.settings, kClassicTop) + (has_groups() ? kGroupTabsRoom : 0.0f);
}

void SubscriptionsScreen::refresh_groups()
{
    if (kind_ != yt::FeedKind::subscriptions || context_.channel_groups == nullptr)
        return;
    const ChannelGroups &groups = *context_.channel_groups;
    seen_groups_ = groups.revision();
    std::vector<ui::TabItem> tabs;
    tabs.push_back({"All", 0, false, -1});
    // The group shown is found again by name: an edit can remove a group
    // before it and move it to another index.
    int found = -1;
    for (std::size_t i = 0; i < groups.groups().size(); ++i)
    {
        tabs.push_back({groups.groups()[i].name, 0, false, static_cast<int>(i)});
        if (groups.groups()[i].name == group_name_)
            found = static_cast<int>(i);
    }
    group_ = found;
    if (found < 0)
        group_name_.clear();
    groups_.set_tabs(std::move(tabs));
    groups_.set_active(group_ + 1, true);
}

void SubscriptionsScreen::refresh_from_service()
{
    const yt::FeedView view = context_.service.feed(kind_);
    if (kind_ == yt::FeedKind::library)
    {
        // Watch later and Liked videos first, then the account's playlists.
        std::vector<yt::FeedItem> items;
        yt::FeedItem later;
        later.video_id = "VLWL";
        later.title = "Watch later";
        items.push_back(later);
        yt::FeedItem liked;
        liked.video_id = "VLLL";
        liked.title = "Liked videos";
        items.push_back(liked);
        for (const yt::FeedItem &item : view.items)
        {
            if (item.is_playlist() && item.video_id != "VLWL" && item.video_id != "VLLL")
                items.push_back(item);
        }
        subs_.set_items(items, context_);
    }
    else if (group_ >= 0 && context_.channel_groups != nullptr)
    {
        // One group: the feed's own order (newest first) with only its
        // channels kept.
        subs_.set_items(
            context_.channel_groups->filter(view.items, static_cast<std::size_t>(group_)),
            context_);
    }
    else
        subs_.set_items(view.items, context_);
    if (context_.service.signed_in())
    {
        subs_.set_empty_text(kind_ == yt::FeedKind::history ? "No watch history yet"
                             : group_ >= 0                  ? "No recent videos from this group"
                                                            : "No subscriptions found");
        subs_.set_state(view.state, view.error);
    }
    else
    {
        // Not signed in: the cause is the missing account, and no text may
        // claim there are no subscriptions.
        subs_.set_empty_text(kind_ == yt::FeedKind::history ? "Sign in to see your history"
                                                            : "Sign in to see your subscriptions");
        subs_.set_state(yt::Load::idle, {});
    }
    more_requested_ = false;
}

void SubscriptionsScreen::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    clock_ += dt;
    {
        // Settings > User interface > Layout: Glow puts the content under its
        // header (the focused video's title).
        const float top = grid_top();
        if (subs_.grid().bounds().y != top)
        {
            subs_.grid().set_bounds({kContentX, top, kContentW, 1010.0f - top});
        }
        groups_.set_bounds({kContentX, content_top(context_.settings, kClassicTop) - 6.0f,
                            kContentW, kGroupTabsH});
    }

    // A group was made, renamed away or emptied from a card's menu.
    if (context_.channel_groups != nullptr && context_.channel_groups->revision() != seen_groups_)
    {
        const int before = group_;
        refresh_groups();
        if (group_ != before)
            group_pages_ = 0;
        refresh_from_service();
    }

    if (!context_.service.signed_in())
    {
        requested_ = false;
        sign_in_.set_active(true);
        if (sign_in_.handle(input, feedback) == ui::Event::activated)
            context_.nav.start_login = true;
        sign_in_.update(dt);
        return;
    }

    const std::uint64_t revision = context_.service.revision(kind_);
    if (revision != seen_revision_)
    {
        seen_revision_ = revision;
        refresh_from_service();
    }

    const yt::FeedBrief view = context_.service.feed_brief(kind_);

    // Triangle: the next group, then All again.
    if (has_groups() && input.is_pressed(Action::north))
    {
        const int count = static_cast<int>(context_.channel_groups->groups().size());
        group_ = group_ + 1 < count ? group_ + 1 : -1;
        group_name_ = group_ >= 0
                          ? context_.channel_groups->groups()[static_cast<std::size_t>(group_)].name
                          : std::string();
        groups_.set_active(group_ + 1);
        group_pages_ = 0;
        feedback.play(audio::Cue::tab);
        refresh_from_service();
        subs_.set_focus(0);
    }

    // First load once the account is known.
    if (!requested_ && view.state == yt::Load::idle)
    {
        requested_ = true;
        request();
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

    if (!more_requested_ && view.state == yt::Load::ready && view.more && subs_.at_last_row())
    {
        more_requested_ = true;
        context_.service.request_more(kind_);
    }
    // A group with few videos in the pages loaded so far: fetch further back
    // without waiting for the viewer to reach a last row that is not there.
    if (!more_requested_ && group_ >= 0 && view.state == yt::Load::ready && view.more &&
        subs_.items().size() < kGroupFill && group_pages_ < kGroupPages)
    {
        more_requested_ = true;
        ++group_pages_;
        context_.service.request_more(kind_);
    }

    groups_.update(dt);
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
        ui::text(canvas.list, context.fonts.semibold, title(), 960.0f, 400.0f, 40.0f,
                 gfx::Color::rgb(0xf1f1f1), gfx::Align::center);
        ui::text(canvas.list, context.fonts.regular,
                 kind_ == yt::FeedKind::history   ? "Sign in to see your watch history."
                 : kind_ == yt::FeedKind::library ? "Sign in to see your playlists."
                                                  : "Sign in to see your subscriptions.",
                 960.0f, 470.0f, 24.0f, gfx::Color::rgb(0xaaaaaa), gfx::Align::center);
        sign_in_.draw(canvas);
        return;
    }

    if (has_groups())
        groups_.draw(canvas);
    subs_.draw(canvas, context);
}

std::span<const TourStep> SubscriptionsScreen::tour() const
{
    return kTour;
}

void SubscriptionsScreen::request()
{
    if (kind_ == yt::FeedKind::history)
        context_.service.request_history();
    else if (kind_ == yt::FeedKind::library)
        context_.service.request_library();
    else
        context_.service.request_subscriptions();
}

std::unique_ptr<Screen> make_subscriptions_screen(Context &context)
{
    return std::make_unique<SubscriptionsScreen>(context);
}

std::unique_ptr<Screen> make_playlists_screen(Context &context)
{
    return std::make_unique<SubscriptionsScreen>(context, yt::FeedKind::library);
}

std::unique_ptr<Screen> make_history_screen(Context &context)
{
    return std::make_unique<SubscriptionsScreen>(context, yt::FeedKind::history);
}

} // namespace hui::app

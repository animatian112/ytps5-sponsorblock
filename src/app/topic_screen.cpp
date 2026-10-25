// youtube-tv - A topic section of the rail.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/topic_screen.hpp"

#include "app/screens.hpp"
#include "app/glow.hpp"
#include "app/theme.hpp"
#include "yt/service.hpp"

#include <memory>

namespace hui::app
{

TopicScreen::TopicScreen(Context &context, const char *id, const char *title, std::string browse_id)
    : context_(context), id_(id), title_(title), browse_id_(std::move(browse_id))
{
    grid_.set_bounds({kContentX, 150.0f, kContentW, 860.0f});
    grid_.set_empty_text("Nothing here right now");
    rows_.set_bounds({kContentX, 140.0f, kContentW, 880.0f});
    shorts_ = browse_id_ == "FEshorts";
    if (shorts_)
        grid_.set_portrait(true); // SmartTube's Shorts grid: tall cards
}

bool TopicScreen::mine() const
{
    return context_.service.topic_id() == browse_id_;
}

std::unique_ptr<TopicScreen> TopicScreen::playlist_page(Context &context)
{
    auto screen = std::make_unique<TopicScreen>(context, "playlist", "Playlist", std::string());
    screen->playlist_page_ = true;
    return screen;
}

void TopicScreen::enter()
{
    if (playlist_page_ && !context_.pending_browse.empty())
    {
        browse_id_ = context_.pending_browse;
        context_.pending_browse.clear();
        title_ = browse_id_ == "VLWL"                     ? "Watch later"
                 : browse_id_ == "VLLL"                   ? "Liked videos"
                 : !context_.pending_browse_title.empty() ? context_.pending_browse_title
                                                          : "Playlist";
        context_.pending_browse_title.clear();
    }
    // One slot serves every topic: ask for this one unless it already holds it.
    const yt::FeedView view = context_.service.feed(yt::FeedKind::topic);
    if (!mine() || view.state == yt::Load::idle || view.state == yt::Load::failed)
        context_.service.request_topic(browse_id_);
    seen_revision_ = 0;
    grid_.enter();
    refresh();
}

void TopicScreen::refresh()
{
    if (!mine())
    {
        grid_.set_items({}, context_);
        grid_.set_state(yt::Load::loading, {});
        use_rows_ = false;
        return;
    }
    const yt::FeedView view = context_.service.feed(yt::FeedKind::topic);
    grid_.set_items(view.items, context_);
    grid_.set_state(view.state, view.error);
    use_rows_ = !shorts_ && RowsView::shelf_count(view.items) >= 1;
    if (use_rows_)
        rows_.set_items(view.items, context_);
    more_requested_ = false;
}

void TopicScreen::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    clock_ += dt;
    {
        // Settings > User interface > Layout: Glow puts the content under its
        // header (the focused video's title).
        const float top = content_top(context_.settings, 150.0f);
        if (grid_.grid().bounds().y != top)
        {
            grid_.grid().set_bounds({kContentX, top, kContentW, 1010.0f - top});
            const float rows_top = content_top(context_.settings, 140.0f);
            rows_.set_bounds({kContentX, rows_top, kContentW, 1020.0f - rows_top});
        }
    }
    if (playlist_page_ && input.is_pressed(Action::back))
    {
        context_.nav.leave_browse = true;
        return;
    }
    const std::uint64_t revision = context_.service.revision(yt::FeedKind::topic);
    if (revision != seen_revision_)
    {
        seen_revision_ = revision;
        refresh();
    }
    const yt::FeedBrief view = context_.service.feed_brief(yt::FeedKind::topic);
    bool at_end = false;
    if (use_rows_)
    {
        rows_.set_active(true);
        if (rows_.handle(input, feedback) && rows_.has_activation())
        {
            context_.nav.open_video = true;
            context_.nav.video_id = rows_.activated();
            rows_.clear_activation();
        }
        at_end = rows_.near_end();
        rows_.update(dt, context_);
    }
    else
    {
        grid_.set_active(true);
        if (grid_.handle(input, feedback, context_) && grid_.has_activation())
        {
            context_.nav.open_video = true;
            context_.nav.video_id = grid_.activated().video_id;
            grid_.clear_activation();
        }
        at_end = grid_.at_last_row();
        grid_.update(dt, context_);
    }
    if (mine() && !more_requested_ && view.state == yt::Load::ready && view.more && at_end)
    {
        more_requested_ = true;
        context_.service.request_more(yt::FeedKind::topic);
    }
}

void TopicScreen::draw(const Context &context, Frame &frame) const
{
    ui::Canvas canvas{frame.scene, context.fonts, frame.glass_texture, clock_};
    // SmartTube names the section in its sidebar only; a playlist page keeps
    // its title.
    if (playlist_page_)
        ui::text(canvas.list, context.fonts.semibold, title_.c_str(), kContentX, kSectionTitleY,
                 kSectionTitleSize, gfx::Color::rgb(0xf1f1f1));
    if (use_rows_)
        rows_.draw(canvas);
    else
        grid_.draw(canvas, context);
}

std::unique_ptr<Screen> make_browse_screen(Context &context)
{
    return TopicScreen::playlist_page(context);
}

std::unique_ptr<Screen> make_topic_screen(Context &context, const char *id, const char *title,
                                          const char *browse_id)
{
    return std::make_unique<TopicScreen>(context, id, title, browse_id);
}

} // namespace hui::app

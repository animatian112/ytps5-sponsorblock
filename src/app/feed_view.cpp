// youtube-tv - The shared feed grid.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/feed_view.hpp"

#include "app/branding.hpp"
#include "app/theme.hpp"
#include "app/thumbs.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <cstdio>

namespace hui::app
{
namespace
{

const gfx::Color kInk = gfx::Color::rgb(0xf1f1f1);
const gfx::Color kMuted = gfx::Color::rgb(0xaaaaaa);

} // namespace

std::string format_duration(double seconds)
{
    if (seconds <= 0.0)
        return {};
    const int total = static_cast<int>(seconds + 0.5);
    char text[32];
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;
    if (hours > 0)
        std::snprintf(text, sizeof(text), "%d:%02d:%02d", hours, minutes, secs);
    else
        std::snprintf(text, sizeof(text), "%d:%02d", minutes, secs);
    return text;
}

std::string card_thumbnail(const yt::FeedItem &item)
{
    if (item.video_id.size() == 11)
        return "https://i.ytimg.com/vi/" + item.video_id + "/mqdefault.jpg";
    return item.thumbnail_url;
}

FeedView::FeedView()
{
    // SmartTube's grid: five thumbnails a row, the duration bottom right.
    grid_.style.theme = card_theme();
    grid_.style.columns = 4;
    grid_.style.gap_x = 30.0f;
    grid_.style.gap_y = 30.0f;
    grid_.style.card.art_aspect = 16.0f / 9.0f;
    grid_.style.card.text = ui::CardText::below;
    grid_.style.card.title_size = 21.0f;
    grid_.style.card.subtitle_size = 18.0f;
    grid_.style.card.text_gap = 10.0f;
    grid_.style.card.badge_corner = ui::CardCorner::bottom_right;
    grid_.style.card.badge_size = 15.0f;
    grid_.style.card.focus_scale = 1.1f;
    grid_.style.card.info_box = true; // SmartTube's cards: the text in a box under the art
    grid_.style.card.title_lines = 2;
    grid_.style.card.radius = 6.0f;
    grid_.style.card.ring = false;
    grid_.style.card.info_color = gfx::Color::rgb(0x1f1f1f);
    grid_.style.card.info_focus = gfx::Color::rgb(0xe3e3e3); // the focused card's box lights up
    grid_.style.card.info_focus_text = gfx::Color::rgb(0x4a4a4a);
    grid_.style.card.title_size = 24.0f;
    grid_.style.card.subtitle_size = 19.0f;

    grid_.style.pitch_by_row = true;
    grid_.style.exits.up = true;
    // Left at the first column leaves the grid instead of doing nothing, so
    // the rail can take the focus.
    grid_.style.exits.left = true;
}

void FeedView::set_portrait(bool portrait)
{
    portrait_ = portrait;
    grid_.style.columns = portrait ? 6 : 4;
    grid_.style.card.art_aspect = portrait ? 9.0f / 16.0f : 16.0f / 9.0f;
    grid_.style.card.subtitle_size = portrait ? 17.0f : 19.0f;
    grid_.style.card.title_size = portrait ? 21.0f : 24.0f;
}

std::string FeedView::thumbnail_for(const yt::FeedItem &item) const
{
    if (portrait_ && !item.thumbnail_url.empty())
        return item.thumbnail_url;
    if (portrait_ && item.video_id.size() == 11)
        return "https://i.ytimg.com/vi/" + item.video_id + "/oar2.jpg";
    return card_thumbnail(item);
}

ui::GridView &FeedView::grid()
{
    return grid_;
}

void FeedView::set_bounds(const gfx::Rect &bounds)
{
    grid_.set_bounds(bounds);
}

void FeedView::set_items(const std::vector<yt::FeedItem> &items, const Context &context)
{
    const int previous_focus = grid_.focus();
    items_ = items;
    std::vector<ui::CardItem> cards;
    cards.reserve(items.size());
    for (const yt::FeedItem &item : items)
    {
        ui::CardItem card;
        card.title = item.title.empty() ? item.video_id : item.title;
        // SmartTube's info line: channel, views and age.
        card.subtitle = item.author;
        if (!item.detail.empty())
            card.subtitle += (card.subtitle.empty() ? "" : "  \xE2\x80\xA2  ") + item.detail;
        card.progress = item.watched >= 0.0f ? std::max(item.watched, 0.03f) : -1.0f;
        card.texture = context.thumbs.texture(thumbnail_for(item));
        card.image_aspect = 16.0f / 9.0f;
        if (portrait_)
        {
            const float aspect = context.thumbs.aspect(thumbnail_for(item));
            card.image_aspect = aspect > 0.0f ? aspect : 9.0f / 16.0f;
        }
        if (item.live)
            card.badge = "LIVE";
        else
            card.badge = item.length_s > 0.0 ? format_duration(item.length_s) : item.badge;
        cards.push_back(std::move(card));
    }
    grid_.set_items(std::move(cards));
    if (previous_focus >= 0 && previous_focus < grid_.count())
        grid_.set_focus(previous_focus, true);
}

void FeedView::set_state(yt::Load state, const std::string &error)
{
    state_ = state;
    error_ = error;
}

void FeedView::set_empty_text(std::string text)
{
    empty_text_ = std::move(text);
}

bool FeedView::handle(const InputFrame &input, ui::Feedback &feedback, const Context &context)
{
    if (state_ != yt::Load::ready && items_.empty())
        return false;
    const ui::Event event = grid_.handle(input, feedback);
    if (event == ui::Event::activated)
    {
        const int focus = grid_.focus();
        if (focus >= 0 && focus < static_cast<int>(items_.size()))
        {
            activated_ = focus;
            activated_item_ = items_[static_cast<std::size_t>(focus)];
        }
    }
    refresh_thumbnails(context);
    return event != ui::Event::none;
}

void FeedView::update(float dt, const Context &context)
{
    // Settings > User interface: cards a row, title lines, corners.
    const Settings &settings = context.settings;
    if (!portrait_)
        grid_.style.columns = settings.grid_columns;
    grid_.style.card.title_lines = settings.title_lines;
    grid_.style.card.radius = settings.rounded_cards ? 6.0f : 0.0f;
    // Refresh visible thumbnails even without input (the grid may not call
    // handle() when it is inactive).
    refresh_thumbnails(context);
    grid_.update(dt);
}

void FeedView::refresh_thumbnails(const Context &context)
{
    // Only the rows around the focus ask for their pictures (a grid of
    // several pages would otherwise queue every one of them ahead of the
    // cards in view), and they ask every frame, so a texture the cache
    // evicted is never drawn under a card that no longer owns it.
    const int columns = std::max(1, grid_.style.columns);
    const int count = std::min(grid_.count(), static_cast<int>(items_.size()));
    const int focus = std::max(0, grid_.focus());
    const int first = std::max(0, (focus / columns - 2) * columns);
    const int last = std::min(count, (focus / columns + 4) * columns);
    for (int index = first; index < last; ++index)
    {
        const yt::FeedItem &item = items_[static_cast<std::size_t>(index)];
        const std::string url = thumbnail_for(item);
        ui::CardItem &card = grid_.item(index);
        // DeArrow (Settings > SponsorBlock): the community title, and its
        // thumbnail once that picture is in (YouTube's until then).
        const CardBrand brand = card_brand(item.video_id, context.settings);
        if (!brand.title.empty() && card.title != brand.title)
            card.title = brand.title;
        if (!brand.thumbnail_url.empty() && !portrait_)
        {
            const std::uint32_t branded = context.thumbs.texture(brand.thumbnail_url);
            if (branded != 0)
            {
                card.texture = branded;
                continue;
            }
        }
        card.texture = context.thumbs.texture(url);
        if (portrait_ && card.texture != 0)
        {
            const float aspect = context.thumbs.aspect(url);
            if (aspect > 0.0f)
                card.image_aspect = aspect;
        }
    }
}

void FeedView::draw(ui::Canvas &canvas, const Context &context) const
{
    if (items_.empty())
    {
        const gfx::Rect &bounds = grid_.bounds();
        std::string text;
        if (state_ == yt::Load::loading)
            text = "Loading...";
        else if (state_ == yt::Load::failed)
            text = error_.empty() ? "No connection" : error_;
        else
            text = empty_text_.empty() ? "Nothing here" : empty_text_;
        ui::text(canvas.list, context.fonts.regular, text, bounds.cx(), bounds.y + 60.0f, 30.0f,
                 kMuted, gfx::Align::center);
        return;
    }
    grid_.draw(canvas);
    if (state_ == yt::Load::loading && !items_.empty())
    {
        // A small note that more rows are on the way.
        ui::text(canvas.list, context.fonts.regular, "Loading more...", grid_.bounds().cx(),
                 grid_.bounds().y + grid_.bounds().h - 18.0f, 22.0f, kMuted, gfx::Align::center);
    }
}

bool FeedView::wants_rail_focus() const
{
    return grid_.exit() == Direction::left;
}

bool FeedView::at_last_row() const
{
    if (grid_.count() <= 0)
        return false;
    const int columns = grid_.columns() > 0 ? grid_.columns() : 1;
    const int last_row = (grid_.count() - 1) / columns;
    return grid_.focus() / columns >= last_row;
}

void FeedView::set_active(bool active)
{
    grid_.set_active(active);
}

void FeedView::enter()
{
    grid_.enter();
}

int FeedView::focus_index() const
{
    return grid_.focus();
}

void FeedView::set_focus(int index)
{
    // Snap, so the cell's row is scrolled into view at once.
    grid_.set_focus(index);
}

float FeedView::smooth_focus() const
{
    return static_cast<float>(grid_.focus());
}

} // namespace hui::app

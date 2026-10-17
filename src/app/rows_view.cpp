// youtube-tv - A feed as rows of shelves.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/rows_view.hpp"

#include "app/feed_view.hpp"
#include "app/theme.hpp"
#include "app/thumbs.hpp"

#include <algorithm>
#include <cmath>

namespace hui::app
{
namespace
{

constexpr float kItemWidth = 400.0f;
constexpr float kRowGap = 18.0f;

void style_shelf(ui::Carousel &shelf)
{
    shelf.style.theme = card_theme();
    shelf.style.mode = ui::CarouselMode::leading;
    shelf.style.item_width = kItemWidth;
    shelf.style.gap = 30.0f;
    shelf.style.peek = 0.0f;
    shelf.style.counter = false;
    shelf.style.title_size = 36.0f;
    shelf.style.title_muted = false; // SmartTube's row titles are white
    shelf.style.title_gap = 14.0f;
    shelf.style.edge_fade = 0.0f;
    shelf.style.exits.left = true;
    shelf.style.card.art_aspect = 16.0f / 9.0f;
    shelf.style.card.text = ui::CardText::below;
    shelf.style.card.title_size = 21.0f;
    shelf.style.card.subtitle_size = 18.0f;
    shelf.style.card.text_gap = 10.0f;
    shelf.style.card.badge_corner = ui::CardCorner::bottom_right;
    shelf.style.card.badge_size = 15.0f;
    shelf.style.card.focus_scale = 1.1f; // Leanback's zoom on the focused card
    shelf.style.card.info_box = true;    // SmartTube's cards: the text in a box under the art
    shelf.style.card.title_lines = 2;
    shelf.style.card.radius = 6.0f;
    shelf.style.card.ring = false;
    shelf.style.card.info_color = gfx::Color::rgb(0x1f1f1f);
    shelf.style.card.info_focus = gfx::Color::rgb(0xe3e3e3); // the focused card's box lights up
    shelf.style.card.info_focus_text = gfx::Color::rgb(0x4a4a4a);
    shelf.style.card.title_size = 24.0f;
    shelf.style.card.subtitle_size = 19.0f;
}

ui::CardItem card_for(const yt::FeedItem &item, const Context &context)
{
    ui::CardItem card;
    card.title = item.title.empty() ? item.video_id : item.title;
    card.subtitle = item.author;
    if (!item.detail.empty())
        card.subtitle += (card.subtitle.empty() ? "" : "  \xE2\x80\xA2  ") + item.detail;
    card.texture = context.thumbs.texture(card_thumbnail(item));
    card.image_aspect = 16.0f / 9.0f;
    card.badge = item.live ? std::string("LIVE") : format_duration(item.length_s);
    card.progress = item.watched >= 0.0f ? std::max(item.watched, 0.03f) : -1.0f;
    return card;
}

} // namespace

int RowsView::shelf_count(const std::vector<yt::FeedItem> &items)
{
    int count = 0;
    std::string last = "\x01";
    for (const yt::FeedItem &item : items)
    {
        if (!item.shelf.empty() && item.shelf != last)
        {
            ++count;
            last = item.shelf;
        }
    }
    return count;
}

void RowsView::set_bounds(const gfx::Rect &bounds)
{
    bounds_ = bounds;
    place();
}

float RowsView::row_height() const
{
    if (rows_.empty())
        return 300.0f;
    return rows_.front()->shelf.preferred_height() + kRowGap;
}

void RowsView::set_items(const std::vector<yt::FeedItem> &items, const Context &context)
{
    // Remember where the focus was: the row (its title and which occurrence
    // of that title) and the card.
    std::string focus_title;
    int focus_occurrence = 0;
    int focus_card = 0;
    if (focus_row_ >= 0 && focus_row_ < static_cast<int>(rows_.size()))
    {
        focus_title = rows_[static_cast<std::size_t>(focus_row_)]->title;
        focus_card = rows_[static_cast<std::size_t>(focus_row_)]->shelf.focus();
        for (int i = 0; i < focus_row_; ++i)
            if (rows_[static_cast<std::size_t>(i)]->title == focus_title)
                ++focus_occurrence;
    }

    // Group the items into rows (titles and items only).
    struct Group
    {
        std::string title;
        std::vector<yt::FeedItem> items;
    };
    std::vector<Group> groups;
    for (const yt::FeedItem &item : items)
    {
        const std::string &title = item.shelf;
        if (groups.empty() || (!title.empty() && title != groups.back().title))
            groups.push_back({title.empty() ? std::string("Recommended") : title, {}});
        groups.back().items.push_back(item);
    }

    // A row that is unchanged (same title, same videos) keeps its carousel:
    // its focus and glide stay put while later pages arrive around it.
    const auto same = [](const Row &row, const Group &group)
    {
        if (row.title != group.title || row.items.size() != group.items.size())
            return false;
        for (std::size_t i = 0; i < row.items.size(); ++i)
            if (row.items[i].video_id != group.items[i].video_id)
                return false;
        return true;
    };
    std::vector<std::unique_ptr<Row>> rows;
    for (std::size_t g = 0; g < groups.size(); ++g)
    {
        std::unique_ptr<Row> reused;
        for (auto &old : rows_)
        {
            if (old != nullptr && same(*old, groups[g]))
            {
                reused = std::move(old);
                break;
            }
        }
        if (reused != nullptr)
        {
            rows.push_back(std::move(reused));
            continue;
        }
        auto row = std::make_unique<Row>();
        row->title = groups[g].title;
        row->items = std::move(groups[g].items);
        style_shelf(row->shelf);
        row->shelf.title = row->title;
        std::vector<ui::CardItem> cards;
        cards.reserve(row->items.size());
        for (const yt::FeedItem &item : row->items)
            cards.push_back(card_for(item, context));
        row->shelf.set_items(std::move(cards));
        rows.push_back(std::move(row));
    }
    rows_ = std::move(rows);

    const int before = focus_row_;
    focus_row_ = 0;
    int seen = 0;
    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        if (focus_title.empty() || rows_[i]->title != focus_title)
            continue;
        if (seen++ < focus_occurrence)
            continue;
        focus_row_ = static_cast<int>(i);
        const int count = static_cast<int>(rows_[i]->items.size());
        rows_[i]->shelf.set_focus(std::min(focus_card, std::max(0, count - 1)));
        break;
    }
    // The view stays where it was when the focus row kept its place.
    if (focus_row_ != before)
        scroll_ = static_cast<float>(focus_row_) * row_height();
    place();
}

void RowsView::place()
{
    const float pitch = row_height();
    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        Row &row = *rows_[i];
        // Fixed places; the vertical scroll is a transform at draw time, so
        // set_bounds (which snaps a shelf's glide) runs only on a new layout.
        const float y = bounds_.y + static_cast<float>(i) * pitch;
        row.shelf.set_bounds({bounds_.x, y, bounds_.w, pitch - kRowGap});
        row.shelf.set_active(active_ && static_cast<int>(i) == focus_row_);
    }
}

void RowsView::set_active(bool active)
{
    active_ = active;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        rows_[i]->shelf.set_active(active_ && static_cast<int>(i) == focus_row_);
}

bool RowsView::handle(const InputFrame &input, ui::Feedback &feedback)
{
    wants_rail_ = false;
    if (rows_.empty())
        return false;
    focus_row_ = std::clamp(focus_row_, 0, static_cast<int>(rows_.size()) - 1);
    Row &row = *rows_[static_cast<std::size_t>(focus_row_)];
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int next = focus_row_ + (input.nav == Direction::down ? 1 : -1);
        if (next < 0 || next >= static_cast<int>(rows_.size()))
        {
            feedback.play(audio::Cue::error);
            return false;
        }
        // The new row takes the card that is on screen under (or over) the
        // focused one, as Leanback does: rows scroll apart, so the same index
        // can sit a card to the left or right.
        const float x = row.shelf.item_rect(row.shelf.focus()).cx();
        focus_row_ = next;
        Row &target = *rows_[static_cast<std::size_t>(next)];
        int pick = 0;
        float best = 1e9f;
        for (int i = 0; i < static_cast<int>(target.items.size()); ++i)
        {
            const float distance = std::fabs(target.shelf.item_rect(i).cx() - x);
            if (distance < best)
            {
                best = distance;
                pick = i;
            }
        }
        target.shelf.set_focus(pick, false);
        feedback.play(audio::Cue::focus);
        for (std::size_t i = 0; i < rows_.size(); ++i)
            rows_[i]->shelf.set_active(active_ && static_cast<int>(i) == focus_row_);
        return true;
    }
    const ui::Event event = row.shelf.handle(input, feedback);
    if (row.shelf.exit() == Direction::left)
        wants_rail_ = true;
    if (event == ui::Event::activated)
    {
        const int index = row.shelf.focus();
        if (index >= 0 && index < static_cast<int>(row.items.size()))
            activated_ = row.items[static_cast<std::size_t>(index)].video_id;
    }
    return event != ui::Event::none;
}

const yt::FeedItem *RowsView::focused_item() const
{
    if (focus_row_ < 0 || focus_row_ >= static_cast<int>(rows_.size()))
        return nullptr;
    const Row &row = *rows_[static_cast<std::size_t>(focus_row_)];
    const int index = row.shelf.focus();
    if (index < 0 || index >= static_cast<int>(row.items.size()))
        return nullptr;
    return &row.items[static_cast<std::size_t>(index)];
}

bool RowsView::near_end() const
{
    return !rows_.empty() && focus_row_ >= static_cast<int>(rows_.size()) - 2;
}

void RowsView::update(float dt, const Context &context)
{
    // The focus row sits at the top of the view; the rows glide there.
    const float target = static_cast<float>(focus_row_) * row_height();
    scroll_ += (target - scroll_) * (1.0f - std::exp(-14.0f * dt));
    if (std::fabs(target - scroll_) < 0.5f)
        scroll_ = target;
    for (auto &row : rows_)
    {
        for (std::size_t i = 0; i < row->items.size(); ++i)
        {
            ui::CardItem &card = row->shelf.item(static_cast<int>(i));
            if (card.texture == 0)
                card.texture = context.thumbs.texture(card_thumbnail(row->items[i]));
        }
        row->shelf.update(dt);
    }
}

void RowsView::draw(ui::Canvas &canvas) const
{
    // The focused card grows past its row: the clip leaves room above.
    canvas.list.push_clip({0.0f, bounds_.y - 20.0f, gfx::kVirtualWidth, bounds_.h + 20.0f});
    canvas.list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, -scroll_);
    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        const auto &row = rows_[i];
        const gfx::Rect &at = row->shelf.bounds();
        if (at.y + at.h - scroll_ < bounds_.y - 40.0f || at.y - scroll_ > bounds_.y + bounds_.h)
            continue;
        // Leanback dims every row but the one with the focus.
        const bool lit = !active_ || static_cast<int>(i) == focus_row_;
        canvas.list.push_opacity(lit ? 1.0f : 0.85f);
        row->shelf.draw(canvas);
        canvas.list.pop_opacity();
    }
    canvas.list.pop_transform();
    canvas.list.pop_clip();
}

} // namespace hui::app

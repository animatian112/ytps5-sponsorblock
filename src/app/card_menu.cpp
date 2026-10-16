// youtube-tv - The menu a video card opens when Cross is held (SmartTube's).
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/card_menu.hpp"

#include <algorithm>

namespace hui::app
{
namespace
{

const gfx::Color kInk = gfx::Color::rgb(0xffffff);
const gfx::Color kMuted = gfx::Color::rgb(0xaaaaaa);
const gfx::Color kDark = gfx::Color::rgb(0x111111);
constexpr float kWidth = 760.0f;
constexpr float kRow = 72.0f;
constexpr float kHeader = 120.0f;
constexpr int kMaxVisible = 9;

} // namespace

void CardMenu::open(std::string title, std::vector<Entry> entries)
{
    title_ = std::move(title);
    entries_ = std::move(entries);
    focus_ = 0;
    active_ = !entries_.empty();
}

void CardMenu::close()
{
    active_ = false;
}

int CardMenu::handle(const InputFrame &input, ui::Feedback &feedback)
{
    if (!active_)
        return -1;
    if (input.is_pressed(Action::back))
    {
        active_ = false;
        feedback.play(audio::Cue::back);
        return -1;
    }
    const int count = static_cast<int>(entries_.size());
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int next = focus_ + (input.nav == Direction::down ? 1 : -1);
        if (next >= 0 && next < count)
        {
            focus_ = next;
            feedback.play(audio::Cue::focus);
        }
        return -1;
    }
    if (input.is_pressed(Action::confirm) && focus_ >= 0 && focus_ < count)
    {
        active_ = false;
        feedback.play(audio::Cue::select);
        return entries_[static_cast<std::size_t>(focus_)].id;
    }
    return -1;
}

void CardMenu::update(float dt)
{
    const float target = active_ ? 1.0f : 0.0f;
    const float step = dt * 8.0f;
    shown_ = shown_ < target ? std::min(target, shown_ + step) : std::max(target, shown_ - step);
}

void CardMenu::draw(ui::Canvas &canvas, const ui::Fonts &fonts) const
{
    if (shown_ < 0.01f || fonts.regular.font == nullptr || fonts.semibold.font == nullptr)
        return;
    gfx::DrawList &list = canvas.list;
    list.push_opacity(shown_);
    list.rounded_rect({0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0.0f,
                      gfx::Color::rgb(0x000000, 0.6f));
    const int count = static_cast<int>(entries_.size());
    const int visible = std::min(count, kMaxVisible);
    const float height = kHeader + static_cast<float>(visible) * kRow + 32.0f;
    const float x = (gfx::kVirtualWidth - kWidth) * 0.5f;
    const float y = (gfx::kVirtualHeight - height) * 0.5f + (1.0f - shown_) * 24.0f;
    list.rounded_rect({x, y, kWidth, height}, 18.0f, gfx::Color::rgb(0x212121));
    ui::text(list, fonts.semibold, fonts.semibold.font->fit(title_, 30.0f, kWidth - 80.0f),
             x + 40.0f, y + 66.0f, 30.0f, kInk);
    list.rounded_rect({x + 40.0f, y + kHeader - 26.0f, kWidth - 80.0f, 2.0f}, 0.0f,
                      gfx::Color::rgb(0x3a3a3a));
    const int first = std::clamp(focus_ - visible / 2, 0, std::max(0, count - visible));
    for (int i = first; i < count && i < first + visible; ++i)
    {
        const float row_y = y + kHeader + static_cast<float>(i - first) * kRow;
        const bool focused = i == focus_;
        const gfx::Rect row{x + 20.0f, row_y, kWidth - 40.0f, kRow - 8.0f};
        if (focused)
            list.rounded_rect(row, 12.0f, kInk);
        const std::string &label = entries_[static_cast<std::size_t>(i)].label;
        ui::text(list, fonts.regular, fonts.regular.font->fit(label, 28.0f, row.w - 60.0f),
                 row.x + 30.0f, row.cy() + 10.0f, 28.0f, focused ? kDark : kInk);
    }
    if (count > visible)
        ui::text(list, fonts.regular, std::to_string(focus_ + 1) + " / " + std::to_string(count),
                 x + kWidth - 40.0f, y + 66.0f, 22.0f, kMuted, gfx::Align::right);
    list.pop_opacity();
}

} // namespace hui::app

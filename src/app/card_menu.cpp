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
constexpr float kWidth = 720.0f;
constexpr float kRow = 88.0f;
constexpr float kHeader = 170.0f;

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
    const float step = dt * 6.0f;
    shown_ = shown_ < target ? std::min(target, shown_ + step) : std::max(target, shown_ - step);
}

void CardMenu::draw(ui::Canvas &canvas, const ui::Fonts &fonts) const
{
    if (shown_ < 0.01f || fonts.regular.font == nullptr || fonts.semibold.font == nullptr)
        return;
    // SmartTube's dialogs: a full-height panel from the right, a header band
    // with the title, one full-width row per choice.
    gfx::DrawList &list = canvas.list;
    const float x = gfx::kVirtualWidth - kWidth * shown_;
    list.push_opacity(std::min(1.0f, shown_ * 1.4f));
    list.rounded_rect({0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0.0f,
                      gfx::Color::rgb(0x000000, 0.45f));
    list.rounded_rect({x, 0.0f, kWidth, gfx::kVirtualHeight}, 0.0f,
                      gfx::Color::rgb(0x2f4156, 0.98f)); // SmartTube's slate dialog
    list.rounded_rect({x, 0.0f, kWidth, kHeader}, 0.0f, gfx::Color::rgb(0x2a3a4d));
    ui::text(list, fonts.semibold, fonts.semibold.font->fit(title_, 34.0f, kWidth - 112.0f),
             x + 56.0f, 100.0f, 34.0f, kInk);
    const int count = static_cast<int>(entries_.size());
    const int visible =
        std::max(1, static_cast<int>((gfx::kVirtualHeight - kHeader - 24.0f) / kRow));
    const int first = std::clamp(focus_ - visible / 2, 0, std::max(0, count - visible));
    for (int i = first; i < count && i < first + visible; ++i)
    {
        const gfx::Rect row{x, kHeader + static_cast<float>(i - first) * kRow, kWidth, kRow};
        if (i == focus_)
            list.rounded_rect(row, 0.0f, gfx::Color::rgb(0x5a7391));
        const std::string &label = entries_[static_cast<std::size_t>(i)].label;
        ui::text(list, fonts.regular, fonts.regular.font->fit(label, 28.0f, kWidth - 112.0f),
                 x + 56.0f, row.cy() + 10.0f, 28.0f, kInk);
    }
    if (count > visible)
        ui::text(list, fonts.regular, std::to_string(focus_ + 1) + " / " + std::to_string(count),
                 x + kWidth - 48.0f, gfx::kVirtualHeight - 36.0f, 20.0f, kMuted, gfx::Align::right);
    list.pop_opacity();
}

} // namespace hui::app

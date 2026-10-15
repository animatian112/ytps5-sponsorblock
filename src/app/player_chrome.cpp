// youtube-tv - The player's controls in SmartTube's layout.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/player_chrome.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace hui::app
{
namespace
{

constexpr float kLeft = 96.0f;
constexpr float kRight = 1824.0f;
constexpr float kTitleY = 842.0f;
constexpr float kSubtitleY = 884.0f;
constexpr float kTrackY = 922.0f;
constexpr float kTimeY = 968.0f;
constexpr float kButtonY = 1016.0f;
constexpr float kButtonRadius = 30.0f;
constexpr float kButtonPitch = 80.0f;
constexpr float kSkipHeight = 56.0f;
constexpr float kHideAfter = 4.0f; // seconds without input while playing
constexpr double kSeekStep = 10.0;
constexpr float kQuarter = 1.5707963f;

const gfx::Color kInk = gfx::Color::rgb(0xffffff);
const gfx::Color kMuted = gfx::Color::rgb(0xc8c8c8);
const gfx::Color kRed = gfx::Color::rgb(0xff0033);
const gfx::Color kDark = gfx::Color::rgb(0x111111);

std::string clock_text(double seconds)
{
    if (!(seconds > 0.0))
        seconds = 0.0;
    const int total = static_cast<int>(seconds);
    char text[32];
    if (total >= 3600)
        std::snprintf(text, sizeof(text), "%d:%02d:%02d", total / 3600, (total % 3600) / 60,
                      total % 60);
    else
        std::snprintf(text, sizeof(text), "%d:%02d", total / 60, total % 60);
    return text;
}

float approach(float value, float target, float rate, float dt)
{
    const float k = 1.0f - std::exp(-rate * dt);
    return value + (target - value) * k;
}

void triangle(gfx::DrawList &list, float cx, float cy, float size, float dir, gfx::Color c)
{
    list.triangle({cx - size * 0.5f, cy - size * 0.5f, size, size}, c, 0.0f, kQuarter * dir);
}

float skip_width(const ui::Fonts &fonts, const std::string &text)
{
    return (fonts.semibold.font != nullptr ? fonts.semibold.measure(text, 24.0f)
                                           : 13.0f * static_cast<float>(text.size())) +
           56.0f;
}

} // namespace

void PlayerChrome::set_times(double position, double duration, double buffered)
{
    position_ = position;
    duration_ = duration;
    buffered_ = buffered;
}

void PlayerChrome::set_playing(bool playing)
{
    if (playing_ && !playing)
        show(); // a pause, from anywhere, brings the controls up
    playing_ = playing;
}

void PlayerChrome::set_ranges(std::vector<ui::MediaRange> ranges)
{
    ranges_ = std::move(ranges);
}

void PlayerChrome::set_info(std::string title, std::string subtitle, std::string quality)
{
    title_ = std::move(title);
    subtitle_ = std::move(subtitle);
    quality_ = std::move(quality);
}

void PlayerChrome::set_has_next(bool has_next)
{
    has_next_ = has_next;
}

void PlayerChrome::set_segment(std::string label)
{
    segment_ = std::move(label);
    if (segment_.empty() && focus_ == Button::skip)
        focus_ = Button::play;
}

std::vector<PlayerChrome::Button> PlayerChrome::buttons() const
{
    std::vector<Button> out{Button::restart, Button::rewind, Button::play, Button::forward};
    if (has_next_)
        out.push_back(Button::next);
    if (!segment_.empty())
        out.push_back(Button::skip);
    return out;
}

void PlayerChrome::show()
{
    if (!shown_)
    {
        zone_ = Zone::buttons;
        focus_ = Button::play;
    }
    shown_ = true;
    idle_ = 0.0f;
}

void PlayerChrome::hide()
{
    shown_ = false;
    lifted_ = false;
}

void PlayerChrome::wake()
{
    idle_ = 0.0f;
}

PlayerChrome::Command PlayerChrome::seek_by(double seconds)
{
    const double from = seek_hold_ > 0.0f ? seek_target_ : position_;
    double target = from + seconds;
    if (duration_ > 0.0)
        target = std::min(target, std::max(0.0, duration_ - 1.0));
    seek_target_ = std::max(0.0, target);
    seek_hold_ = 1.2f;
    return Command::seek;
}

PlayerChrome::Command PlayerChrome::press(Button button)
{
    switch (button)
    {
    case Button::restart:
        seek_target_ = 0.0;
        seek_hold_ = 1.2f;
        return Command::restart;
    case Button::rewind:
        return seek_by(-kSeekStep);
    case Button::play:
        return Command::toggle_pause;
    case Button::forward:
        return seek_by(kSeekStep);
    case Button::next:
        return Command::next;
    case Button::skip:
        return Command::skip_segment;
    }
    return Command::none;
}

PlayerChrome::Command PlayerChrome::handle(const InputFrame &input, ui::Feedback &feedback,
                                           bool *consumed_back)
{
    if (consumed_back != nullptr)
        *consumed_back = false;
    const bool any = input.nav != Direction::none || input.pressed != 0;
    if (!any)
        return Command::none;

    if (!shown_)
    {
        // Hidden: X pauses, Left/Right seek, Circle leaves (not ours), the rest
        // only bring the controls back.
        if (input.is_pressed(Action::back))
            return Command::none;
        show();
        if (input.is_pressed(Action::confirm))
        {
            feedback.play(audio::Cue::select);
            return Command::toggle_pause;
        }
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            zone_ = Zone::seek;
            feedback.play(audio::Cue::tick);
            return seek_by(input.nav == Direction::right ? kSeekStep : -kSeekStep);
        }
        feedback.play(audio::Cue::open);
        return Command::none;
    }

    wake();
    if (input.is_pressed(Action::back))
    {
        if (consumed_back != nullptr)
            *consumed_back = true;
        hide();
        feedback.play(audio::Cue::back);
        return Command::none;
    }

    if (zone_ == Zone::seek)
    {
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            feedback.play(audio::Cue::tick);
            return seek_by(input.nav == Direction::right ? kSeekStep : -kSeekStep);
        }
        if (input.nav == Direction::down)
        {
            zone_ = Zone::buttons;
            feedback.play(audio::Cue::focus);
            return Command::none;
        }
        if (input.is_pressed(Action::confirm))
        {
            feedback.play(audio::Cue::select);
            return Command::toggle_pause;
        }
        return Command::none;
    }

    const std::vector<Button> row = buttons();
    std::size_t at = 0;
    for (std::size_t i = 0; i < row.size(); ++i)
    {
        if (row[i] == focus_)
            at = i;
    }
    if (input.nav == Direction::left || input.nav == Direction::right)
    {
        const bool right = input.nav == Direction::right;
        if ((right && at + 1 < row.size()) || (!right && at > 0))
        {
            focus_ = row[right ? at + 1 : at - 1];
            feedback.play(audio::Cue::focus);
        }
        else
            feedback.play(audio::Cue::error);
        return Command::none;
    }
    if (input.nav == Direction::up)
    {
        zone_ = Zone::seek;
        feedback.play(audio::Cue::focus);
        return Command::none;
    }
    if (input.nav == Direction::down)
        return Command::suggestions;
    if (input.is_pressed(Action::confirm))
    {
        feedback.play(audio::Cue::select);
        return press(row[at]);
    }
    return Command::none;
}

void PlayerChrome::update(float dt)
{
    if (shown_ && playing_ && !lifted_)
    {
        idle_ += dt;
        if (idle_ >= kHideAfter)
            hide();
    }
    if (seek_hold_ > 0.0f)
        seek_hold_ = std::max(0.0f, seek_hold_ - dt);
    visibility_ = approach(visibility_, shown_ ? 1.0f : 0.0f, 14.0f, dt);
    lift_ = approach(lift_, lifted_ && shown_ ? 1.0f : 0.0f, 12.0f, dt);
}

void PlayerChrome::draw(ui::Canvas &canvas, const ui::Fonts &fonts, float rise) const
{
    const float a = visibility_;
    if (a < 0.01f)
        return;
    gfx::DrawList &list = canvas.list;
    list.push_opacity(a);

    // Scrim: the lower part of the picture darkens toward the bottom edge.
    const float scrim_top = 560.0f - rise;
    list.gradient_rect({0.0f, scrim_top, gfx::kVirtualWidth, gfx::kVirtualHeight - scrim_top}, 0.0f,
                       gfx::Color::rgb(0x000000, 0.0f), gfx::Color::rgb(0x000000, 0.9f));

    // Title and the channel line.
    const float width = kRight - kLeft;
    if (fonts.semibold.font != nullptr)
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(title_, 40.0f, width), kLeft,
                 kTitleY - rise, 40.0f, kInk);
    if (!subtitle_.empty() && fonts.regular.font != nullptr)
        ui::text(list, fonts.regular, fonts.regular.font->fit(subtitle_, 24.0f, width), kLeft,
                 kSubtitleY - rise, 24.0f, kMuted);

    // Seek bar: buffered, played, then the segments over both.
    const bool seek_focus = zone_ == Zone::seek;
    const float track_h = seek_focus ? 10.0f : 6.0f;
    const gfx::Rect track{kLeft, kTrackY - rise - track_h * 0.5f, width, track_h};
    const double shown_position = seek_hold_ > 0.0f ? seek_target_ : position_;
    const float played = duration_ > 0.0
                             ? static_cast<float>(std::clamp(shown_position / duration_, 0.0, 1.0))
                             : 0.0f;
    const float loaded =
        duration_ > 0.0 ? static_cast<float>(std::clamp(buffered_ / duration_, 0.0, 1.0)) : 0.0f;
    list.rounded_rect(track, track_h * 0.5f, gfx::Color::rgb(0xffffff, 0.25f));
    if (loaded > played)
        list.rounded_rect({track.x, track.y, track.w * loaded, track.h}, track_h * 0.5f,
                          gfx::Color::rgb(0xffffff, 0.4f));
    list.rounded_rect({track.x, track.y, std::max(track.w * played, track_h), track.h},
                      track_h * 0.5f, kRed);
    if (duration_ > 0.0)
    {
        for (const ui::MediaRange &range : ranges_)
        {
            const float x0 =
                track.x +
                track.w * std::clamp(range.start / static_cast<float>(duration_), 0.0f, 1.0f);
            const float x1 =
                track.x +
                track.w * std::clamp(range.end / static_cast<float>(duration_), 0.0f, 1.0f);
            if (x1 - x0 >= 1.0f)
                list.rounded_rect({x0, track.y, x1 - x0, track.h}, 0.0f, range.color);
        }
    }
    const float thumb_x = track.x + track.w * played;
    list.circle(thumb_x, track.y + track_h * 0.5f, seek_focus ? 15.0f : 9.0f,
                seek_focus ? kInk : kRed);

    // Times, and the stream's quality on the right.
    const std::string times = clock_text(shown_position) + " / " + clock_text(duration_);
    ui::text(list, fonts.regular, times, kLeft, kTimeY - rise, 24.0f, kInk);
    if (!quality_.empty())
        ui::text(list, fonts.regular, quality_, kRight, kTimeY - rise, 22.0f, kMuted,
                 gfx::Align::right);

    // The buttons.
    const std::vector<Button> row = buttons();
    const float cy = kButtonY - rise;
    float cx = kLeft + kButtonRadius;
    for (const Button button : row)
    {
        const bool focused = !seek_focus && button == focus_;
        if (button == Button::skip)
        {
            const std::string label = "Skip " + segment_;
            const float w = skip_width(fonts, label);
            const gfx::Rect pill{kRight - w, cy - kSkipHeight * 0.5f, w, kSkipHeight};
            list.rounded_rect(pill, kSkipHeight * 0.5f,
                              focused ? kInk : gfx::Color::rgb(0xffffff, 0.16f));
            ui::text(list, fonts.semibold, label, pill.cx(), cy + 8.5f, 24.0f,
                     focused ? kDark : kInk, gfx::Align::center);
            continue;
        }
        const gfx::Color ink = focused ? kDark : kInk;
        list.circle(cx, cy, kButtonRadius, focused ? kInk : gfx::Color::rgb(0xffffff, 0.12f));
        switch (button)
        {
        case Button::restart:
            list.rounded_rect({cx - 11.0f, cy - 10.0f, 4.0f, 20.0f}, 1.5f, ink);
            triangle(list, cx + 3.0f, cy, 20.0f, -1.0f, ink);
            break;
        case Button::rewind:
            triangle(list, cx - 6.0f, cy, 17.0f, -1.0f, ink);
            triangle(list, cx + 7.0f, cy, 17.0f, -1.0f, ink);
            break;
        case Button::play:
            if (playing_)
            {
                list.rounded_rect({cx - 10.0f, cy - 12.0f, 7.0f, 24.0f}, 2.0f, ink);
                list.rounded_rect({cx + 3.0f, cy - 12.0f, 7.0f, 24.0f}, 2.0f, ink);
            }
            else
                triangle(list, cx + 3.0f, cy, 26.0f, 1.0f, ink);
            break;
        case Button::forward:
            triangle(list, cx - 7.0f, cy, 17.0f, 1.0f, ink);
            triangle(list, cx + 6.0f, cy, 17.0f, 1.0f, ink);
            break;
        case Button::next:
            triangle(list, cx - 3.0f, cy, 20.0f, 1.0f, ink);
            list.rounded_rect({cx + 7.0f, cy - 10.0f, 4.0f, 20.0f}, 1.5f, ink);
            break;
        case Button::skip:
            break;
        }
        cx += kButtonPitch;
    }
    list.pop_opacity();
}

} // namespace hui::app

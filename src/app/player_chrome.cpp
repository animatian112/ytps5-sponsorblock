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

void PlayerChrome::set_badges(bool captions_on, std::string speed, std::string quality,
                              bool audio_tracks)
{
    captions_on_ = captions_on;
    speed_label_ = std::move(speed);
    quality_label_ = std::move(quality);
    audio_tracks_ = audio_tracks;
    if (!audio_tracks_ && focus_ == Button::audio)
        focus_ = Button::play;
}

void PlayerChrome::set_account(bool signed_in, bool liked, bool disliked, bool subscribed)
{
    signed_in_ = signed_in;
    liked_ = liked;
    disliked_ = disliked;
    subscribed_ = subscribed;
    if (!signed_in_ &&
        (focus_ == Button::like || focus_ == Button::dislike || focus_ == Button::subscribe))
        focus_ = Button::play;
}

std::vector<PlayerChrome::Button> PlayerChrome::buttons() const
{
    std::vector<Button> out{Button::restart, Button::rewind, Button::play, Button::forward};
    if (has_next_)
        out.push_back(Button::next);
    if (!segment_.empty())
        out.push_back(Button::skip);
    out.push_back(Button::info);
    if (has_channel_)
        out.push_back(Button::channel);
    if (signed_in_)
    {
        out.push_back(Button::like);
        out.push_back(Button::dislike);
        out.push_back(Button::subscribe);
    }
    out.push_back(Button::captions);
    out.push_back(Button::speed);
    out.push_back(Button::quality);
    if (audio_tracks_)
        out.push_back(Button::audio);
    return out;
}

bool PlayerChrome::right_side(Button button)
{
    return button == Button::skip || button == Button::captions || button == Button::speed ||
           button == Button::quality || button == Button::audio || button == Button::like ||
           button == Button::dislike || button == Button::subscribe || button == Button::channel ||
           button == Button::info;
}

std::string PlayerChrome::button_label(Button button) const
{
    switch (button)
    {
    case Button::skip:
        return "Skip " + segment_;
    case Button::captions:
        return "CC";
    case Button::speed:
        return speed_label_;
    case Button::quality:
        return quality_label_.empty() ? std::string("Quality") : quality_label_;
    case Button::audio:
        return "Audio";
    case Button::like:
        return liked_ ? "Liked" : "Like";
    case Button::dislike:
        return disliked_ ? "Disliked" : "Dislike";
    case Button::subscribe:
        return subscribed_ ? "Subscribed" : "Subscribe";
    case Button::channel:
        return "Channel";
    case Button::info:
        return "Info";
    default:
        return {};
    }
}

float PlayerChrome::button_width(const ui::Fonts &fonts, Button button) const
{
    if (!right_side(button))
        return kButtonRadius * 2.0f;
    const std::string label = button_label(button);
    const float text = fonts.semibold.font != nullptr ? fonts.semibold.measure(label, 22.0f)
                                                      : 12.0f * static_cast<float>(label.size());
    return std::max(kButtonRadius * 2.0f, text + 44.0f);
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
    panel_ = Panel::none;
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
    case Button::captions:
        return Command::open_captions;
    case Button::speed:
        return Command::open_speed;
    case Button::quality:
        return Command::open_quality;
    case Button::audio:
        return Command::open_audio;
    case Button::like:
        return Command::like;
    case Button::dislike:
        return Command::dislike;
    case Button::subscribe:
        return Command::subscribe;
    case Button::channel:
        return Command::channel;
    case Button::info:
        return Command::open_info;
    }
    return Command::none;
}

void PlayerChrome::open_panel(Panel panel, std::string title, std::vector<std::string> options,
                              int selected)
{
    panel_ = panel;
    panel_title_ = std::move(title);
    panel_options_ = std::move(options);
    panel_selected_ = selected;
    panel_focus_ =
        selected >= 0 && selected < static_cast<int>(panel_options_.size()) ? selected : 0;
    picked_ = -1;
    show();
}

void PlayerChrome::close_panel()
{
    panel_ = Panel::none;
}

PlayerChrome::Command PlayerChrome::handle_panel(const InputFrame &input, ui::Feedback &feedback,
                                                 bool *consumed_back)
{
    const int count = static_cast<int>(panel_options_.size());
    if (input.is_pressed(Action::back) || input.nav == Direction::left)
    {
        if (consumed_back != nullptr)
            *consumed_back = input.is_pressed(Action::back);
        close_panel();
        feedback.play(audio::Cue::back);
        return Command::none;
    }
    if ((input.nav == Direction::up || input.nav == Direction::down) && count > 0)
    {
        const int step = input.nav == Direction::down ? 1 : -1;
        const int next = panel_focus_ + step;
        if (next >= 0 && next < count)
        {
            panel_focus_ = next;
            feedback.play(audio::Cue::focus);
        }
        else
            feedback.play(audio::Cue::error);
        return Command::none;
    }
    if (input.is_pressed(Action::confirm) && count > 0)
    {
        picked_ = panel_focus_;
        panel_selected_ = panel_focus_;
        feedback.play(audio::Cue::select);
        return Command::pick; // the screen reads panel() and closes it
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
    if (panel_ != Panel::none)
        return handle_panel(input, feedback, consumed_back);
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
    panel_open_ = approach(panel_open_, panel_ != Panel::none ? 1.0f : 0.0f, 16.0f, dt);
    if (shown_ && playing_ && !lifted_ && panel_ == Panel::none)
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

    // The buttons: transport on the left, the pills on the right.
    const std::vector<Button> row = buttons();
    const float cy = kButtonY - rise;
    float cx = kLeft + kButtonRadius;
    float right_x = kRight;
    for (auto it = row.rbegin(); it != row.rend(); ++it)
    {
        if (!right_side(*it))
            continue;
        const Button button = *it;
        const bool focused = !seek_focus && panel_ == Panel::none && button == focus_;
        const bool lit = button == focus_ && panel_ != Panel::none;
        const float w = button_width(fonts, button);
        const gfx::Rect pill{right_x - w, cy - kButtonRadius, w, kButtonRadius * 2.0f};
        gfx::Color fill = gfx::Color::rgb(0xffffff, 0.12f);
        if (focused || lit)
            fill = kInk;
        else if (button == Button::skip)
            fill = gfx::Color::rgb(0xffffff, 0.2f);
        else if ((button == Button::like && liked_) || (button == Button::dislike && disliked_) ||
                 (button == Button::subscribe && subscribed_))
            fill = gfx::Color::rgb(0xff0033, 0.85f); // YouTube's red for "on"
        list.rounded_rect(pill, kButtonRadius, fill);
        const gfx::Color ink = (focused || lit) ? kDark : kInk;
        ui::text(list, fonts.semibold, button_label(button), pill.cx(), cy + 8.0f, 22.0f, ink,
                 gfx::Align::center);
        if (button == Button::captions && captions_on_)
            list.rounded_rect({pill.cx() - 14.0f, pill.y + pill.h - 12.0f, 28.0f, 4.0f}, 2.0f,
                              kRed);
        right_x -= w + 16.0f;
    }
    for (const Button button : row)
    {
        if (right_side(button))
            continue;
        const bool focused = !seek_focus && panel_ == Panel::none && button == focus_;
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
        default:
            break;
        }
        cx += kButtonPitch;
    }
    list.pop_opacity();
    draw_panel(canvas, fonts);
}

void PlayerChrome::draw_panel(ui::Canvas &canvas, const ui::Fonts &fonts) const
{
    if (panel_open_ < 0.01f)
        return;
    gfx::DrawList &list = canvas.list;
    // The description reads as text: tighter lines, no row highlight; Up and
    // Down scroll it.
    const bool reading = panel_ == Panel::info;
    constexpr float kWidth = 560.0f;
    const float row_height = reading ? 36.0f : 68.0f;
    constexpr float kTop = 170.0f;
    const float x = gfx::kVirtualWidth - kWidth * panel_open_;
    list.push_opacity(std::min(1.0f, panel_open_ * 1.4f));
    list.rounded_rect({x, 0.0f, kWidth, gfx::kVirtualHeight}, 0.0f,
                      gfx::Color::rgb(0x161616, 0.97f));
    if (fonts.semibold.font != nullptr)
        ui::text(list, fonts.semibold,
                 fonts.semibold.font->fit(panel_title_, 32.0f, kWidth - 96.0f), x + 48.0f, 112.0f,
                 32.0f, kInk);
    const int count = static_cast<int>(panel_options_.size());
    const int visible = static_cast<int>((gfx::kVirtualHeight - kTop - 40.0f) / row_height);
    int first = reading ? panel_focus_ : std::max(0, panel_focus_ - visible / 2);
    first = std::min(first, std::max(0, count - visible));
    for (int i = first; i < count && i < first + visible; ++i)
    {
        const float y = kTop + static_cast<float>(i - first) * row_height;
        const std::string &text = panel_options_[static_cast<std::size_t>(i)];
        if (reading)
        {
            ui::text(list, fonts.regular, text, x + 48.0f, y + 26.0f, 24.0f,
                     i == 0 ? kMuted : kInk);
            continue;
        }
        const bool focused = i == panel_focus_;
        const gfx::Rect row{x + 24.0f, y, kWidth - 48.0f, row_height - 8.0f};
        if (focused)
            list.rounded_rect(row, 10.0f, kInk);
        const gfx::Color ink = focused ? kDark : kInk;
        if (i == panel_selected_)
            list.circle(row.x + 30.0f, row.cy(), 8.0f, kRed);
        if (fonts.regular.font != nullptr)
            ui::text(list, fonts.regular, fonts.regular.font->fit(text, 26.0f, row.w - 80.0f),
                     row.x + 60.0f, row.cy() + 9.0f, 26.0f, ink);
    }
    list.pop_opacity();
}

} // namespace hui::app

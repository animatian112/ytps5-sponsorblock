// youtube-tv - The player's controls in SmartTube's layout.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/player_chrome.hpp"

#include "app/icons.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace hui::app
{
namespace
{

// SmartTube's player overlay, measured off its 1080p layout.
constexpr float kLeft = 112.0f;
constexpr float kRight = 1808.0f;
constexpr float kTitleY = 600.0f;
constexpr float kSubtitleY = 652.0f;
constexpr float kTopRowY = 784.0f;
constexpr float kTrackY = 858.0f;
constexpr float kBottomRowY = 936.0f;
constexpr float kButtonRadius = 42.0f; // the grey circle behind the focused icon
constexpr float kButtonPitch = 116.0f;
constexpr float kQuarter = 1.5707963f;

const gfx::Color kInk = gfx::Color::rgb(0xffffff);
const gfx::Color kMuted = gfx::Color::rgb(0xc8c8c8);
const gfx::Color kRed = gfx::Color::rgb(0xe52d27);
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
    {
        focus_ = Button::play;
        zone_ = Zone::top;
    }
}

void PlayerChrome::set_badges(bool captions_on, std::string speed, std::string quality,
                              bool audio_tracks)
{
    captions_on_ = captions_on;
    speed_label_ = std::move(speed);
    quality_label_ = std::move(quality);
    audio_tracks_ = audio_tracks;
    if (!audio_tracks_ && focus_ == Button::audio)
    {
        focus_ = Button::play;
        zone_ = Zone::top;
    }
}

void PlayerChrome::set_account(bool signed_in, bool liked, bool disliked, bool subscribed)
{
    signed_in_ = signed_in;
    liked_ = liked;
    disliked_ = disliked;
    subscribed_ = subscribed;
    if (!signed_in_ &&
        (focus_ == Button::like || focus_ == Button::dislike || focus_ == Button::subscribe))
    {
        focus_ = Button::play;
        zone_ = Zone::top;
    }
}

std::vector<PlayerChrome::Button> PlayerChrome::top_buttons() const
{
    std::vector<Button> out{Button::play, Button::restart};
    if (has_next_)
        out.push_back(Button::next);
    out.push_back(Button::rewind);
    out.push_back(Button::forward);
    out.push_back(Button::speed);
    out.push_back(Button::mode);
    if (!segment_.empty())
        out.push_back(Button::skip);
    return out;
}

std::vector<PlayerChrome::Button> PlayerChrome::bottom_buttons() const
{
    std::vector<Button> out{Button::quality};
    if (has_channel_)
        out.push_back(Button::channel);
    if (signed_in_)
    {
        out.push_back(Button::like);
        out.push_back(Button::dislike);
        out.push_back(Button::subscribe);
    }
    out.push_back(Button::captions);
    if (signed_in_)
        out.push_back(Button::save);
    if (audio_tracks_)
        out.push_back(Button::audio);
    out.push_back(Button::info);
    out.push_back(Button::comments);
    out.push_back(Button::sleep);
    out.push_back(Button::dim);
    out.push_back(Button::stats);
    return out;
}

std::string PlayerChrome::tooltip(Button button) const
{
    switch (button)
    {
    case Button::restart:
        return "Previous";
    case Button::rewind:
        return "Back 10 seconds";
    case Button::play:
        return playing_ ? "Pause" : "Play";
    case Button::forward:
        return "Forward 10 seconds";
    case Button::next:
        return "Next";
    case Button::skip:
        return "Skip " + segment_;
    case Button::captions:
        return "Subtitles";
    case Button::speed:
        return "Playback speed (" + speed_label_ + ")";
    case Button::quality:
        return "Playback quality settings" +
               (quality_label_.empty() ? std::string() : " (" + quality_label_ + ")");
    case Button::audio:
        return "Audio track";
    case Button::like:
        return liked_ ? "Liked" : "Like";
    case Button::dislike:
        return disliked_ ? "Disliked" : "Dislike";
    case Button::subscribe:
        return subscribed_ ? "Unsubscribe" : "Subscribe";
    case Button::channel:
        return "Open channel";
    case Button::save:
        return "Add to playlist";
    case Button::info:
        return "Video info";
    case Button::chapters:
        return "Chapters";
    case Button::comments:
        return "Comments";
    case Button::sleep:
        return sleep_on_ ? "Sleep timer (on)" : "Sleep timer";
    case Button::dim:
        return "Screen dimming";
    case Button::stats:
        return stats_on_ ? "Hide stats for nerds" : "Stats for nerds";
    case Button::mode:
    {
        static const char *const kModes[] = {"Play next", "Repeat the video", "Pause at the end",
                                             "Close at the end", "Shuffle"};
        return std::string("Playback mode: ") + kModes[std::clamp(mode_, 0, 4)];
    }
    }
    return {};
}

bool PlayerChrome::lit(Button button) const
{
    return (button == Button::mode && mode_ == 4) || (button == Button::like && liked_) ||
           (button == Button::dislike && disliked_) ||
           (button == Button::subscribe && subscribed_) ||
           (button == Button::captions && captions_on_) || (button == Button::sleep && sleep_on_) ||
           (button == Button::stats && stats_on_);
}

void PlayerChrome::show()
{
    if (!shown_)
    {
        zone_ = Zone::top;
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
    // Held (or pressed quickly again and again) the step grows, as in
    // SmartTube: x1, then x2, x4 and x8.
    seek_streak_ = seek_gap_ < 0.6f ? seek_streak_ + 1 : 0;
    seek_gap_ = 0.0f;
    const double factor = seek_streak_ < 4    ? 1.0
                          : seek_streak_ < 10 ? 2.0
                          : seek_streak_ < 18 ? 4.0
                                              : 8.0;
    const double from = seek_hold_ > 0.0f || seek_pending_ > 0.0f ? seek_target_ : position_;
    double target = from + seconds * factor;
    if (duration_ > 0.0)
        target = std::min(target, std::max(0.0, duration_ - 1.0));
    seek_target_ = std::max(0.0, target);
    seek_hold_ = 1.2f;
    // The bar moves now; the video follows once the presses stop.
    seek_pending_ = 0.45f;
    return Command::none;
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
        return seek_by(-options_.seek_step);
    case Button::play:
        return Command::toggle_pause;
    case Button::forward:
        return seek_by(options_.seek_step);
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
    case Button::save:
        return Command::save;
    case Button::info:
        return Command::open_info;
    case Button::chapters:
        return Command::open_chapters;
    case Button::mode:
        return Command::cycle_mode;
    case Button::comments:
        return Command::open_comments;
    case Button::sleep:
        return Command::open_sleep;
    case Button::dim:
        return Command::open_dim;
    case Button::stats:
        return Command::toggle_stats;
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

void PlayerChrome::set_panel_options(std::vector<std::string> options)
{
    panel_options_ = std::move(options);
    panel_focus_ =
        std::clamp(panel_focus_, 0, std::max(0, static_cast<int>(panel_options_.size()) - 1));
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
        else if (panel_ != Panel::info && panel_ != Panel::comments) // reading: no mistake
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
        if (input.is_pressed(Action::confirm))
        {
            // Settings > Player > OK button behavior.
            feedback.play(audio::Cue::select);
            if (options_.ok_behavior != 1)
                show();
            return options_.ok_behavior == 0 ? Command::none : Command::toggle_pause;
        }
        show();
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            zone_ = Zone::seek;
            feedback.play(audio::Cue::tick);
            return seek_by(input.nav == Direction::right ? options_.seek_step
                                                         : -options_.seek_step);
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
            return seek_by(input.nav == Direction::right ? options_.seek_step
                                                         : -options_.seek_step);
        }
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const bool down = input.nav == Direction::down;
            zone_ = down ? Zone::bottom : Zone::top;
            const std::vector<Button> row = down ? bottom_buttons() : top_buttons();
            focus_ = row.empty() ? Button::play : row.front();
            feedback.play(audio::Cue::focus);
            return Command::none;
        }
        if (input.is_pressed(Action::confirm))
        {
            feedback.play(audio::Cue::select);
            if (seek_pending_ > 0.0f)
            {
                seek_pending_ = 0.0f; // Cross: go there now
                seek_ready_ = true;
                return Command::none;
            }
            return Command::toggle_pause;
        }
        return Command::none;
    }

    const std::vector<Button> row = zone_ == Zone::top ? top_buttons() : bottom_buttons();
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
        if (zone_ == Zone::bottom)
        {
            zone_ = Zone::seek;
            feedback.play(audio::Cue::focus);
        }
        else
        {
            // Up past the top row puts the controls away.
            hide();
            feedback.play(audio::Cue::back);
        }
        return Command::none;
    }
    if (input.nav == Direction::down)
    {
        if (zone_ == Zone::top)
        {
            zone_ = Zone::seek;
            feedback.play(audio::Cue::focus);
            return Command::none;
        }
        return Command::suggestions;
    }
    if (input.is_pressed(Action::confirm) && !row.empty())
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
        if (options_.hide_after > 0.0f && idle_ >= options_.hide_after)
            hide();
    }
    seek_gap_ += dt;
    if (seek_pending_ > 0.0f)
    {
        seek_pending_ -= dt;
        if (seek_pending_ <= 0.0f)
        {
            seek_pending_ = 0.0f;
            seek_ready_ = true;
            seek_hold_ = 1.2f; // keep the target on the bar until the video got there
        }
    }
    else if (seek_hold_ > 0.0f)
        seek_hold_ = std::max(0.0f, seek_hold_ - dt);
    visibility_ = approach(visibility_, shown_ ? 1.0f : 0.0f, 14.0f, dt);
    lift_ = approach(lift_, lifted_ && shown_ ? 1.0f : 0.0f, 12.0f, dt);
}

void PlayerChrome::draw_icon(gfx::DrawList &list, const ui::Fonts &fonts, Button button, float cx,
                             float cy, gfx::Color ink) const
{
    // SmartTube's own glyphs (assets/icons, MIT, yuliskov); the drawn
    // shapes below stay as the fallback when a file is missing.
    const char *name = nullptr;
    switch (button)
    {
    case Button::restart:
        name = "skip_previous";
        break;
    case Button::rewind:
        name = "fast_rewind";
        break;
    case Button::play:
        name = playing_ ? "pause" : "play";
        break;
    case Button::forward:
        name = "fast_forward";
        break;
    case Button::next:
        name = "skip_next";
        break;
    case Button::skip:
        name = "skip_segment";
        break;
    case Button::speed:
        name = "speed";
        break;
    case Button::captions:
        name = "cc";
        break;
    case Button::quality:
        name = "quality";
        break;
    case Button::audio:
        name = "audio";
        break;
    case Button::like:
        name = liked_ ? "thumb_up" : "thumb_up_outline";
        break;
    case Button::dislike:
        name = disliked_ ? "thumb_down" : "thumb_down_outline";
        break;
    case Button::subscribe:
        name = "subscribe";
        break;
    case Button::channel:
        if (avatar_ != 0)
        {
            // SmartTube's "Show icon on the channel button": the picture.
            list.image(avatar_, {cx - 24.0f, cy - 24.0f, 48.0f, 48.0f}, gfx::kFullUv,
                       gfx::Color::rgb(0xffffff), 24.0f);
            return;
        }
        name = "channel";
        break;
    case Button::save:
        name = "playlist_add";
        break;
    case Button::info:
        name = "info";
        break;
    case Button::chapters:
        name = "chapters";
        break;
    case Button::stats:
        name = "stats";
        break;
    case Button::comments:
    case Button::sleep:
    case Button::dim:
        break; // drawn below
    case Button::mode:
    {
        // SmartTube's playback mode icons.
        static const char *const kIcons[] = {"mode_all", "mode_one", "mode_pause", "mode_none",
                                             "mode_shuffle"};
        name = kIcons[std::clamp(mode_, 0, 4)];
        break;
    }
    }
    const float size = button == Button::play ? 52.0f : 44.0f;
    if (name != nullptr &&
        app::draw_icon(list, name, {cx - size * 0.5f, cy - size * 0.5f, size, size}, ink))
        return;
    const gfx::Color hole = gfx::Color::rgb(0x161616);
    const auto boxed = [&](const char *label)
    {
        // SmartTube's "CC" and "HQ": a filled rounded box, the letters cut out.
        list.rounded_rect({cx - 24.0f, cy - 17.0f, 48.0f, 34.0f}, 6.0f, ink);
        if (fonts.semibold.font != nullptr)
            ui::text(list, fonts.semibold, label, cx, cy + 8.0f, 21.0f, hole, gfx::Align::center);
    };
    switch (button)
    {
    case Button::restart: // previous: a bar and a triangle pointing left
        list.rounded_rect({cx - 16.0f, cy - 14.0f, 5.0f, 28.0f}, 1.5f, ink);
        triangle(list, cx + 4.0f, cy, 28.0f, -1.0f, ink);
        break;
    case Button::rewind:
        triangle(list, cx - 8.0f, cy, 22.0f, -1.0f, ink);
        triangle(list, cx + 9.0f, cy, 22.0f, -1.0f, ink);
        break;
    case Button::play:
        if (playing_)
        {
            list.rounded_rect({cx - 13.0f, cy - 17.0f, 9.0f, 34.0f}, 2.0f, ink);
            list.rounded_rect({cx + 4.0f, cy - 17.0f, 9.0f, 34.0f}, 2.0f, ink);
        }
        else
            triangle(list, cx + 4.0f, cy, 36.0f, 1.0f, ink);
        break;
    case Button::forward:
        triangle(list, cx - 9.0f, cy, 22.0f, 1.0f, ink);
        triangle(list, cx + 8.0f, cy, 22.0f, 1.0f, ink);
        break;
    case Button::next:
        triangle(list, cx - 4.0f, cy, 28.0f, 1.0f, ink);
        list.rounded_rect({cx + 11.0f, cy - 14.0f, 5.0f, 28.0f}, 1.5f, ink);
        break;
    case Button::skip: // skip forward, red: a segment is under the playhead
        triangle(list, cx - 10.0f, cy, 22.0f, 1.0f, ink);
        triangle(list, cx + 6.0f, cy, 22.0f, 1.0f, ink);
        list.rounded_rect({cx + 15.0f, cy - 11.0f, 4.0f, 22.0f}, 1.5f, ink);
        break;
    case Button::speed: // a gauge
        list.ring(cx, cy + 4.0f, 20.0f, 4.0f, ink);
        list.line(cx, cy + 4.0f, cx + 11.0f, cy - 8.0f, 4.0f, ink);
        list.circle(cx, cy + 4.0f, 4.0f, ink);
        break;
    case Button::captions:
        boxed("CC");
        break;
    case Button::quality:
        boxed("HQ");
        break;
    case Button::audio: // a note
        list.circle(cx - 7.0f, cy + 10.0f, 8.0f, ink);
        list.line(cx, cy + 10.0f, cx, cy - 16.0f, 4.0f, ink);
        list.line(cx, cy - 16.0f, cx + 12.0f, cy - 9.0f, 4.0f, ink);
        break;
    case Button::like:
    case Button::dislike:
    {
        // A thumb: cuff, palm and the raised thumb (flipped for dislike).
        const float flip = button == Button::like ? 1.0f : -1.0f;
        list.rounded_rect({cx - 20.0f, cy - 6.0f * flip - (flip < 0 ? 18.0f : 0.0f), 8.0f, 18.0f},
                          2.0f, ink);
        list.rounded_rect({cx - 9.0f, cy - 6.0f * flip - (flip < 0 ? 20.0f : 0.0f), 26.0f, 20.0f},
                          4.0f, ink);
        list.rounded_rect({cx - 7.0f, flip > 0 ? cy - 22.0f : cy + 6.0f, 10.0f, 18.0f}, 5.0f, ink);
        break;
    }
    case Button::subscribe: // a person and a plus
        list.circle(cx - 6.0f, cy - 9.0f, 8.0f, ink);
        list.rounded_rect({cx - 20.0f, cy + 2.0f, 28.0f, 15.0f}, 7.0f, ink);
        list.line(cx + 14.0f, cy - 6.0f, cx + 14.0f, cy + 8.0f, 4.0f, ink);
        list.line(cx + 7.0f, cy + 1.0f, cx + 21.0f, cy + 1.0f, 4.0f, ink);
        break;
    case Button::channel: // the channel's initial in a round avatar
    {
        list.circle(cx, cy, 22.0f, gfx::Color::rgb(0x3a3a3a));
        const std::string initial = subtitle_.empty() ? std::string("?") : subtitle_.substr(0, 1);
        if (fonts.semibold.font != nullptr)
            ui::text(list, fonts.semibold, initial, cx, cy + 8.0f, 24.0f, kInk, gfx::Align::center);
        break;
    }
    case Button::save: // three lines and a plus
        for (int i = 0; i < 3; ++i)
            list.rounded_rect({cx - 22.0f, cy - 14.0f + static_cast<float>(i) * 10.0f,
                               i == 2 ? 16.0f : 26.0f, 4.0f},
                              1.0f, ink);
        list.line(cx + 14.0f, cy - 2.0f, cx + 14.0f, cy + 18.0f, 4.0f, ink);
        list.line(cx + 4.0f, cy + 8.0f, cx + 24.0f, cy + 8.0f, 4.0f, ink);
        break;
    case Button::info:
        list.ring(cx, cy, 20.0f, 4.0f, ink);
        list.circle(cx, cy - 9.0f, 3.0f, ink);
        list.rounded_rect({cx - 2.0f, cy - 3.0f, 4.0f, 14.0f}, 1.0f, ink);
        break;
    case Button::mode: // two arrows in a loop
        list.ring(cx, cy, 18.0f, 4.0f, ink);
        triangle(list, cx + 18.0f, cy - 4.0f, 14.0f, 1.0f, ink);
        break;
    case Button::comments: // a speech bubble
        list.rounded_rect({cx - 20.0f, cy - 16.0f, 40.0f, 26.0f}, 6.0f, ink);
        triangle(list, cx - 8.0f, cy + 14.0f, 10.0f, 1.0f, ink);
        for (int i = 0; i < 2; ++i)
            list.rounded_rect({cx - 12.0f, cy - 9.0f + static_cast<float>(i) * 9.0f, 24.0f, 3.0f},
                              1.0f, gfx::Color::rgb(0x202020));
        break;
    case Button::sleep: // a moon
        list.circle(cx, cy, 18.0f, ink);
        list.circle(cx + 9.0f, cy - 7.0f, 15.0f, gfx::Color::rgb(0x202020));
        break;
    case Button::stats: // a bug
        list.circle(cx, cy + 3.0f, 13.0f, ink);
        list.circle(cx, cy - 13.0f, 7.0f, ink);
        for (int i = -1; i <= 1; ++i)
        {
            list.line(cx - 11.0f, cy + 3.0f + i * 8.0f, cx - 21.0f, cy + i * 10.0f, 3.0f, ink);
            list.line(cx + 11.0f, cy + 3.0f + i * 8.0f, cx + 21.0f, cy + i * 10.0f, 3.0f, ink);
        }
        break;
    case Button::dim: // a sun, half dark
        list.ring(cx, cy, 12.0f, 3.5f, ink);
        list.rounded_rect({cx - 12.0f, cy - 12.0f, 12.0f, 24.0f}, 6.0f, ink);
        for (int i = 0; i < 8; ++i)
        {
            const float angle = static_cast<float>(i) * 0.785398f;
            list.line(cx + std::cos(angle) * 17.0f, cy + std::sin(angle) * 17.0f,
                      cx + std::cos(angle) * 23.0f, cy + std::sin(angle) * 23.0f, 3.0f, ink);
        }
        break;
    case Button::chapters: // a list with bullets
        for (int i = 0; i < 3; ++i)
        {
            const float y = cy - 12.0f + static_cast<float>(i) * 12.0f;
            list.circle(cx - 16.0f, y, 3.5f, ink);
            list.rounded_rect({cx - 8.0f, y - 2.0f, 26.0f, 4.0f}, 1.0f, ink);
        }
        break;
    }
}

void PlayerChrome::draw(ui::Canvas &canvas, const ui::Fonts &fonts, float rise) const
{
    const float a = visibility_;
    if (a < 0.01f)
        return;
    gfx::DrawList &list = canvas.list;
    list.push_opacity(a);

    // Scrim: the lower part of the picture darkens toward the bottom edge.
    const float scrim_top = 420.0f - rise;
    list.gradient_rect({0.0f, scrim_top, gfx::kVirtualWidth, gfx::kVirtualHeight - scrim_top}, 0.0f,
                       gfx::Color::rgb(0x000000, 0.0f), gfx::Color::rgb(0x000000, 0.92f));

    // Title, large and light, and the channel line under it.
    const float width = kRight - kLeft;
    if (fonts.regular.font != nullptr)
    {
        ui::text(list, fonts.regular, fonts.regular.font->fit(title_, 60.0f, width), kLeft,
                 kTitleY - rise, 60.0f, kInk);
        if (!subtitle_.empty())
        {
            // The channel's picture before its name.
            float x = kLeft;
            if (avatar_ != 0)
            {
                list.image(avatar_, {kLeft, kSubtitleY - rise - 30.0f, 40.0f, 40.0f}, gfx::kFullUv,
                           gfx::Color::rgb(0xffffff), 20.0f);
                x += 54.0f;
            }
            ui::text(list, fonts.regular, fonts.regular.font->fit(subtitle_, 30.0f, width), x,
                     kSubtitleY - rise, 30.0f, kInk);
        }
    }

    // The format and the clock at the right of the top row.
    if (options_.quality && !quality_.empty())
        ui::text(list, fonts.regular, quality_, kRight, kTopRowY - 12.0f - rise, 30.0f, kInk,
                 gfx::Align::right);
    if (options_.clock && !clock_.empty())
        ui::text(list, fonts.regular, clock_, kRight, kTopRowY + 28.0f - rise, 28.0f, kInk,
                 gfx::Align::right);

    // The rows of buttons: plain white icons; the focused one sits on a grey
    // circle with its name in a pill beside the row.
    std::string tip;
    float tip_x = 0.0f;
    float tip_y = 0.0f;
    const auto draw_row = [&](const std::vector<Button> &row, float cy, Zone zone)
    {
        float cx = kLeft + 24.0f;
        for (const Button button : row)
        {
            const bool focused = zone_ == zone && button == focus_ && !lifted_;
            if (focused)
            {
                list.circle(cx, cy, kButtonRadius,
                            gfx::Color::rgb(0x6b6b6b, panel_ == Panel::none ? 0.85f : 0.6f));
                if (panel_ == Panel::none && options_.tooltips)
                {
                    tip = tooltip(button);
                    tip_x = cx;
                    tip_y =
                        zone == Zone::top ? cy - kButtonRadius - 30.0f : cy + kButtonRadius + 38.0f;
                }
            }
            const gfx::Color ink = button == Button::skip || lit(button) ? kRed : kInk;
            draw_icon(list, fonts, button, cx, cy, ink);
            cx += kButtonPitch;
        }
    };
    draw_row(top_buttons(), kTopRowY - rise, Zone::top);

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
    list.rounded_rect(track, 0.0f, gfx::Color::rgb(0xffffff, 0.25f));
    if (loaded > played)
        list.rounded_rect({track.x, track.y, track.w * loaded, track.h}, 0.0f,
                          gfx::Color::rgb(0xffffff, 0.4f));
    list.rounded_rect({track.x, track.y, std::max(track.w * played, 2.0f), track.h}, 0.0f, kRed);
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
    if (duration_ > 0.0 && chapters_.size() > 1)
    {
        // Chapter marks: a gap in the bar where each chapter starts.
        for (std::size_t i = 1; i < chapters_.size(); ++i)
        {
            const float x =
                track.x + track.w * std::clamp(static_cast<float>(chapters_[i].start_s / duration_),
                                               0.0f, 1.0f);
            list.rounded_rect({x - 2.0f, track.y - 1.0f, 4.0f, track.h + 2.0f}, 0.0f,
                              gfx::Color::rgb(0x000000, 0.85f));
        }
        // While seeking, the chapter under the target above the thumb.
        int at = -1;
        for (std::size_t i = 0; i < chapters_.size(); ++i)
            if (chapters_[i].start_s <= shown_position + 0.25)
                at = static_cast<int>(i);
        if (at >= 0 && seek_focus && fonts.regular.font != nullptr)
        {
            const std::string name = fonts.regular.font->fit(
                chapters_[static_cast<std::size_t>(at)].title, 26.0f, 900.0f);
            const float x = std::clamp(track.x + track.w * played, track.x + 200.0f,
                                       track.x + track.w - 200.0f);
            ui::text(list, fonts.regular, name, x, track.y - 22.0f, 26.0f,
                     gfx::Color::rgb(0xdddddd), gfx::Align::center);
        }
    }
    if (seek_focus)
        list.circle(track.x + track.w * played, track.y + track_h * 0.5f, 14.0f, kInk);
    // SmartTube's seek preview: the storyboard frame of the target above the
    // thumb, its time under it.
    if (previewing() && preview_texture_ != 0)
    {
        const float w = 320.0f;
        const float h = w / std::max(0.5f, preview_aspect_);
        const float x =
            std::clamp(track.x + track.w * played - w * 0.5f, track.x, track.x + track.w - w);
        const gfx::Rect box{x, track.y - h - 70.0f, w, h};
        list.rounded_rect({box.x - 4.0f, box.y - 4.0f, box.w + 8.0f, box.h + 8.0f}, 8.0f,
                          gfx::Color::rgb(0xffffff));
        list.image(preview_texture_, box, preview_uv_, gfx::Color::rgb(0xffffff), 6.0f);
        if (fonts.semibold.font != nullptr)
        {
            const std::string at = clock_text(seek_target_);
            const float tw = fonts.semibold.measure(at, 26.0f) + 24.0f;
            const gfx::Rect pill{box.cx() - tw * 0.5f, box.y + box.h - 40.0f, tw, 34.0f};
            list.rounded_rect(pill, 6.0f, gfx::Color::rgb(0x000000, 0.75f));
            ui::text(list, fonts.semibold, at, pill.cx(), pill.cy() + 9.0f, 26.0f, kInk,
                     gfx::Align::center);
        }
    }

    draw_row(bottom_buttons(), kBottomRowY - rise, Zone::bottom);

    // Remaining time, then position / length, at the right of the bottom row.
    const double left = duration_ > shown_position ? duration_ - shown_position : 0.0;
    if (options_.remaining)
        ui::text(list, fonts.regular, "Remaining: " + clock_text(left), kRight,
                 kBottomRowY - 8.0f - rise, 30.0f, kInk, gfx::Align::right);
    ui::text(list, fonts.regular,
             live_ ? "LIVE  " + clock_text(shown_position)
                   : clock_text(shown_position) + " / " + clock_text(duration_),
             kRight, kBottomRowY + 34.0f - rise, 30.0f, kInk, gfx::Align::right);

    if (!tip.empty() && fonts.regular.font != nullptr)
    {
        const float w = fonts.regular.measure(tip, 26.0f) + 40.0f;
        const float x = std::clamp(tip_x - w * 0.5f, 16.0f, gfx::kVirtualWidth - w - 16.0f);
        const gfx::Rect pill{x, tip_y - 24.0f, w, 48.0f};
        list.rounded_rect(pill, 24.0f, gfx::Color::rgb(0x424242, 0.95f));
        ui::text(list, fonts.regular, tip, pill.cx(), pill.cy() + 9.0f, 26.0f, kInk,
                 gfx::Align::center);
    }
    list.pop_opacity();
    draw_panel(canvas, fonts);
}

void PlayerChrome::draw_panel(ui::Canvas &canvas, const ui::Fonts &fonts) const
{
    if (panel_open_ < 0.01f)
        return;
    gfx::DrawList &list = canvas.list;
    // SmartTube's settings panel: a header band with the title, then the
    // choices with radio circles (red for the current one). The description
    // reads as text: tighter lines, Up and Down scroll it.
    const bool reading = panel_ == Panel::info || panel_ == Panel::comments;
    constexpr float kWidth = 720.0f;
    constexpr float kHeader = 170.0f;
    const float row_height = reading ? 38.0f : 96.0f;
    const float top = kHeader + (reading ? 34.0f : 0.0f);
    const float x = gfx::kVirtualWidth - kWidth * panel_open_;
    list.push_opacity(std::min(1.0f, panel_open_ * 1.4f));
    list.rounded_rect({x, 0.0f, kWidth, gfx::kVirtualHeight}, 0.0f,
                      gfx::Color::rgb(0x1d1d1d, 0.98f));
    list.rounded_rect({x, 0.0f, kWidth, kHeader}, 0.0f, gfx::Color::rgb(0x262626));
    if (fonts.semibold.font != nullptr)
        ui::text(list, fonts.semibold,
                 fonts.semibold.font->fit(panel_title_, 38.0f, kWidth - 112.0f), x + 56.0f, 100.0f,
                 38.0f, kInk);
    const int count = static_cast<int>(panel_options_.size());
    const int visible = static_cast<int>((gfx::kVirtualHeight - top - 24.0f) / row_height);
    int first = reading ? panel_focus_ : std::max(0, panel_focus_ - visible / 2);
    first = std::min(first, std::max(0, count - visible));
    for (int i = first; i < count && i < first + visible; ++i)
    {
        const float y = top + static_cast<float>(i - first) * row_height;
        const std::string &text = panel_options_[static_cast<std::size_t>(i)];
        if (reading)
        {
            ui::text(list, fonts.regular, text, x + 56.0f, y + 26.0f, 26.0f,
                     i == 0 ? kMuted : kInk);
            continue;
        }
        const bool focused = i == panel_focus_;
        const gfx::Rect row{x, y, kWidth, row_height};
        if (focused)
            list.rounded_rect(row, 0.0f, gfx::Color::rgb(0xffffff, 0.12f));
        const float radio_x = x + 80.0f;
        if (i == panel_selected_)
        {
            list.ring(radio_x, row.cy(), 18.0f, 3.0f, kRed);
            list.circle(radio_x, row.cy(), 10.0f, kRed);
        }
        else
            list.ring(radio_x, row.cy(), 18.0f, 3.0f, gfx::Color::rgb(0xd0d0d0));
        if (fonts.regular.font != nullptr)
            ui::text(list, fonts.regular, fonts.regular.font->fit(text, 28.0f, kWidth - 180.0f),
                     x + 144.0f, row.cy() + 10.0f, 28.0f, kInk);
    }
    list.pop_opacity();
}

} // namespace hui::app

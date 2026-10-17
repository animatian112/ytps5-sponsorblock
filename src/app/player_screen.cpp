// youtube-tv - The player screen.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The service resolves the formats and opens the demuxers on its worker;
// this screen only owns the picture: it uploads the frame the player hands
// over, draws the transport controls and mirrors their commands onto the
// player. Circle leaves (and closes the player through the app).

#include "app/player_screen.hpp"

#include "yt/chapters.hpp"

#include "platform/ps5/system.hpp"

#include "app/feed_view.hpp"
#include "app/thumbs.hpp"
#include "yt/log.hpp"
#include "app/theme.hpp"
#include "gfx/gl_batch.hpp"

#include <GL/glcorearb.h>

#include "yt/codec.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace hui::app
{
namespace
{

// "1234567" -> "1,234,567".
std::string grouped_digits(const std::string &digits)
{
    std::string grouped;
    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        if (i > 0 && (digits.size() - i) % 3 == 0)
            grouped += ',';
        grouped += digits[i];
    }
    return grouped;
}

// "Fri Oct 16  10:34 AM", as SmartTube shows it in the player.
// "0:00", "12:05", "1:02:03" for a chapter's start.
std::string chapter_time(double seconds)
{
    const int total = static_cast<int>(seconds + 0.5);
    char text[24];
    if (total >= 3600)
        std::snprintf(text, sizeof(text), "%d:%02d:%02d", total / 3600, (total % 3600) / 60,
                      total % 60);
    else
        std::snprintf(text, sizeof(text), "%d:%02d", total / 60, total % 60);
    return text;
}

std::string clock_line(bool hours24)
{
    static const char *const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::string line;
    int weekday = 0;
    int month = 0;
    int day = 0;
    if (sys::local_date(&weekday, &month, &day) && month >= 1 && month <= 12)
        line = std::string(kDays[weekday % 7]) + " " + kMonths[month - 1] + " " +
               std::to_string(day) + "  ";
    int hour = 0;
    int minute = 0;
    bool twelve = true;
    if (sys::local_clock(&hour, &minute, &twelve))
    {
        char text[16];
        if (!hours24)
            std::snprintf(text, sizeof(text), "%d:%02d %s", hour % 12 == 0 ? 12 : hour % 12, minute,
                          hour < 12 ? "AM" : "PM");
        else
            std::snprintf(text, sizeof(text), "%02d:%02d", hour, minute);
        line += text;
    }
    return line;
}

const TourStep kTour[] = {
    // Playing: the controls hide on their own, the picture is clear.
    {5.0f, 0, Direction::none, "video", 0, 0, 0, 0, 0},
    // Any button brings them back with the focus on play/pause (top row).
    {0.9f, 0, Direction::up, nullptr, 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::none, "controls", 0, 0, 0, 0, 0},
    // Down to the seek bar, down again to the bottom row (quality first).
    {0.6f, 0, Direction::down, nullptr, 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::none, "seek", 0, 0, 0, 0, 0},
    {0.6f, 0, Direction::down, nullptr, 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::none, "buttons", 0, 0, 0, 0, 0},
    {0.9f, hui::action_bit(Action::confirm), Direction::none, nullptr, 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::none, "quality-menu", 0, 0, 0, 0, 0},
    {0.9f, hui::action_bit(Action::back), Direction::none, nullptr, 0, 0, 0, 0, 0},
    // Down from the bottom row: the suggestions.
    {0.9f, 0, Direction::down, nullptr, 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::right, nullptr, 0, 0, 0, 0, 0},
    {1.0f, 0, Direction::none, "related", 0, 0, 0, 0, 0},
};

constexpr const char *kNoStream = "No stream for the enabled codecs";

// The last log lines, drawn at the bottom-left of the picture just above the
// hint row: a stuck or failed player becomes readable on the television
// without FTP. Newest last, each line clipped to 96 characters.
void draw_log_tail(ui::Canvas &canvas, const Context &context)
{
    std::string lines[12];
    const std::size_t count = yt::log::recent(lines, 12);
    if (count == 0)
        return;
    constexpr float kLineHeight = 22.0f;
    constexpr std::size_t kMaxChars = 140;
    float y = 1010.0f - static_cast<float>(count - 1) * kLineHeight;
    for (std::size_t i = 0; i < count; ++i)
    {
        std::string shown = lines[i];
        if (shown.size() > kMaxChars)
            shown.resize(kMaxChars);
        ui::text(canvas.list, context.fonts.mono, shown, 60.0f, y, 17.0f,
                 gfx::Color::rgb(0x9a9a9a));
        y += kLineHeight;
    }
}

// SponsorBlock's own category colours, so the bar reads like everywhere else.
gfx::Color segment_color(const std::string &category)
{
    if (category == "sponsor")
        return gfx::Color::rgb(0x00d400);
    if (category == "selfpromo")
        return gfx::Color::rgb(0xffff00);
    if (category == "interaction")
        return gfx::Color::rgb(0xcc00ff);
    if (category == "intro")
        return gfx::Color::rgb(0x00ffff);
    if (category == "outro")
        return gfx::Color::rgb(0x0202ed);
    if (category == "preview")
        return gfx::Color::rgb(0x008fd6);
    if (category == "music_offtopic")
        return gfx::Color::rgb(0xff9900);
    if (category == "filler")
        return gfx::Color::rgb(0x7300ff);
    if (category == "poi_highlight")
        return gfx::Color::rgb(0xff1684);
    return gfx::Color::rgb(0xaaaaaa);
}

} // namespace

PlayerScreen::PlayerScreen(Context &context) : context_(context)
{
    related_.style.theme = card_theme();
    related_.style.card.art_aspect = 16.0f / 9.0f;
    related_.style.card.text = ui::CardText::below;
    related_.style.card.title_size = 21.0f;
    related_.style.card.subtitle_size = 18.0f;
    related_.style.card.badge_corner = ui::CardCorner::bottom_right;
    related_.style.card.radius = 12.0f;
    related_.style.card.info_box = true;
    related_.style.card.ring = false;
    related_.style.card.info_color = gfx::Color::rgb(0x1f1f1f);
    related_.style.card.info_focus = gfx::Color::rgb(0xe3e3e3);
    related_.style.card.info_focus_text = gfx::Color::rgb(0x4a4a4a);
    related_.style.item_width = 330.0f;
    related_.style.counter = false;
    related_.title = "Suggestions";
    related_.set_bounds({96.0f, 1080.0f, 1728.0f, 370.0f});
    chapters_row_.style = related_.style;
    chapters_row_.style.item_width = 240.0f;
    chapters_row_.style.card.title_lines = 1;
    chapters_row_.style.card.title_size = 19.0f;
    chapters_row_.style.card.subtitle_size = 16.0f;
    chapters_row_.style.title_size = 26.0f;
    chapters_row_.title = "Chapters";
    chapters_row_.set_bounds({96.0f, 1080.0f, 1728.0f, 250.0f});
    related_.set_active(false);
}

PlayerScreen::~PlayerScreen()
{
    drop_texture();
}

void PlayerScreen::drop_texture()
{
    if (texture_ != 0)
    {
        const GLuint name = texture_;
        glDeleteTextures(1, &name);
        texture_ = 0;
    }
    texture_width_ = 0;
    texture_height_ = 0;
    last_pts_ = -1.0;
}

void PlayerScreen::enter()
{
    login_retry_sent_ = false;
    ended_ = false;
    in_related_ = false;
    in_chapters_ = false;
    related_.set_active(false);
    chrome_.set_lifted(false);
    chrome_.show();
    if (!context_.active_video.empty() && context_.active_video != playing_id_)
        request_video(context_.active_video);
}

void PlayerScreen::leave()
{
    if (player_ != nullptr)
    {
        context_.service.report_watch_progress(player_->clock_s());
        const std::string id = playing_id_.empty() ? requested_id_ : playing_id_;
        if (!id.empty())
            context_.resume.remember(id, player_->clock_s(), player_->duration_s());
    }
    reset_sponsorblock();
    player_.reset();
    drop_texture();
    playing_id_.clear();
    requested_id_.clear();
    related_items_.clear();
}

yt::CodecChoice PlayerScreen::codec_choice() const
{
    yt::CodecChoice choice;
    choice.avc = context_.settings.codec_avc;
    choice.vp9 = context_.settings.codec_vp9;
    choice.av1 = context_.settings.codec_av1;
    choice.max_height = context_.settings.max_height;
    choice.decode = context_.settings.decode_mode;
    if (context_.settings.prefer_avc && choice.avc)
    {
        // Settings > Player > Prefer AVC: VP9 and AV1 only when AVC is off.
        choice.vp9 = false;
        choice.av1 = false;
    }
    if (quality_override_ > 0)
    {
        choice.max_height = quality_override_;
        choice.exact_height = quality_override_;
    }
    choice.audio_track = audio_track_;
    return choice;
}

void PlayerScreen::request_video(const std::string &video_id)
{
    if (player_ != nullptr)
        context_.service.report_watch_progress(player_->clock_s());
    history_reported_s_ = 0.0;
    const std::string current = playing_id_.empty() ? requested_id_ : playing_id_;
    if (player_ != nullptr && !current.empty())
        context_.resume.remember(current, player_->clock_s(), player_->duration_s());
    if (video_id != current)
    {
        // Continue where this video was left last time (0: from the start).
        const double saved = context_.settings.resume ? context_.resume.position(video_id) : 0.0;
        resume_s_ = saved > 0.0 ? saved : -1.0;
        // Settings > Player > Remember speed off: each video starts at the
        // default speed.
        if (!context_.settings.remember_speed)
            speed_ = context_.settings.speed();
        skipped_ends_.clear();
        audio_track_.clear(); // a track id belongs to one video
    }
    requested_id_ = video_id;
    // Keep the app's notion of the open video in step, so the divergence check
    // in update() only fires on a video genuinely handed in from outside.
    context_.active_video = video_id;
    consumed_player_ = false;
    ended_ = false;
    reset_sponsorblock();
    player_.reset();
    drop_texture();
    context_.service.request_video(video_id, codec_choice());
}

void PlayerScreen::reset_sponsorblock()
{
    sponsor_.clear();
    sponsor_video_.clear();
    sponsor_here_.clear();
    sponsor_revision_ = sponsor_.revision();
    chrome_.set_ranges({});
}

void PlayerScreen::update_sponsorblock(const InputFrame &input, ui::Feedback &feedback)
{
    namespace sb = yt::sponsorblock;
    const Settings &settings = context_.settings;
    const yt::VideoView &view = video_view();
    // SponsorBlock off, or this channel excluded from it (video menu).
    const bool excluded = !view.info.channel_id.empty() &&
                          std::find(context_.sb_excluded.begin(), context_.sb_excluded.end(),
                                    view.info.channel_id) != context_.sb_excluded.end();
    if (!settings.sb_enabled || excluded || player_ == nullptr || playing_id_.empty())
    {
        if (!sponsor_video_.empty())
            reset_sponsorblock();
        return;
    }
    // SmartTube's per-category settings; a change reloads the list.
    sb::Options options;
    for (int i = 0; i < sb::kCategoryCount && i < Settings::kSbCategories; ++i)
        options.action[i] = static_cast<sb::Action>(std::clamp(settings.sb_act[i], 0, 4));
    options.min_skip_ms = static_cast<long>(settings.sb_min_seconds) * 1000;
    options.skip_once = settings.sb_once;
    options.alt_server = settings.sb_alt_server;
    if (sponsor_video_ != playing_id_ || !(sponsor_options_ == options))
    {
        reset_sponsorblock();
        sponsor_video_ = playing_id_;
        sponsor_options_ = options;
        sponsor_.load(playing_id_, options);
    }

    const double position = player_->clock_s();
    const double duration = player_->duration_s();
    const bool playing = !player_->paused() && !ended_ && player_->buffered_s() > 0.0;
    const sb::Decision decision = sponsor_.poll(position, duration, playing);

    // A new segment list: paint it onto the seek bar (Color markers), say
    // what a whole video is labelled, and remember the highlight.
    if (sponsor_.revision() != sponsor_revision_ || sponsor_markers_ != settings.sb_markers)
    {
        sponsor_revision_ = sponsor_.revision();
        sponsor_markers_ = settings.sb_markers;
        std::vector<ui::MediaRange> ranges;
        for (const sb::Segment &segment : sponsor_.segments())
        {
            if (segment.action == "full" || !settings.sb_markers)
                continue;
            ui::MediaRange range;
            range.start = static_cast<float>(segment.start_s);
            // A highlight is a point: a short mark.
            range.end = segment.action == "poi"
                            ? static_cast<float>(segment.start_s) +
                                  std::max(1.0f, static_cast<float>(duration) * 0.004f)
                            : static_cast<float>(segment.end_s);
            range.color = segment_color(segment.category);
            ranges.push_back(range);
        }
        chrome_.set_ranges(std::move(ranges));
        yt::log::line("sponsorblock: %zu segments on the bar (%s)", sponsor_.segments().size(),
                      sponsor_.status().c_str());
        const std::string label = sponsor_.whole_video_label();
        if (!label.empty() && settings.sb_act[9] != 0 && sponsor_notice_for_ != playing_id_)
        {
            sponsor_notice_for_ = playing_id_;
            context_.toasts.push(ui::StatusKind::info, "This video: " + label, "SponsorBlock",
                                 4.0f);
        }
    }

    // "Don't skip segments again" is the tracker's; the one-shot list here
    // keeps a skipped segment from being skipped on every poll after a seek.
    const bool skipped_before =
        settings.sb_once && std::any_of(skipped_ends_.begin(), skipped_ends_.end(), [&](double end)
                                        { return std::abs(end - decision.target_s) < 0.5; });
    if (decision.kind == sb::Kind::skip && !skipped_before)
    {
        skipped_ends_.push_back(decision.target_s);
        player_->seek(decision.target_s);
        if (!decision.silent)
            context_.toasts.push(ui::StatusKind::info, "Skipped " + decision.label, "SponsorBlock",
                                 2.5f);
        if (settings.sb_stats)
            sb::report_viewed(decision.uuid, settings.sb_alt_server);
        yt::log::line("sponsorblock: skipped %s to %.1f s", decision.label.c_str(),
                      decision.target_s);
    }

    // The segment under the playhead, for the Triangle hint; without one, a
    // highlight still ahead (Settings: Highlight > Offer jump).
    sponsor_here_.clear();
    sponsor_highlight_ = -1.0;
    for (const sb::Segment &segment : sponsor_.segments())
    {
        if (segment.action == "poi" || segment.action == "full")
            continue;
        if (segment.index >= 0 && segment.index < Settings::kSbCategories &&
            settings.sb_act[segment.index] == 0)
            continue;
        if (position >= segment.start_s && position < segment.end_s - 0.5)
        {
            sponsor_here_ = segment.label;
            break;
        }
    }
    const double highlight = sponsor_.highlight_s();
    if (sponsor_here_.empty() && settings.sb_act[8] != 0 && highlight > position + 2.0)
    {
        sponsor_highlight_ = highlight;
        sponsor_here_ = "to highlight";
    }

    if (input.is_pressed(Action::north) && !sponsor_here_.empty())
        skip_segment_now(feedback);
}

void PlayerScreen::skip_segment_now(ui::Feedback &feedback)
{
    if (player_ == nullptr)
        return;
    if (sponsor_highlight_ > 0.0)
    {
        // Triangle before the highlight: jump to the best part.
        player_->seek(sponsor_highlight_);
        context_.toasts.push(ui::StatusKind::info, "Jumped to the highlight", "SponsorBlock", 2.0f);
        sponsor_highlight_ = -1.0;
        sponsor_here_.clear();
        return;
    }
    const yt::sponsorblock::Decision manual =
        sponsor_.skip_now(player_->clock_s(), player_->duration_s());
    if (manual.kind == yt::sponsorblock::Kind::skip)
    {
        player_->seek(manual.target_s);
        context_.toasts.push(ui::StatusKind::info, "Skipped " + manual.label, "SponsorBlock", 2.5f);
        feedback.rumble(0.2f, 0.02f);
        sponsor_here_.clear();
    }
}

void PlayerScreen::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    clock_ += dt;
    const Settings &settings = context_.settings;
    // Settings > Player > Video speed: a new default applies at once.
    if (settings.speed_index != applied_speed_index_)
    {
        applied_speed_index_ = settings.speed_index;
        speed_ = settings.speed();
        if (player_ != nullptr)
            player_->set_speed(speed_);
    }
    {
        PlayerChrome::Options options;
        options.seek_step = static_cast<double>(settings.seek_seconds);
        options.hide_after = static_cast<float>(settings.ui_hide_seconds);
        options.clock = settings.show_clock;
        options.remaining = settings.show_remaining;
        options.quality = settings.show_quality;
        options.tooltips = settings.show_tooltips;
        options.ok_behavior = settings.ok_behavior;
        chrome_.set_options(options);
    }

    // Adopt the player the worker opened.
    if (!consumed_player_)
    {
        std::unique_ptr<yt::Player> fresh = context_.service.take_player();
        if (fresh != nullptr)
        {
            player_ = std::move(fresh);
            playing_id_ = requested_id_;
            consumed_player_ = true;
            ended_ = false;
            // Videos under 5 minutes start over unless the setting keeps them.
            const double length = video_view().info.length_s;
            if (resume_s_ > 0.0 &&
                (context_.settings.resume_short || length <= 0.0 || length >= 300.0))
                player_->seek(resume_s_);
            resume_s_ = -1.0;
            if (speed_ != 1.0)
                player_->set_speed(speed_);
        }
    }

    const yt::VideoView &view = video_view();
    waiting_s_ = player_ == nullptr && view.state == yt::Load::loading ? waiting_s_ + dt : 0.0f;

    // A new video handed to an already open player must start: the app only
    // moved active_video, so kick the service when the target diverges.
    if (!context_.active_video.empty() && context_.active_video != requested_id_ &&
        view.state != yt::Load::loading)
    {
        request_video(context_.active_video);
        return;
    }

    if (view.revision != video_revision_)
    {
        video_revision_ = view.revision;
        yt::log::line("player: state=%d stage=%s error=%s", static_cast<int>(view.state),
                      view.stage.c_str(), view.error.c_str());
    }
    if (view.state == yt::Load::failed && player_ == nullptr)
    {
        if (view.error == "login_required")
        {
            // A signed-out refusal is the one failure the user can fix: X goes
            // to the sign-in screen, and the video starts by itself after it.
            if (input.is_pressed(Action::confirm))
                context_.nav.start_login = true;
            else if (context_.service.signed_in() && !login_retry_sent_)
            {
                // The account arrived while this screen was up: try once more.
                const std::string id = playing_id_.empty() ? context_.active_video : playing_id_;
                if (!id.empty())
                {
                    login_retry_sent_ = true;
                    request_video(id);
                }
            }
        }
        if (input.is_pressed(Action::back))
            context_.nav.leave_player = true;
        return;
    }

    if (player_ != nullptr)
    {
        player_->update();
        const yt::VideoFrameView frame = player_->poll_frame();
        if (frame.fresh && frame.rgba != nullptr && frame.width > 0 && frame.height > 0)
        {
            drop_texture();
            texture_ = context_.batch.create_texture(frame.width, frame.height, frame.rgba);
            texture_width_ = frame.width;
            texture_height_ = frame.height;
            last_pts_ = frame.pts;
            // The upload reads width*height*4 bytes out of the player's pool
            // slot. The first one is logged at once and afterwards one line a
            // second at most, so yt.log's last lines name this step when the
            // run dies around it without logging every frame.
            const auto now = std::chrono::steady_clock::now();
            if (now - last_upload_log_ >= std::chrono::seconds(1))
            {
                last_upload_log_ = now;
                yt::log::line("ffmpeg: texture upload %dx%dx4", frame.width, frame.height);
            }
        }
        ended_ = player_->ended();
    }

    // Related row.
    const std::uint64_t related_revision = context_.service.revision(yt::FeedKind::related);
    if (related_revision != related_revision_)
    {
        related_revision_ = related_revision;
        const yt::FeedView feed = context_.service.feed(yt::FeedKind::related);
        related_items_ = feed.items;
        std::vector<ui::CardItem> cards;
        cards.reserve(related_items_.size());
        for (const yt::FeedItem &item : related_items_)
        {
            ui::CardItem card;
            card.title = item.title.empty() ? item.video_id : item.title;
            card.subtitle = item.author;
            card.texture = context_.thumbs.texture(card_thumbnail(item));
            card.image_aspect = 16.0f / 9.0f;
            if (item.live)
                card.badge = "LIVE";
            else
                card.badge = item.length_s > 0.0 ? format_duration(item.length_s) : item.badge;
            cards.push_back(std::move(card));
        }
        related_.set_items(std::move(cards));
    }

    // Watch history: the span watched, first after 10 s, then every 30 s.
    if (player_ != nullptr && !player_->paused() &&
        player_->clock_s() >= history_reported_s_ + (history_reported_s_ <= 0.0 ? 10.0 : 30.0))
    {
        history_reported_s_ = player_->clock_s();
        context_.service.report_watch_progress(history_reported_s_);
    }

    // Suggestion thumbnails that arrived after the row was filled.
    for (int index = 0; index < static_cast<int>(related_.items().size()) &&
                        index < static_cast<int>(related_items_.size());
         ++index)
    {
        if (related_.item(index).texture == 0)
            related_.item(index).texture = context_.thumbs.texture(
                card_thumbnail(related_items_[static_cast<std::size_t>(index)]));
    }

    // Transport.
    if (player_ != nullptr)
    {
        chrome_.set_times(player_->clock_s(), player_->duration_s(),
                          player_->clock_s() + player_->buffered_s());
        chrome_.set_playing(!player_->paused() && !ended_);
    }
    // Shuffle picks once per video (and again when the section arrives).
    if (settings.playback_mode == 4)
    {
        const std::string current = playing_id_.empty() ? requested_id_ : playing_id_;
        if (shuffle_for_ != current || shuffle_pick_.empty())
            choose_shuffle();
    }
    chrome_.set_mode(settings.playback_mode);
    chrome_.set_has_next(next_suggestion() != nullptr);
    {
        // SmartTube's line under the title: channel, then the view count.
        std::string meta = view.info.author;
        if (!view.info.view_count.empty())
            meta += (meta.empty() ? "" : "  \xE2\x80\xA2  ") +
                    grouped_digits(view.info.view_count) + " views";
        // The chapter playing, after the channel and views.
        if (player_ != nullptr && !chapters_.empty())
        {
            const int at = yt::chapter_at(chapters_, player_->clock_s());
            if (at >= 0)
                meta += (meta.empty() ? "" : "  \xE2\x80\xA2  ") +
                        chapters_[static_cast<std::size_t>(at)].title;
        }
        chrome_.set_info(view.info.title.empty() ? playing_id_ : view.info.title, meta,
                         player_ != nullptr ? player_->quality_label() : std::string());
        chrome_.set_clock(clock_line(context_.settings.time_24h));
    }

    // Chapters: the watch page's, else the description's timestamps.
    if (view.revision != chapters_revision_ || playing_id_ != chapters_for_)
    {
        chapters_revision_ = view.revision;
        chapters_for_ = playing_id_;
        chapters_ =
            !view.chapters.empty() ? view.chapters : yt::parse_chapters(view.info.description);
        chrome_.set_chapters(chapters_);
    }
    if (chrome_.take_seek() && player_ != nullptr)
        player_->seek(chrome_.seek_target());
    // Seek preview: the storyboard frame the bar points at.
    if (storyboard_for_ != playing_id_ && !view.info.storyboard_spec.empty())
    {
        storyboard_for_ = playing_id_;
        const double length = view.info.length_s > 0.0
                                  ? view.info.length_s
                                  : (player_ != nullptr ? player_->duration_s() : 0.0);
        if (!storyboard_.parse(view.info.storyboard_spec, length))
            yt::log::line("storyboard: unreadable spec");
    }
    if (chrome_.previewing() && storyboard_for_ == playing_id_ && !storyboard_.empty())
    {
        const yt::StoryboardFrame frame = storyboard_.frame_at(chrome_.preview_time());
        chrome_.set_preview(
            context_.thumbs.texture(frame.url), {frame.u, frame.v, frame.w, frame.h},
            frame.height > 0 ? static_cast<float>(frame.width) / static_cast<float>(frame.height)
                             : 16.0f / 9.0f);
    }
    else
        chrome_.set_preview(0, {0.0f, 0.0f, 1.0f, 1.0f}, 16.0f / 9.0f);

    update_sponsorblock(input, feedback);
    chrome_.set_segment(sponsor_here_);
    {
        static const char *const kSpeedLabels[] = {"0.25x", "0.5x", "0.75x", "1x",
                                                   "1.25x", "1.5x", "1.75x", "2x"};
        static const double kSpeeds[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0};
        std::string speed = "1x";
        for (std::size_t i = 0; i < 8; ++i)
        {
            if (std::abs(kSpeeds[i] - speed_) < 0.01)
                speed = kSpeedLabels[i];
        }
        std::string quality = player_ != nullptr ? player_->quality_label() : std::string();
        quality = quality.substr(0, quality.find(' '));
        chrome_.set_badges(!caption_language_.empty() && !caption_url_.empty(), speed, quality,
                           yt::audio_tracks(view.info.formats).size() > 1);
    }
    chrome_.set_has_channel(!view.info.channel_id.empty());
    chrome_.set_channel_avatar(
        view.channel_avatar.empty() ? 0u : context_.thumbs.texture(view.channel_avatar));
    chrome_.set_account(context_.service.signed_in() && view.state == yt::Load::ready,
                        view.like_status == "LIKE", view.like_status == "DISLIKE", view.subscribed);
    // Say once whether YouTube took the video into the account's history.
    if (!view.history_status.empty() && history_toast_for_ != view.info.video_id)
    {
        history_toast_for_ = view.info.video_id;
        // Only a failure is worth a word now that history works.
        if (view.history_status != "ok")
            context_.toasts.push(ui::StatusKind::warning, "Not added to YouTube history",
                                 view.history_status, 4.0f);
    }
    update_captions();

    bool back_used = false;
    if (!in_related_)
    {
        const PlayerChrome::Command command = chrome_.handle(input, feedback, &back_used);
        switch (command)
        {
        case PlayerChrome::Command::toggle_pause:
            if (player_ != nullptr)
            {
                if (ended_)
                    player_->seek(0.0);
                player_->set_paused(ended_ ? false : !player_->paused());
            }
            break;
        case PlayerChrome::Command::seek:
            if (player_ != nullptr)
                player_->seek(chrome_.seek_target());
            break;
        case PlayerChrome::Command::restart:
            if (player_ != nullptr)
                player_->seek(0.0);
            break;
        case PlayerChrome::Command::next:
            play_next();
            break;
        case PlayerChrome::Command::skip_segment:
            skip_segment_now(feedback);
            break;
        case PlayerChrome::Command::suggestions:
            // Down from the controls: the chapters first, then the
            // suggestions.
            if (!chapters_.empty() || (!related_items_.empty() && settings.suggestions))
            {
                in_related_ = true;
                in_chapters_ = !chapters_.empty();
                if (in_chapters_ && player_ != nullptr)
                    chapters_row_.set_focus(
                        std::max(0, yt::chapter_at(chapters_, player_->clock_s())));
                chrome_.set_lifted(true);
                feedback.play(audio::Cue::tab);
            }
            else
                feedback.play(audio::Cue::error);
            break;
        case PlayerChrome::Command::open_captions:
        case PlayerChrome::Command::open_speed:
        case PlayerChrome::Command::open_quality:
        case PlayerChrome::Command::open_audio:
        case PlayerChrome::Command::open_info:
        case PlayerChrome::Command::open_chapters:
            open_menu(command);
            break;
        case PlayerChrome::Command::cycle_mode:
        {
            // SmartTube's playback mode button: next, shuffle, repeat,
            // pause, close, and round again. It is the setting itself.
            static const int kOrder[] = {0, 4, 1, 2, 3};
            static const char *const kNames[] = {"Play next", "Repeat the video",
                                                 "Pause at the end", "Close at the end", "Shuffle"};
            int at = 0;
            for (int i = 0; i < 5; ++i)
                if (kOrder[i] == context_.settings.playback_mode)
                    at = i;
            context_.settings.playback_mode = kOrder[(at + 1) % 5];
            context_.settings_changed = true;
            shuffle_pick_.clear();
            context_.toasts.push(ui::StatusKind::info, kNames[context_.settings.playback_mode],
                                 "Playback mode", 1.5f);
            break;
        }
        case PlayerChrome::Command::pick:
            apply_pick();
            break;
        case PlayerChrome::Command::like:
        case PlayerChrome::Command::dislike:
        {
            // A second press on the lit button takes the rating back.
            const bool like = command == PlayerChrome::Command::like;
            const bool lit = view.like_status == (like ? "LIKE" : "DISLIKE");
            const std::string id = playing_id_.empty() ? requested_id_ : playing_id_;
            context_.service.rate_video(id, lit ? "removelike" : (like ? "like" : "dislike"));
            break;
        }
        case PlayerChrome::Command::save:
            context_.nav.add_to_playlist = true;
            context_.nav.playlist_item.video_id = playing_id_.empty() ? requested_id_ : playing_id_;
            context_.nav.playlist_item.title = view.info.title;
            context_.nav.playlist_item.author = view.info.author;
            break;
        case PlayerChrome::Command::channel:
            if (!view.info.channel_id.empty())
            {
                context_.pending_channel = {view.info.channel_id, view.info.author, {}, {}};
                context_.nav.open_channel = true;
            }
            break;
        case PlayerChrome::Command::subscribe:
            if (!view.info.channel_id.empty())
            {
                context_.service.set_subscribed(view.info.channel_id, !view.subscribed);
                context_.toasts.push(ui::StatusKind::info,
                                     view.subscribed ? "Unsubscribed" : "Subscribed",
                                     view.info.author, 2.0f);
            }
            break;
        case PlayerChrome::Command::none:
            break;
        }
        // L2 / R2: the previous / next chapter.
        if (input.is_pressed(Action::jump_prev))
            jump_chapter(-1);
        if (input.is_pressed(Action::jump_next))
            jump_chapter(1);
        if (input.is_pressed(Action::page_prev) && player_ != nullptr)
        {
            player_->seek(player_->clock_s() - static_cast<double>(settings.seek_seconds));
            chrome_.show();
        }
        if (input.is_pressed(Action::page_next) && player_ != nullptr)
        {
            player_->seek(player_->clock_s() + static_cast<double>(settings.seek_seconds));
            chrome_.show();
        }
    }
    else if (in_chapters_)
    {
        chrome_.show();
        const ui::Event event = chapters_row_.handle(input, feedback);
        if (event == ui::Event::activated)
        {
            const std::size_t index = static_cast<std::size_t>(chapters_row_.focus());
            if (index < chapters_.size() && player_ != nullptr)
                player_->seek(chapters_[index].start_s);
            in_related_ = false;
            in_chapters_ = false;
            chrome_.set_lifted(false);
        }
        else if (input.nav == Direction::down && !related_items_.empty() && settings.suggestions)
        {
            in_chapters_ = false;
            feedback.play(audio::Cue::focus);
        }
        else if (event == ui::Event::cancelled || input.nav == Direction::up)
        {
            in_related_ = false;
            in_chapters_ = false;
            chrome_.set_lifted(false);
            back_used = event == ui::Event::cancelled;
            if (input.nav == Direction::up)
                feedback.play(audio::Cue::focus);
        }
    }
    else
    {
        chrome_.show(); // the row keeps the controls up
        const ui::Event event = related_.handle(input, feedback);
        if (event == ui::Event::activated)
        {
            const std::size_t index = static_cast<std::size_t>(related_.focus());
            if (index < related_items_.size())
            {
                const std::string next = related_items_[index].video_id;
                in_related_ = false;
                chrome_.set_lifted(false);
                request_video(next);
            }
        }
        else if (input.nav == Direction::up && !chapters_.empty())
        {
            in_chapters_ = true; // up: the chapters row above
            feedback.play(audio::Cue::focus);
        }
        else if (event == ui::Event::cancelled || input.nav == Direction::up)
        {
            in_related_ = false;
            chrome_.set_lifted(false);
            back_used = event == ui::Event::cancelled;
            if (input.nav == Direction::up)
                feedback.play(audio::Cue::focus);
        }
    }
    related_.set_active(in_related_ && !in_chapters_);
    chapters_row_.set_active(in_related_ && in_chapters_);

    // The end of a video, as Settings > Player > Playback mode says: the next
    // suggestion (autoplay), the same video again, a pause, or back out.
    // A Short loops instead when "Loop Shorts" is on.
    if (!ended_)
        end_handled_ = false;
    else if (!in_related_ && !end_handled_ && player_ != nullptr)
    {
        end_handled_ = true;
        const bool tall = texture_width_ > 0 && texture_height_ > texture_width_;
        const int mode = settings.loop_shorts && tall ? 1 : settings.playback_mode;
        if (mode == 1)
        {
            player_->seek(0.0);
            player_->set_paused(false);
        }
        else if ((mode == 0 || mode == 4) && next_suggestion() != nullptr)
        {
            play_next();
            return;
        }
        else if (mode == 3)
        {
            context_.nav.leave_player = true;
            return;
        }
    }

    if (input.is_pressed(Action::back) && !back_used)
        context_.nav.leave_player = true;

    // The rows sit under the block and slide up with it: the chapters,
    // then the suggestions.
    fill_chapter_row();
    const float top = 1080.0f - rise() + 10.0f;
    if (!chapters_.empty())
    {
        chapters_row_.set_bounds({96.0f, top, 1728.0f, 270.0f});
        related_.set_bounds({96.0f, top + 300.0f, 1728.0f, 370.0f});
    }
    else
        related_.set_bounds({96.0f, top, 1728.0f, 370.0f});
    chrome_.update(dt);
    related_.update(dt);
    chapters_row_.update(dt);
}

float PlayerScreen::rise() const
{
    return (chapters_.empty() ? 400.0f : 700.0f) * chrome_.lift();
}

void PlayerScreen::fill_chapter_row()
{
    if (chapters_.empty())
        return;
    // Built when the chapters (or the storyboard) arrive; the pictures are
    // patched in as their sheets load.
    if (chapters_row_for_ != chapters_revision_ + 1 ||
        chapters_row_.items().size() != chapters_.size())
    {
        chapters_row_for_ = chapters_revision_ + 1;
        std::vector<ui::CardItem> cards;
        for (const yt::Chapter &chapter : chapters_)
        {
            ui::CardItem card;
            card.title = chapter.title;
            card.subtitle = chapter_time(chapter.start_s);
            card.image_aspect = 16.0f / 9.0f;
            cards.push_back(std::move(card));
        }
        chapters_row_.set_items(std::move(cards));
    }
    if (storyboard_.empty() || storyboard_for_ != playing_id_ || chrome_.lift() < 0.01f)
        return;
    for (std::size_t i = 0; i < chapters_.size(); ++i)
    {
        ui::CardItem &card = chapters_row_.item(static_cast<int>(i));
        // A moment into the chapter: its first frame is often a title card.
        const yt::StoryboardFrame frame = storyboard_.frame_at(chapters_[i].start_s + 2.0);
        card.texture = context_.thumbs.texture(frame.url);
        card.uv = {frame.u, frame.v, frame.w, frame.h};
    }
}

void PlayerScreen::choose_shuffle()
{
    const std::string current = playing_id_.empty() ? requested_id_ : playing_id_;
    shuffle_for_ = current;
    shuffle_pick_.clear();
    const auto candidates = [&](bool skip_played)
    {
        std::vector<const yt::FeedItem *> out;
        for (const std::vector<yt::FeedItem> *list :
             std::initializer_list<const std::vector<yt::FeedItem> *>{&context_.section,
                                                                      &related_items_})
        {
            for (const yt::FeedItem &item : *list)
            {
                if (item.video_id.size() != 11 || item.video_id == current)
                    continue;
                if (skip_played &&
                    std::find(played_.begin(), played_.end(), item.video_id) != played_.end())
                    continue;
                out.push_back(&item);
            }
            if (!out.empty())
                break; // the section first; the suggestions only without one
        }
        return out;
    };
    std::vector<const yt::FeedItem *> pool = candidates(true);
    if (pool.empty())
    {
        played_.clear(); // the whole section played: start a new round
        pool = candidates(false);
    }
    if (pool.empty())
        return;
    // xorshift: no <random> needed for one pick a video.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    rng_ ^= static_cast<std::uint32_t>(clock_ * 1000.0f);
    shuffle_pick_ = pool[rng_ % pool.size()]->video_id;
}

const yt::VideoView &PlayerScreen::video_view() const
{
    const std::uint64_t revision = context_.service.video_revision();
    if (revision != view_cache_revision_)
    {
        view_cache_revision_ = revision;
        view_cache_ = context_.service.video();
    }
    return view_cache_;
}

const yt::FeedItem *PlayerScreen::next_suggestion() const
{
    // The first suggestion that is not this video: the list YouTube answers
    // can start with the one playing.
    const std::string current = playing_id_.empty() ? requested_id_ : playing_id_;
    // "Play next" picks come first.
    for (const yt::FeedItem &item : context_.play_queue)
    {
        if (!item.video_id.empty() && item.video_id != current)
            return &item;
    }
    // Shuffle: the pick made for this video, from the section.
    if (context_.settings.playback_mode == 4 && !shuffle_pick_.empty())
        for (const std::vector<yt::FeedItem> *list :
             std::initializer_list<const std::vector<yt::FeedItem> *>{&context_.section,
                                                                      &related_items_})
            for (const yt::FeedItem &item : *list)
                if (item.video_id == shuffle_pick_)
                    return &item;
    for (const yt::FeedItem &item : related_items_)
    {
        if (!item.video_id.empty() && item.video_id != current)
            return &item;
    }
    return nullptr;
}

void PlayerScreen::play_next()
{
    const yt::FeedItem *next = next_suggestion();
    if (next == nullptr)
        return;
    const std::string id = next->video_id;
    const std::string current = playing_id_.empty() ? requested_id_ : playing_id_;
    if (!current.empty())
        played_.push_back(current);
    if (played_.size() > 500)
        played_.erase(played_.begin());
    // A queued pick is played once.
    auto &queue = context_.play_queue;
    queue.erase(std::remove_if(queue.begin(), queue.end(),
                               [&](const yt::FeedItem &item) { return item.video_id == id; }),
                queue.end());
    yt::log::line("player: next -> %s", id.c_str());
    request_video(id);
    chrome_.show();
}

void PlayerScreen::reopen()
{
    const std::string id = playing_id_.empty() ? requested_id_ : playing_id_;
    if (id.empty())
        return;
    const double at = player_ != nullptr ? player_->clock_s() : 0.0;
    request_video(id);
    resume_s_ = at; // after request_video: the same id keeps its place
}

void PlayerScreen::open_menu(PlayerChrome::Command command)
{
    const yt::VideoView &view = video_view();
    switch (command)
    {
    case PlayerChrome::Command::open_captions:
    {
        std::vector<std::string> options{"Off"};
        int selected = 0;
        for (std::size_t i = 0; i < view.info.captions.size(); ++i)
        {
            options.push_back(view.info.captions[i].name);
            if (!caption_url_.empty() && view.info.captions[i].url == caption_url_)
                selected = static_cast<int>(i) + 1;
        }
        if (options.size() == 1)
            options[0] = "Off (this video has no subtitles)";
        chrome_.open_panel(PlayerChrome::Panel::captions, "Subtitles", std::move(options),
                           selected);
        break;
    }
    case PlayerChrome::Command::open_speed:
    {
        static const double kSpeeds[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0};
        std::vector<std::string> options{"0.25x", "0.5x", "0.75x", "Normal",
                                         "1.25x", "1.5x", "1.75x", "2x"};
        int selected = 3;
        for (int i = 0; i < 8; ++i)
        {
            if (std::abs(kSpeeds[i] - speed_) < 0.01)
                selected = i;
        }
        chrome_.open_panel(PlayerChrome::Panel::speed, "Playback speed", std::move(options),
                           selected);
        break;
    }
    case PlayerChrome::Command::open_quality:
    {
        // The heights this video offers in the codecs Settings allow.
        const yt::CodecChoice choice = codec_choice();
        menu_heights_.clear();
        for (const yt::Format &format : view.info.formats)
        {
            if (!yt::is_video_codec(format.codec) || format.height <= 0)
                continue;
            if ((format.codec == yt::Codec::avc && !choice.avc) ||
                (format.codec == yt::Codec::vp9 && !choice.vp9) ||
                (format.codec == yt::Codec::av1 && !choice.av1))
                continue;
            if (std::find(menu_heights_.begin(), menu_heights_.end(), format.height) ==
                menu_heights_.end())
                menu_heights_.push_back(format.height);
        }
        std::sort(menu_heights_.rbegin(), menu_heights_.rend());
        std::vector<std::string> options{"Auto (up to " +
                                         std::to_string(context_.settings.max_height) + "p)"};
        int selected = 0;
        for (std::size_t i = 0; i < menu_heights_.size(); ++i)
        {
            // SmartTube's line: "1080p, 30fps, 5.02Mbps, avc", from the
            // format that height would play.
            yt::CodecChoice exact = choice;
            exact.exact_height = menu_heights_[i];
            exact.max_height = menu_heights_[i];
            const yt::Format *format = yt::select_video_format(view.info.formats, exact);
            std::string line = std::to_string(menu_heights_[i]) + "p";
            if (format != nullptr && format->height == menu_heights_[i])
            {
                char extra[64];
                std::snprintf(extra, sizeof(extra), ", %dfps, %.2fMbps, %s",
                              static_cast<int>(format->fps + 0.5),
                              static_cast<double>(format->bitrate) / 1e6,
                              yt::codec_label(format->codec));
                line += extra;
                yt::CodecChoice ceiling = choice;
                ceiling.max_height = 0;
                const int limit = yt::codec_height_ceiling(format->codec, ceiling);
                if (limit > 0 && format->height > limit)
                    line += "  (may stutter)";
            }
            options.push_back(line);
            if (menu_heights_[i] == quality_override_)
                selected = static_cast<int>(i) + 1;
        }
        chrome_.open_panel(PlayerChrome::Panel::quality, "Quality", std::move(options), selected);
        break;
    }
    case PlayerChrome::Command::open_chapters:
    {
        // SmartTube's chapter list: the start and the name; the current one
        // is marked, a pick jumps there.
        std::vector<std::string> options;
        for (const yt::Chapter &chapter : chapters_)
            options.push_back(chapter_time(chapter.start_s) + "   " + chapter.title);
        const int at = player_ != nullptr ? yt::chapter_at(chapters_, player_->clock_s()) : 0;
        chrome_.open_panel(PlayerChrome::Panel::chapters, "Chapters", std::move(options),
                           std::max(0, at));
        break;
    }
    case PlayerChrome::Command::open_info:
    {
        // The description, wrapped to the panel; Up/Down scroll it.
        std::vector<std::string> lines;
        std::string head = view.info.author;
        if (!view.info.view_count.empty())
        {
            // "1234567" -> "1,234,567 views"
            std::string digits = view.info.view_count;
            std::string grouped;
            for (std::size_t i = 0; i < digits.size(); ++i)
            {
                if (i > 0 && (digits.size() - i) % 3 == 0)
                    grouped += ',';
                grouped += digits[i];
            }
            head += (head.empty() ? "" : "  \xE2\x80\xA2  ") + grouped + " views";
        }
        if (!head.empty())
            lines.push_back(head);
        const std::string text =
            view.info.description.empty() ? std::string("No description.") : view.info.description;
        std::size_t at = 0;
        while (at <= text.size() && context_.fonts.regular.font != nullptr)
        {
            const std::size_t end = text.find('\n', at);
            const std::string paragraph =
                text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (paragraph.empty())
                lines.push_back(" ");
            else
                for (const std::string &line :
                     context_.fonts.regular.font->wrap(paragraph, 26.0f, 440.0f))
                    lines.push_back(line);
            if (end == std::string::npos || lines.size() > 400)
                break;
            at = end + 1;
        }
        chrome_.open_panel(PlayerChrome::Panel::info,
                           view.info.title.empty() ? std::string("Info") : view.info.title,
                           std::move(lines), -1);
        break;
    }
    case PlayerChrome::Command::open_audio:
    {
        const std::vector<yt::AudioTrack> tracks = yt::audio_tracks(view.info.formats);
        menu_tracks_.clear();
        std::vector<std::string> options;
        int selected = 0;
        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            menu_tracks_.push_back(tracks[i].id);
            options.push_back(tracks[i].name + (tracks[i].is_default ? "  (original)" : ""));
            if ((audio_track_.empty() && tracks[i].is_default) || tracks[i].id == audio_track_)
                selected = static_cast<int>(i);
        }
        chrome_.open_panel(PlayerChrome::Panel::audio, "Audio track", std::move(options), selected);
        break;
    }
    default:
        break;
    }
}

void PlayerScreen::apply_pick()
{
    const int pick = chrome_.picked();
    const PlayerChrome::Panel panel = chrome_.panel();
    chrome_.close_panel();
    const yt::VideoView &view = video_view();
    switch (panel)
    {
    case PlayerChrome::Panel::captions:
        captions_chosen_ = true;
        if (pick <= 0 || pick > static_cast<int>(view.info.captions.size()))
        {
            caption_language_.clear();
            caption_url_.clear();
            captions_.clear();
        }
        else
        {
            const yt::CaptionTrack &track = view.info.captions[static_cast<std::size_t>(pick - 1)];
            caption_language_ = track.language;
            caption_url_ = track.url;
            captions_for_ = playing_id_;
            captions_.load(track.url);
            yt::log::line("captions: %s (%s)", track.name.c_str(), track.language.c_str());
        }
        break;
    case PlayerChrome::Panel::speed:
    {
        static const double kSpeeds[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0};
        if (pick >= 0 && pick < 8)
        {
            speed_ = kSpeeds[pick];
            if (player_ != nullptr)
                player_->set_speed(speed_);
        }
        break;
    }
    case PlayerChrome::Panel::quality:
    {
        const int height = pick <= 0 || pick > static_cast<int>(menu_heights_.size())
                               ? 0
                               : menu_heights_[static_cast<std::size_t>(pick - 1)];
        if (height != quality_override_)
        {
            quality_override_ = height;
            reopen();
        }
        break;
    }
    case PlayerChrome::Panel::audio:
        if (pick >= 0 && pick < static_cast<int>(menu_tracks_.size()) &&
            menu_tracks_[static_cast<std::size_t>(pick)] != audio_track_)
        {
            audio_track_ = menu_tracks_[static_cast<std::size_t>(pick)];
            reopen();
        }
        break;
    case PlayerChrome::Panel::chapters:
        if (pick >= 0 && pick < static_cast<int>(chapters_.size()) && player_ != nullptr)
            player_->seek(chapters_[static_cast<std::size_t>(pick)].start_s);
        break;
    case PlayerChrome::Panel::info:
    case PlayerChrome::Panel::none:
        break;
    }
}

void PlayerScreen::jump_chapter(int step)
{
    if (player_ == nullptr || chapters_.empty())
        return;
    const double position = player_->clock_s();
    int at = yt::chapter_at(chapters_, position);
    // Back from a few seconds into a chapter restarts it, as a previous
    // track button does; from its start it goes to the one before.
    if (step < 0 && at >= 0 && position - chapters_[static_cast<std::size_t>(at)].start_s > 3.0)
        step = 0;
    const int target = std::clamp(at + step, 0, static_cast<int>(chapters_.size()) - 1);
    if (step > 0 && target == at)
        return; // the last chapter: nothing after it
    const yt::Chapter &chapter = chapters_[static_cast<std::size_t>(target)];
    player_->seek(chapter.start_s);
    chrome_.show();
    context_.toasts.push(ui::StatusKind::info, chapter.title, "Chapter", 2.0f);
}

void PlayerScreen::update_captions()
{
    const yt::VideoView &view = video_view();
    // Settings > Subtitles > Subtitle language: on by itself until the user
    // picks captions (or Off) in the player.
    const int language = context_.settings.subtitle_language;
    if (caption_language_.empty() && !captions_chosen_ && language > 0 &&
        language < Settings::kSubtitleLanguageCount)
        caption_language_ = Settings::kSubtitleLanguages[language][0];
    // A new video: turn the chosen language on again when it has it.
    if (!caption_language_.empty() && !playing_id_.empty() && captions_for_ != playing_id_ &&
        view.state == yt::Load::ready && !view.info.captions.empty())
    {
        captions_for_ = playing_id_;
        caption_url_.clear();
        captions_.clear();
        const yt::CaptionTrack *best = nullptr;
        for (const yt::CaptionTrack &track : view.info.captions)
        {
            if (track.language == caption_language_ &&
                (best == nullptr || (best->automatic && !track.automatic)))
                best = &track;
        }
        if (best != nullptr)
        {
            caption_url_ = best->url;
            captions_.load(best->url);
        }
    }
    caption_text_.clear();
    if (!caption_url_.empty() && player_ != nullptr)
        caption_text_ = captions_.text_at(player_->clock_s());
}

void PlayerScreen::draw_captions(ui::Canvas &canvas, const Context &context) const
{
    if (caption_text_.empty() || context.fonts.semibold.font == nullptr)
        return;
    // Settings > Subtitles: text size and the box behind it.
    static const float kSizes[] = {30.0f, 38.0f, 48.0f};
    const float size = kSizes[std::clamp(context.settings.subtitle_size, 0, 2)];
    const float pitch = size + 14.0f;
    const bool box = context.settings.subtitle_background;
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at <= caption_text_.size())
    {
        const std::size_t end = caption_text_.find('\n', at);
        const std::string line =
            caption_text_.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!line.empty())
        {
            // Long lines wrap at 1400 pixels.
            for (const std::string &part : context.fonts.semibold.font->wrap(line, size, 1400.0f))
                lines.push_back(part);
        }
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    if (lines.size() > 4)
        lines.erase(lines.begin(), lines.end() - 4);
    // Above the controls while they are up, near the bottom edge otherwise.
    const float bottom = 1030.0f - (rise() + 290.0f) * chrome_.visibility();
    float y = bottom - static_cast<float>(lines.size()) * pitch;
    for (const std::string &line : lines)
    {
        const float width = context.fonts.semibold.measure(line, size);
        if (box)
            canvas.list.rounded_rect(
                {gfx::kVirtualWidth * 0.5f - width * 0.5f - 16.0f, y, width + 32.0f, pitch - 2.0f},
                6.0f, gfx::Color::rgb(0x000000, 0.72f));
        else // without the box a dark copy underneath keeps it readable
            ui::text(canvas.list, context.fonts.semibold, line, gfx::kVirtualWidth * 0.5f + 2.0f,
                     y + size + 2.0f, size, gfx::Color::rgb(0x000000, 0.9f), gfx::Align::center);
        ui::text(canvas.list, context.fonts.semibold, line, gfx::kVirtualWidth * 0.5f, y + size,
                 size, gfx::Color::rgb(0xffffff), gfx::Align::center);
        y += pitch;
    }
}

void PlayerScreen::draw_hud(ui::Canvas &canvas, const Context &context) const
{
    (void)context;
    (void)canvas;
}

void PlayerScreen::draw(const Context &context, Frame &frame) const
{
    ui::Canvas canvas{frame.scene, context.fonts, frame.glass_texture, clock_};
    const yt::VideoView &view = video_view();

    // The picture, letterboxed into the virtual canvas.
    if (texture_ != 0 && texture_width_ > 0 && texture_height_ > 0)
    {
        const float scale = std::min(gfx::kVirtualWidth / static_cast<float>(texture_width_),
                                     gfx::kVirtualHeight / static_cast<float>(texture_height_));
        const float width = static_cast<float>(texture_width_) * scale;
        const float height = static_cast<float>(texture_height_) * scale;
        const gfx::Rect target{(gfx::kVirtualWidth - width) * 0.5f,
                               (gfx::kVirtualHeight - height) * 0.5f, width, height};
        canvas.list.image(texture_, target, gfx::kFullUv, gfx::Color::rgb(0xffffff), 0.0f);
    }
    else
    {
        canvas.list.rounded_rect({0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0.0f,
                                 gfx::Color::rgb(0x000000));
    }

    if (view.state == yt::Load::failed && player_ == nullptr)
    {
        const bool no_stream = view.error.starts_with("no_stream");
        const bool needs_login = view.error == "login_required";
        const bool timed_out = view.error == "timeout";
        const char *headline = needs_login ? "Sign-in required"
                               : no_stream ? kNoStream
                               : timed_out ? "Playback timed out"
                                           : "Playback failed";
        ui::text(canvas.list, context.fonts.semibold, headline, gfx::kVirtualWidth * 0.5f, 470.0f,
                 36.0f,
                 (needs_login || no_stream) ? gfx::Color::rgb(0xf5a524) : gfx::Color::rgb(0xe5484d),
                 gfx::Align::center);
        if (!no_stream && !timed_out && !view.error.empty())
            ui::paragraph(canvas.list, context.fonts.regular, view.error, 460.0f, 510.0f, 22.0f,
                          1000.0f, 30.0f, gfx::Color::rgb(0xaaaaaa), 3, gfx::Align::center);
        if (no_stream)
        {
            ui::text(canvas.list, context.fonts.regular, "Enable another codec in Settings.",
                     gfx::kVirtualWidth * 0.5f, 520.0f, 24.0f, gfx::Color::rgb(0xaaaaaa),
                     gfx::Align::center);
            // The failure text carries the format census after the "no_stream"
            // marker; show it so the television names the cause as well.
            std::string detail = view.error.substr(std::string("no_stream").size());
            while (!detail.empty() && (detail.front() == ' ' || detail.front() == '('))
                detail.erase(detail.begin());
            while (!detail.empty() && (detail.back() == ' ' || detail.back() == ')'))
                detail.pop_back();
            if (!detail.empty())
                ui::paragraph(canvas.list, context.fonts.regular, detail, 460.0f, 556.0f, 20.0f,
                              1000.0f, 26.0f, gfx::Color::rgb(0x999999), 2, gfx::Align::center);
        }
        if (timed_out)
            ui::text(canvas.list, context.fonts.regular,
                     "The stream did not answer in time. Details: /download0/hui/dev/yt.log",
                     gfx::kVirtualWidth * 0.5f, 520.0f, 24.0f, gfx::Color::rgb(0xaaaaaa),
                     gfx::Align::center);
        if (needs_login)
            ui::text(canvas.list, context.fonts.regular,
                     "YouTube requires a signed-in account for playback on this network.",
                     gfx::kVirtualWidth * 0.5f, 520.0f, 24.0f, gfx::Color::rgb(0xaaaaaa),
                     gfx::Align::center);
        // Name the stage that failed and, once the request has waited, show the
        // log tail so the failure is readable on the television.
        if (!view.stage.empty())
            ui::text(canvas.list, context.fonts.regular, view.stage, gfx::kVirtualWidth * 0.5f,
                     556.0f, 22.0f, gfx::Color::rgb(0x777777), gfx::Align::center);
        // Always: the log names which client and which URL failed, readable
        // on the television without FTP.
        draw_log_tail(canvas, context);
        const char *hint = needs_login ? "X: sign in      Circle: back" : "Circle: back";
        ui::text(canvas.list, context.fonts.regular, hint, gfx::kVirtualWidth * 0.5f, 600.0f, 24.0f,
                 gfx::Color::rgb(0x777777), gfx::Align::center);
        return;
    }

    if (view.state == yt::Load::loading || (!consumed_player_ && player_ == nullptr))
    {
        // YouTube's spinner: a red arc that turns while it grows and
        // shrinks (Material's indeterminate progress circle).
        constexpr float kPi = 3.14159265f;
        constexpr float kCycle = 1.4f;
        const float cycles = clock_ / kCycle;
        const float phase = cycles - std::floor(cycles);
        const auto ease = [](float x) { return x * x * (3.0f - 2.0f * x); };
        const float grown = 1.35f * kPi;
        float start = clock_ * 4.0f + std::floor(cycles) * grown;
        float sweep = 0.0f;
        if (phase < 0.5f)
            sweep = 0.15f * kPi + grown * ease(phase / 0.5f);
        else
        {
            const float shrink = ease((phase - 0.5f) / 0.5f);
            sweep = 0.15f * kPi + grown * (1.0f - shrink);
            start += grown * shrink;
        }
        canvas.list.arc(gfx::kVirtualWidth * 0.5f, gfx::kVirtualHeight * 0.5f, 52.0f, 7.0f, start,
                        sweep, gfx::Color::rgb(0xe52d27));
        // Only a long wait says more: what the app is waiting on.
        if (waiting_s_ >= 8.0f)
        {
            if (!view.stage.empty())
                ui::text(canvas.list, context.fonts.regular, view.stage, gfx::kVirtualWidth * 0.5f,
                         640.0f, 24.0f, gfx::Color::rgb(0x9e9e9e), gfx::Align::center);
            draw_log_tail(canvas, context);
        }
        return;
    }

    draw_captions(canvas, context);

    // The controls, and the suggestions row under them when it is open.
    const float lift = chrome_.lift();
    chrome_.draw(canvas, context.fonts, rise());
    if (lift > 0.01f)
    {
        canvas.list.push_opacity(std::min(1.0f, lift * 1.5f));
        if (!chapters_.empty())
            chapters_row_.draw(canvas);
        if (!related_items_.empty())
            related_.draw(canvas);
        canvas.list.pop_opacity();
    }

    if (player_ != nullptr && ended_)
        ui::text(canvas.list, context.fonts.semibold, "Ended", gfx::kVirtualWidth * 0.5f, 500.0f,
                 30.0f, gfx::Color::rgb(0xf5a524), gfx::Align::center);

    if (!sponsor_here_.empty() && chrome_.visibility() < 0.5f)
    {
        // The skip offer while the controls are away (they carry their own
        // "Skip" button): bottom right, Triangle skips.
        const std::string text = "Triangle  Skip " + sponsor_here_;
        constexpr float kSize = 26.0f;
        const float width = (context.fonts.semibold.font != nullptr
                                 ? context.fonts.semibold.font->measure(text, kSize)
                                 : 14.0f * static_cast<float>(text.size())) +
                            48.0f;
        const gfx::Rect pill{1824.0f - width, 940.0f, width, 58.0f};
        canvas.list.rounded_rect(pill, 29.0f, gfx::Color::rgb(0x000000, 0.75f));
        ui::text(canvas.list, context.fonts.semibold, text, pill.x + 24.0f, pill.y + 40.0f, kSize,
                 gfx::Color::rgb(0xf1f1f1));
    }
}

std::span<const TourStep> PlayerScreen::tour() const
{
    return kTour;
}

std::unique_ptr<Screen> make_player_screen(Context &context)
{
    return std::make_unique<PlayerScreen>(context);
}

} // namespace hui::app

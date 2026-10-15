// youtube-tv - The player screen.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The service resolves the formats and opens the demuxers on its worker;
// this screen only owns the picture: it uploads the frame the player hands
// over, draws the transport controls and mirrors their commands onto the
// player. Circle leaves (and closes the player through the app).

#include "app/player_screen.hpp"

#include "app/feed_view.hpp"
#include "app/thumbs.hpp"
#include "yt/log.hpp"
#include "app/theme.hpp"
#include "gfx/gl_batch.hpp"

#include <GL/glcorearb.h>

#include <chrono>

namespace hui::app
{
namespace
{

const TourStep kTour[] = {
    {2.2f, 0, Direction::none, "video", 0, 0, 0, 0, 0},
    {0.8f, 0, Direction::none, "controls", 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::down, nullptr, 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::right, "related", 0, 0, 0, 0, 0},
};

constexpr const char *kNoStream = "No stream for the enabled codecs";

// The last log lines, drawn at the bottom-left of the picture just above the
// hint row: a stuck or failed player becomes readable on the television
// without FTP. Newest last, each line clipped to 96 characters.
void draw_log_tail(ui::Canvas &canvas, const Context &context)
{
    std::string lines[6];
    const std::size_t count = yt::log::recent(lines, 6);
    if (count == 0)
        return;
    constexpr float kLineHeight = 22.0f;
    constexpr std::size_t kMaxChars = 96;
    float y = 1010.0f - static_cast<float>(count - 1) * kLineHeight;
    for (std::size_t i = 0; i < count; ++i)
    {
        std::string shown = lines[i];
        if (shown.size() > kMaxChars)
            shown.resize(kMaxChars);
        ui::text(canvas.list, context.fonts.mono, shown, 150.0f, y, 18.0f,
                 gfx::Color::rgb(0x8a8a8a));
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
    return gfx::Color::rgb(0xaaaaaa);
}

} // namespace

PlayerScreen::PlayerScreen(Context &context) : context_(context)
{
    controls_.style.theme = youtube_theme();
    controls_.style.auto_hide = 3.0f;
    controls_.style.show_volume = false;
    controls_.style.show_shuffle = false;
    controls_.style.show_repeat = false;
    controls_.style.skip_seconds = 10.0f;
    controls_.style.exits.down = true;
    controls_.set_bounds({480.0f, 840.0f, 960.0f, 170.0f});

    related_.style.theme = youtube_theme();
    related_.style.card.art_aspect = 16.0f / 9.0f;
    related_.style.card.text = ui::CardText::below;
    related_.style.card.title_size = 21.0f;
    related_.set_bounds({150.0f, 560.0f, 1620.0f, 250.0f});
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
    controls_.set_active(true);
    related_.set_active(false);
    controls_.wake();
    if (!context_.active_video.empty() && context_.active_video != playing_id_)
        request_video(context_.active_video);
}

void PlayerScreen::leave()
{
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
    return choice;
}

void PlayerScreen::request_video(const std::string &video_id)
{
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
    controls_.set_ranges({});
}

void PlayerScreen::update_sponsorblock(const InputFrame &input, ui::Feedback &feedback)
{
    namespace sb = yt::sponsorblock;
    const Settings &settings = context_.settings;
    if (!settings.sb_enabled || player_ == nullptr || playing_id_.empty())
    {
        if (!sponsor_video_.empty())
            reset_sponsorblock();
        return;
    }
    // Start (or restart, when the category setting moved) the lookup.
    if (sponsor_video_ != playing_id_ || sponsor_scope_ != settings.sb_scope)
    {
        reset_sponsorblock();
        sponsor_video_ = playing_id_;
        sponsor_scope_ = settings.sb_scope;
        sponsor_.load(playing_id_, static_cast<sb::Scope>(settings.sb_scope));
    }

    const double position = player_->clock_s();
    const double duration = player_->duration_s();
    const bool playing = !player_->paused() && !ended_ && player_->buffered_s() > 0.0;
    const sb::Decision decision =
        sponsor_.poll(position, duration, playing, static_cast<sb::Mode>(settings.sb_mode));

    // A new segment list: paint it onto the seek bar.
    if (sponsor_.revision() != sponsor_revision_)
    {
        sponsor_revision_ = sponsor_.revision();
        std::vector<ui::MediaRange> ranges;
        for (const sb::Segment &segment : sponsor_.segments())
        {
            ui::MediaRange range;
            range.start = static_cast<float>(segment.start_s);
            range.end = static_cast<float>(segment.end_s);
            range.color = segment_color(segment.category);
            ranges.push_back(range);
        }
        controls_.set_ranges(std::move(ranges));
        yt::log::line("sponsorblock: %zu segments on the bar (%s)", sponsor_.segments().size(),
                      sponsor_.status().c_str());
    }

    if (decision.kind == sb::Kind::skip)
    {
        player_->seek(decision.target_s);
        context_.toasts.push(ui::StatusKind::info, "Skipped " + decision.label, "SponsorBlock",
                             2.5f);
        yt::log::line("sponsorblock: skipped %s to %.1f s", decision.label.c_str(),
                      decision.target_s);
    }

    // The segment under the playhead, for the Triangle hint.
    sponsor_here_.clear();
    for (const sb::Segment &segment : sponsor_.segments())
    {
        if (position >= segment.start_s && position < segment.end_s - 0.5)
        {
            sponsor_here_ = segment.label;
            break;
        }
    }

    if (input.is_pressed(Action::north) && !sponsor_here_.empty())
    {
        const sb::Decision manual = sponsor_.skip_now(position, duration);
        if (manual.kind == sb::Kind::skip)
        {
            player_->seek(manual.target_s);
            context_.toasts.push(ui::StatusKind::info, "Skipped " + manual.label,
                                 "SponsorBlock", 2.5f);
            feedback.rumble(0.2f, 0.02f);
            sponsor_here_.clear();
        }
    }
}

void PlayerScreen::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    clock_ += dt;

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
        }
    }

    const yt::VideoView view = context_.service.video();

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
            card.texture = context_.thumbs.texture(item.thumbnail_url);
            if (item.live)
                card.badge = "LIVE";
            else
                card.badge = format_duration(item.length_s);
            cards.push_back(std::move(card));
        }
        related_.set_items(std::move(cards));
    }

    // Transport.
    if (player_ != nullptr)
    {
        controls_.set_duration(static_cast<float>(player_->duration_s()));
        controls_.set_position(static_cast<float>(player_->clock_s()));
        controls_.set_buffered(static_cast<float>(player_->clock_s() + player_->buffered_s()));
        controls_.set_playing(!player_->paused());
    }

    update_sponsorblock(input, feedback);

    const bool controls_active = !in_related_;
    controls_.set_active(controls_active);
    related_.set_active(in_related_);

    if (controls_active)
    {
        const ui::Event event = controls_.handle(input, feedback);
        if (event == ui::Event::changed || event == ui::Event::activated)
        {
            if (player_ != nullptr)
            {
                switch (controls_.command())
                {
                case ui::MediaCommand::play:
                    player_->set_paused(false);
                    break;
                case ui::MediaCommand::pause:
                    player_->set_paused(true);
                    break;
                case ui::MediaCommand::rewind:
                    player_->seek(player_->clock_s() - 10.0);
                    break;
                case ui::MediaCommand::forward:
                    player_->seek(player_->clock_s() + 10.0);
                    break;
                case ui::MediaCommand::seek:
                {
                    const double target = controls_.seek_position();
                    if (target >= 0.0)
                    {
                        player_->seek(target);
                        feedback.rumble(0.2f, 0.02f);
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }
        if (controls_.exit() == Direction::down)
        {
            in_related_ = !related_items_.empty();
            feedback.play(audio::Cue::tab);
        }
        if (input.is_pressed(Action::page_prev) && player_ != nullptr)
            player_->seek(player_->clock_s() - 10.0);
        if (input.is_pressed(Action::page_next) && player_ != nullptr)
            player_->seek(player_->clock_s() + 10.0);
        if (input.is_pressed(Action::confirm) && player_ != nullptr)
            player_->set_paused(!player_->paused());
    }
    else
    {
        const ui::Event event = related_.handle(input, feedback);
        if (event == ui::Event::activated)
        {
            const std::size_t index = static_cast<std::size_t>(related_.focus());
            if (index < related_items_.size())
            {
                const std::string next = related_items_[index].video_id;
                in_related_ = false;
                related_.set_active(false);
                controls_.set_active(true);
                controls_.wake();
                request_video(next);
            }
        }
        else if (event == ui::Event::cancelled)
        {
            in_related_ = false;
        }
    }

    if (input.is_pressed(Action::back))
    {
        if (in_related_)
            in_related_ = false;
        else
            context_.nav.leave_player = true;
    }
    if (ended_ && !in_related_)
    {
        // Stay on the last frame; Circle leaves.
        controls_.set_playing(false);
    }

    controls_.update(dt);
    related_.update(dt);
}

void PlayerScreen::draw_hud(ui::Canvas &canvas, const Context &context) const
{
    (void)context;
    (void)canvas;
}

void PlayerScreen::draw(const Context &context, Frame &frame) const
{
    ui::Canvas canvas{frame.scene, context.fonts, frame.glass_texture, clock_};
    const yt::VideoView view = context.service.video();

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
        if (view.waiting_s >= 3.0f)
            draw_log_tail(canvas, context);
        const char *hint = needs_login ? "X: sign in      Circle: back" : "Circle: back";
        ui::text(canvas.list, context.fonts.regular, hint, gfx::kVirtualWidth * 0.5f, 600.0f, 24.0f,
                 gfx::Color::rgb(0x777777), gfx::Align::center);
        return;
    }

    if (view.state == yt::Load::loading || (!consumed_player_ && player_ == nullptr))
    {
        std::string text = "Loading video";
        if (!view.stage.empty())
            text += " - " + view.stage;
        text += "  " + std::to_string(static_cast<int>(view.waiting_s)) + " s";
        ui::text(canvas.list, context.fonts.semibold, text, gfx::kVirtualWidth * 0.5f, 500.0f,
                 34.0f, gfx::Color::rgb(0xf1f1f1), gfx::Align::center);
        if (view.waiting_s >= 3.0f)
            draw_log_tail(canvas, context);
        return;
    }

    // Everything over the picture - scrim, title, "Up next" - follows the
    // controls: it fades out with them and comes back on the next button
    // press, so the video is unobstructed while it plays.
    const float chrome = in_related_ ? 1.0f : controls_.visibility();
    if (chrome > 0.01f)
    {
        // A scrim over the lower half: white text over bright video is unreadable.
        canvas.list.gradient_rect({0.0f, 470.0f, gfx::kVirtualWidth, 610.0f}, 0.0f,
                                  gfx::Color::rgb(0x000000, 0.0f),
                                  gfx::Color::rgb(0x000000, 0.72f * chrome));

        // Title and the codec/quality label, just above the controls.
        const std::string full = view.info.title.empty() ? playing_id_ : view.info.title;
        const std::string title = context.fonts.semibold.font != nullptr
                                      ? context.fonts.semibold.font->fit(full, 28.0f, 700.0f)
                                      : full;
        ui::text(canvas.list, context.fonts.semibold, title, 480.0f, 826.0f, 28.0f,
                 gfx::Color::rgb(0xf1f1f1, chrome));
        if (player_ != nullptr)
        {
            const std::string label = player_->quality_label();
            if (!label.empty())
            {
                ui::text(canvas.list, context.fonts.mono, label, 1440.0f, 826.0f, 24.0f,
                         gfx::Color::rgb(0xd0d0d0, chrome), gfx::Align::right);
            }
        }
    }
    if (player_ != nullptr && ended_)
        ui::text(canvas.list, context.fonts.semibold, "Ended", gfx::kVirtualWidth * 0.5f, 500.0f,
                 30.0f, gfx::Color::rgb(0xf5a524), gfx::Align::center);

    if (!sponsor_here_.empty())
    {
        // The skip offer: bottom right, above the controls, visible even while
        // they are hidden.
        const std::string text = "Triangle  Skip " + sponsor_here_;
        constexpr float kSize = 26.0f;
        const float width = (context.fonts.semibold.font != nullptr
                                 ? context.fonts.semibold.font->measure(text, kSize)
                                 : 14.0f * static_cast<float>(text.size())) +
                            48.0f;
        const gfx::Rect pill{1680.0f - width, 740.0f, width, 58.0f};
        canvas.list.rounded_rect(pill, 29.0f, gfx::Color::rgb(0x000000, 0.75f));
        ui::text(canvas.list, context.fonts.semibold, text, pill.x + 24.0f, pill.y + 40.0f, kSize,
                 gfx::Color::rgb(0xf1f1f1));
    }

    if (!related_items_.empty() && chrome > 0.5f)
    {
        ui::text(canvas.list, context.fonts.semibold, "Up next", 150.0f, 548.0f, 26.0f,
                 in_related_ ? gfx::Color::rgb(0xf1f1f1) : gfx::Color::rgb(0x9a9a9a, chrome));
        related_.draw(canvas);
    }
    controls_.draw(canvas);
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

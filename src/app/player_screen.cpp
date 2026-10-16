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

#include "yt/codec.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace hui::app
{
namespace
{

const TourStep kTour[] = {
    // Playing: the controls hide on their own, the picture is clear.
    {5.0f, 0, Direction::none, "video", 0, 0, 0, 0, 0},
    // Any button brings them back with the focus on play/pause.
    {0.9f, 0, Direction::up, nullptr, 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::none, "controls", 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::up, nullptr, 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::none, "seek", 0, 0, 0, 0, 0},
    {0.6f, 0, Direction::down, nullptr, 0, 0, 0, 0, 0},
    // Right to the Info pill, and the description.
    {0.4f, 0, Direction::right, nullptr, 0, 0, 0, 0, 0},
    {0.4f, 0, Direction::right, nullptr, 0, 0, 0, 0, 0},
    {0.4f, 0, Direction::right, nullptr, 0, 0, 0, 0, 0},
    {0.9f, hui::action_bit(Action::confirm), Direction::none, nullptr, 0, 0, 0, 0, 0},
    {0.9f, 0, Direction::none, "info", 0, 0, 0, 0, 0},
    {0.9f, hui::action_bit(Action::back), Direction::none, nullptr, 0, 0, 0, 0, 0},
    {0.6f, 0, Direction::left, nullptr, 0, 0, 0, 0, 0},
    {0.6f, 0, Direction::left, nullptr, 0, 0, 0, 0, 0},
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
    related_.style.card.radius = 8.0f;
    related_.style.item_width = 330.0f;
    related_.style.counter = false;
    related_.title = "Suggestions";
    related_.set_bounds({96.0f, 1080.0f, 1728.0f, 290.0f});
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
    if (quality_override_ > 0)
        choice.max_height = quality_override_;
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
        const double saved = context_.resume.position(video_id);
        resume_s_ = saved > 0.0 ? saved : -1.0;
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
        chrome_.set_ranges(std::move(ranges));
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
        skip_segment_now(feedback);
}

void PlayerScreen::skip_segment_now(ui::Feedback &feedback)
{
    if (player_ == nullptr)
        return;
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
            if (resume_s_ > 0.0)
                player_->seek(resume_s_);
            resume_s_ = -1.0;
            if (speed_ != 1.0)
                player_->set_speed(speed_);
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
            card.texture = context_.thumbs.texture(card_thumbnail(item));
            card.image_aspect = 16.0f / 9.0f;
            if (item.live)
                card.badge = "LIVE";
            else
                card.badge = format_duration(item.length_s);
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
    chrome_.set_has_next(!related_items_.empty());
    chrome_.set_info(view.info.title.empty() ? playing_id_ : view.info.title, view.info.author,
                     player_ != nullptr ? player_->quality_label() : std::string());

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
    chrome_.set_account(context_.service.signed_in() && view.state == yt::Load::ready,
                        view.like_status == "LIKE", view.like_status == "DISLIKE", view.subscribed);
    // Say once whether YouTube took the video into the account's history.
    if (!view.history_status.empty() && history_toast_for_ != view.info.video_id)
    {
        history_toast_for_ = view.info.video_id;
        if (view.history_status != "ok")
            context_.toasts.push(ui::StatusKind::warning, "Not added to YouTube history",
                                 view.history_status, 4.0f);
        else if (!history_ok_shown_)
        {
            history_ok_shown_ = true;
            context_.toasts.push(ui::StatusKind::info, "Added to your YouTube history", {}, 2.5f);
        }
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
            if (!related_items_.empty())
            {
                in_related_ = true;
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
            open_menu(command);
            break;
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
        if (input.is_pressed(Action::page_prev) && player_ != nullptr)
        {
            player_->seek(player_->clock_s() - 10.0);
            chrome_.show();
        }
        if (input.is_pressed(Action::page_next) && player_ != nullptr)
        {
            player_->seek(player_->clock_s() + 10.0);
            chrome_.show();
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
        else if (event == ui::Event::cancelled || input.nav == Direction::up)
        {
            in_related_ = false;
            chrome_.set_lifted(false);
            back_used = event == ui::Event::cancelled;
            if (input.nav == Direction::up)
                feedback.play(audio::Cue::focus);
        }
    }
    related_.set_active(in_related_);

    // Autoplay: the next suggestion starts the moment the video ends.
    if (ended_ && !in_related_ && next_suggestion() != nullptr)
    {
        play_next();
        return;
    }

    if (input.is_pressed(Action::back) && !back_used)
        context_.nav.leave_player = true;

    // The row sits under the block and slides up with it.
    const float lift = chrome_.lift();
    related_.set_bounds({96.0f, 1080.0f - 330.0f * lift + 10.0f, 1728.0f, 290.0f});
    chrome_.update(dt);
    related_.update(dt);
}

const yt::FeedItem *PlayerScreen::next_suggestion() const
{
    // The first suggestion that is not this video: the list YouTube answers
    // can start with the one playing.
    const std::string current = playing_id_.empty() ? requested_id_ : playing_id_;
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
    const yt::VideoView view = context_.service.video();
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
        std::vector<std::string> options{
            "Auto (Settings: " + std::to_string(context_.settings.max_height) + "p)"};
        int selected = 0;
        for (std::size_t i = 0; i < menu_heights_.size(); ++i)
        {
            options.push_back(std::to_string(menu_heights_[i]) + "p");
            if (menu_heights_[i] == quality_override_)
                selected = static_cast<int>(i) + 1;
        }
        chrome_.open_panel(PlayerChrome::Panel::quality, "Quality", std::move(options), selected);
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
    const yt::VideoView view = context_.service.video();
    switch (panel)
    {
    case PlayerChrome::Panel::captions:
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
    case PlayerChrome::Panel::info:
    case PlayerChrome::Panel::none:
        break;
    }
}

void PlayerScreen::update_captions()
{
    const yt::VideoView view = context_.service.video();
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
            for (const std::string &part : context.fonts.semibold.font->wrap(line, 38.0f, 1400.0f))
                lines.push_back(part);
        }
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    if (lines.size() > 4)
        lines.erase(lines.begin(), lines.end() - 4);
    // Above the controls while they are up, near the bottom edge otherwise.
    const float bottom = 1030.0f - (330.0f * chrome_.lift() + 290.0f) * chrome_.visibility();
    float y = bottom - static_cast<float>(lines.size()) * 52.0f;
    for (const std::string &line : lines)
    {
        const float width = context.fonts.semibold.measure(line, 38.0f);
        canvas.list.rounded_rect(
            {gfx::kVirtualWidth * 0.5f - width * 0.5f - 16.0f, y, width + 32.0f, 50.0f}, 6.0f,
            gfx::Color::rgb(0x000000, 0.72f));
        ui::text(canvas.list, context.fonts.semibold, line, gfx::kVirtualWidth * 0.5f, y + 38.0f,
                 38.0f, gfx::Color::rgb(0xffffff), gfx::Align::center);
        y += 52.0f;
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

    draw_captions(canvas, context);

    // The controls, and the suggestions row under them when it is open.
    const float lift = chrome_.lift();
    chrome_.draw(canvas, context.fonts, 330.0f * lift);
    if (lift > 0.01f && !related_items_.empty())
    {
        canvas.list.push_opacity(std::min(1.0f, lift * 1.5f));
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

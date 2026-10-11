// youtube-tv - The app implementation.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"

#include "app/screens.hpp"
#include "app/theme.hpp"
#include "app/thumbs.hpp"
#include "core/save_file.hpp"
#include "core/version.hpp"
#include "gfx/gl_batch.hpp"
#include "gfx/renderer.hpp"
#include "ui/components.hpp"
#include "yt/service.hpp"

#include <GL/glcorearb.h>

#include <cstdio>

namespace hui::app
{
namespace
{

constexpr float kIntroSeconds = 1.4f;
constexpr float kOutroSeconds = 0.4f;
constexpr float kSaveDelay = 0.6f;
constexpr float kUpdateGrace = 2.0f; // staged: how long before the app closes
constexpr const char *kSettingsFile = "/settings.bin";
constexpr const char *kParamJson = "/app0/sce_sys/param.json";

void delete_texture(std::uint32_t texture)
{
    const GLuint name = texture;
    glDeleteTextures(1, &name);
}

// "12.4" for a byte count, so the download card reads in MB.
std::string megabytes(long long bytes)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

// The first `count` non-empty lines of a release body, joined with newlines.
std::string first_lines(const std::string &text, int count)
{
    std::string out;
    std::size_t start = 0;
    int taken = 0;
    while (start < text.size() && taken < count)
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::size_t stop = end;
        while (stop > start && (text[stop - 1] == '\r' || text[stop - 1] == ' '))
            --stop;
        if (stop > start)
        {
            if (!out.empty())
                out.push_back('\n');
            out.append(text, start, stop - start);
            ++taken;
        }
        start = end + 1;
    }
    return out;
}

std::string download_body(const yt::update::Snapshot &snapshot)
{
    return "Version " + snapshot.latest_version + " - " + megabytes(snapshot.downloaded) + " / " +
           megabytes(snapshot.asset_size) + " MB";
}

} // namespace

App::App(const ui::Fonts &fonts, Settings settings, std::string data_root, gfx::GlBatch &batch,
         yt::Service &service)
    : fonts_(fonts), data_root_(std::move(data_root)), settings_(settings), service_(service),
      thumbs_(std::make_unique<ThumbCache>(batch, &delete_texture, 72)),
      context_{fonts_, telemetry_, settings_, false, *thumbs_,
               batch,  service_,   toasts_,   nav_,  active_video_}
{
    toasts_.style.theme = youtube_theme();
    toasts_.style.anchor = ui::ToastAnchor::top_right;
    toasts_.style.max_visible = 3;

    // The auto-update card: one at a time, anchored away from the toasts, and
    // answerable with the controller (the stack takes the focus while it shows).
    updates_.style.theme = youtube_theme();
    updates_.style.anchor = ui::ToastAnchor::bottom_right;
    updates_.style.width = 620.0f;
    updates_.style.max_visible = 1;
    updates_.style.body_lines = 4;
    updates_.set_bounds({0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight});
    updates_.on_action = [this](int id, int action) { on_update_action(id, action); };

    // The rail: the four sections on the left, each with its own icon.
    rail_.style.theme = youtube_theme();
    rail_.style.collapsed_width = 104.0f;
    rail_.style.expanded_width = 320.0f;
    rail_.style.row_height = 72.0f;
    rail_.style.panel = false;
    rail_.style.current_plate = true;
    rail_.style.icon_size = 34.0f;
    rail_.set_entries({{"Home", {}, {}, false, false, false, 0},
                       {"Search", {}, {}, false, false, false, 1},
                       {"Subscriptions", {}, {}, false, false, false, 2},
                       {"Settings", {}, {}, false, false, false, 3}});
    rail_.icon = [](ui::Canvas &canvas, const gfx::Rect &box, const ui::NavEntry &, int index,
                    float, gfx::Color ink)
    {
        const gfx::Color page = youtube_theme().page;
        const float centre_x = box.cx();
        const float centre_y = box.cy();
        const float s = box.w * 0.5f;
        switch (index)
        {
        case 0: // home
            canvas.list.triangle({centre_x - s, centre_y - s * 0.92f, s * 1.7f, s * 0.9f}, ink);
            canvas.list.rounded_rect(
                {centre_x - s * 0.62f, centre_y - s * 0.16f, s * 1.24f, s * 0.96f}, 2.0f, ink);
            break;
        case 1: // search
            canvas.list.ring(centre_x - s * 0.16f, centre_y - s * 0.16f, s * 0.56f, s * 0.16f, ink);
            canvas.list.line(centre_x + s * 0.24f, centre_y + s * 0.24f, centre_x + s * 0.84f,
                             centre_y + s * 0.84f, s * 0.17f, ink);
            break;
        case 2: // subscriptions: a stack of lines, like YouTube's list
            for (int row = 0; row < 4; ++row)
            {
                const float y = centre_y - s * 0.62f + static_cast<float>(row) * s * 0.42f;
                const float length = row % 2 == 0 ? s * 1.7f : s * 1.2f;
                canvas.list.rounded_rect({centre_x - s * 0.85f, y - s * 0.09f, length, s * 0.18f},
                                         2.0f, ink);
            }
            canvas.list.circle(centre_x + s * 0.32f, centre_y + s * 0.6f, s * 0.26f, page);
            canvas.list.triangle({centre_x + s * 0.24f, centre_y + s * 0.45f, s * 0.34f, s * 0.3f},
                                 ink, 0.0f, 1.5708f);
            break;
        default: // settings: sliders
            for (int row = 0; row < 3; ++row)
            {
                const float y = centre_y - s * 0.5f + static_cast<float>(row) * s * 0.5f;
                canvas.list.line(centre_x - s * 0.9f, y, centre_x + s * 0.9f, y, s * 0.14f, ink);
                canvas.list.circle(centre_x + (row == 1 ? s * 0.3f : -s * 0.3f), y, s * 0.2f, ink);
            }
            break;
        }
    };
    rail_.set_bounds({0.0f, 108.0f, 320.0f, 820.0f});

    screens_.push_back(make_home_screen(context_));
    screens_.push_back(make_search_screen(context_));
    screens_.push_back(make_subscriptions_screen(context_));
    screens_.push_back(make_settings_screen(context_));
    screens_.push_back(make_login_screen(context_));
    screens_.push_back(make_player_screen(context_));
    screens_[current_]->enter();
}

App::~App() = default;

Settings App::load_settings(const std::string &data_root)
{
    Settings settings;
    std::string data;
    if (save::read_file(data_root + kSettingsFile, &data, 4096))
        decode_settings(data, &settings);
    return settings;
}

void App::start()
{
    if (started_)
        return;
    started_ = true;
    service_.start();
    // A token the user carried over in the install folder wins over nothing:
    // the sandbox copy in /download0 is tried first by the auth object itself.
    service_.import_token_file();
    service_.refresh_token();
    service_.request_home();
    // The version this build carries is what a release tag is compared
    // against; the check runs on its own thread and never blocks the frame.
    yt::update::set_current_version(read_content_version(kParamJson));
    yt::update::check();
}

void App::set_version(std::string version)
{
    version_ = std::move(version);
}

std::size_t App::screen_count() const
{
    return screens_.size();
}

void App::show(std::size_t index)
{
    if (index >= screens_.size() || index == current_)
        return;
    screens_[current_]->leave();
    if (index < 4)
        last_tab_ = index;
    current_ = index;
    screens_[current_]->enter();
}

gfx::Color App::accent() const
{
    return youtube_theme().accent;
}

audio::SoundSet App::sound_set() const
{
    return audio::SoundSet::glass;
}

bool App::take_settings_changed()
{
    const bool changed = settings_changed_;
    if (changed)
    {
        settings_changed_ = false;
        save_delay_ = kSaveDelay;
    }
    return changed;
}

void App::save_settings()
{
    save::write_atomic(data_root_ + kSettingsFile, encode_settings(settings_));
}

// ---- auto-update -----------------------------------------------------------

ui::Notification App::update_note(const yt::update::Snapshot &snapshot) const
{
    ui::Notification note;
    note.seconds = -1.0f; // it stays until the player answers it
    note.source = "Update";
    switch (snapshot.state)
    {
    case yt::update::State::downloading:
        note.kind = ui::StatusKind::info;
        note.title = "Downloading update";
        note.body = download_body(snapshot);
        note.progress = tween::clamp01(snapshot.progress);
        note.actions = {"Cancel"};
        note.closable = false;
        break;
    case yt::update::State::staged:
        note.kind = ui::StatusKind::success;
        note.title = "Update staged";
        note.body = "The app closes now";
        note.progress = -1.0f;
        note.actions = {};
        note.closable = false;
        break;
    case yt::update::State::failed:
    {
        note.kind = ui::StatusKind::danger;
        note.title = "Update failed";
        std::string body = snapshot.error;
        if (snapshot.error.find("no payload loader on port 9021") != std::string::npos)
            body += "\nStart the ELF loader on the console first";
        note.body = std::move(body);
        note.actions = {"Try again", "Later"};
        note.closable = true;
        break;
    }
    case yt::update::State::available:
    default:
    {
        const std::string running =
            snapshot.current_version.empty() ? version_ : snapshot.current_version;
        note.kind = ui::StatusKind::info;
        note.title = "Update available";
        std::string body = "Version " + snapshot.latest_version + " (running " + running + ")";
        const std::string notes = first_lines(snapshot.notes, 2);
        if (!notes.empty())
            body += "\n" + notes;
        note.body = std::move(body);
        note.actions = {"Update now", "Later"};
        note.closable = true;
        break;
    }
    }
    return note;
}

void App::show_update_card(const yt::update::Snapshot &snapshot)
{
    const ui::Notification note = update_note(snapshot);
    if (update_card_ != 0 && updates_.update_notification(update_card_, note, false))
        return;
    update_card_ = updates_.push(note);
}

void App::refresh_update_progress(const yt::update::Snapshot &snapshot)
{
    if (update_card_ == 0)
        return;
    updates_.set_progress(update_card_, tween::clamp01(snapshot.progress));
    updates_.set_body(update_card_, download_body(snapshot));
}

void App::dismiss_update_card()
{
    if (update_card_ == 0)
        return;
    updates_.dismiss(update_card_);
    update_card_ = 0;
}

void App::pump_update_card()
{
    // The unattended tour drives its own scripted input; an update card would
    // take the frame from it, so the update UI stays out of a tour entirely.
    if (touring_)
        return;
    const yt::update::Snapshot snapshot = yt::update::snapshot();
    const bool revised = snapshot.revision != update_revision_;
    update_revision_ = snapshot.revision;

    // The version line mentions a newer release only while one is pending.
    const bool pending = snapshot.state == yt::update::State::available ||
                         snapshot.state == yt::update::State::downloading ||
                         snapshot.state == yt::update::State::staged;
    update_latest_ = pending ? snapshot.latest_version : std::string();

    const bool forced = update_forced_;
    update_forced_ = false;
    // The frame only reacts to a revision change - or, while the asset
    // streams, to the progress it carries.
    if (!revised && !forced && snapshot.state != yt::update::State::downloading)
        return;

    const bool moved = snapshot.state != update_state_;
    update_state_ = snapshot.state;
    if (snapshot.state == yt::update::State::checking)
        update_installing_ = false; // a check is the step in flight now

    switch (snapshot.state)
    {
    case yt::update::State::available:
    case yt::update::State::failed:
        if (update_dismissed_)
        {
            dismiss_update_card();
            return;
        }
        if (revised || forced || moved || update_card_ == 0)
            show_update_card(snapshot);
        break;
    case yt::update::State::downloading:
        if (update_dismissed_)
        {
            dismiss_update_card();
            return;
        }
        if (moved || forced || update_card_ == 0)
            show_update_card(snapshot);
        else
            refresh_update_progress(snapshot);
        break;
    case yt::update::State::staged:
        if (moved || forced || update_card_ == 0)
            show_update_card(snapshot);
        if (update_close_in_ < 0.0f)
            update_close_in_ = kUpdateGrace;
        break;
    default: // idle, checking, up_to_date
        if (moved || forced)
            dismiss_update_card();
        break;
    }
}

bool App::handle_update_card(const InputFrame &input)
{
    if (update_card_ == 0)
        return false;
    // The card is answerable: while it shows it takes the focus.
    updates_.set_active(updates_.focusable());
    const bool held = updates_.active();
    const ui::Event event = updates_.handle(input, feedback_);
    if (event == ui::Event::changed) // the close control answered "Later"
    {
        update_dismissed_ = true;
        update_card_ = 0;
    }
    // An action may have started or stopped the download: reflect it at once.
    if (event == ui::Event::activated || event == ui::Event::changed)
        pump_update_card();
    return held;
}

void App::on_update_action(int id, int action)
{
    (void)id;
    switch (update_state_)
    {
    case yt::update::State::available:
        if (action == 0)
        {
            update_installing_ = true;
            // Nothing to install: refresh the card instead of leaving it gone.
            if (!yt::update::install())
                update_forced_ = true;
        }
        else
        {
            update_dismissed_ = true; // "Later": gone for this session
        }
        break;
    case yt::update::State::downloading:
        if (action == 0)
            yt::update::cancel();
        break;
    case yt::update::State::failed:
        if (action == 0)
        {
            // Retry the step that failed: the download, or the check itself.
            if (update_installing_)
                yt::update::install();
            else
                yt::update::check();
        }
        else
        {
            update_dismissed_ = true;
        }
        break;
    default:
        break;
    }
}

void App::handle_global(const InputFrame &input, float dt)
{
    (void)dt;
    // Options leaves the app.
    if (input.is_pressed(Action::menu))
    {
        nav_.quit = true;
        return;
    }
    // The update card holds the focus: the section keys wait for it to be
    // answered (Options still leaves above).
    if (updates_.active())
        return;
    // L1 / R1 switch tabs on the four tab screens - unless the screen says it
    // uses them itself (a channel page switches its tabs with them); the
    // player seeks with them and the login screen ignores them.
    if (current_ < 4 && !screens_[current_]->consumes_section_keys())
    {
        if (input.is_pressed(Action::page_prev) && current_ > 0)
            show(current_ - 1);
        else if (input.is_pressed(Action::page_next) && current_ + 1 < 4)
            show(current_ + 1);
    }
}

void App::update(const InputFrame &input, float dt)
{
    clock_ += dt;
    feedback_.clear();
    context_.settings_changed = false;
    context_.nav = Nav{};

    // The auto-update card is answered first: while it shows it holds the
    // frame, so the screen below never sees the input it answers with.
    pump_update_card();
    const bool update_held = handle_update_card(input);
    handle_global(input, dt);

    // The rail takes the focus on its own (touchpad, or left past the grid's
    // first column) and gives it back with right or back; while it has it, the
    // screen below sees nothing. Up/down walk the entries and confirm switches
    // section, keeping the rail, so the player can go Home -> Subscriptions ->
    // Settings without L1/R1.
    bool rail_keep_expanded = false;
    if (current_ >= kTabCount)
        rail_focused_ = false;
    else if (!update_held)
    {
        if (input.is_pressed(Action::touch))
            rail_focused_ = !rail_focused_;
        if (rail_focused_)
        {
            if (input.is_pressed(Action::west))
            {
                rail_pinned_ = !rail_pinned_;
                feedback_.play(audio::Cue::toggle);
            }
            const ui::Event event = rail_.handle(input, feedback_);
            if (event == ui::Event::activated)
                show(static_cast<std::size_t>(rail_.current())); // the rail keeps the focus
            else if (event == ui::Event::cancelled || input.nav == Direction::right)
            {
                // Back to the content; the rail stays open one more frame so
                // the hand-off reads as a transition, then collapses.
                rail_focused_ = false;
                rail_keep_expanded = true;
            }
        }
    }

    InputFrame screen_input = input;
    if (rail_focused_ || update_held)
    {
        screen_input = InputFrame{};
        screen_input.connected = input.connected;
    }
    screens_[current_]->update(screen_input, dt, feedback_);
    thumbs_->pump();
    // A video request that neither arrives nor fails would leave the player on
    // "loading" forever; the watchdog ends it and logs why.
    service_.video_watchdog(dt);

    // A grid that ran off its left edge hands the focus to the rail; the screen
    // keeps its own focus until it is returned.
    if (current_ < kTabCount && screens_[current_]->wants_rail_focus())
        rail_focused_ = true;

    rail_.set_focused(rail_focused_);
    rail_.set_expanded(rail_focused_ || rail_pinned_ || rail_keep_expanded);
    rail_.set_current(static_cast<int>(current_ < kTabCount ? current_ : previous_tab()), false);
    rail_.update(dt);

    // Apply what the screen asked for.
    const Nav &ask = context_.nav;
    if (ask.quit)
        quit_ = true;
    if (ask.open_video && !ask.video_id.empty())
    {
        active_video_ = ask.video_id;
        const std::size_t player = 5;
        if (current_ != player)
            show(player); // the player screen reads the id when it enters
    }
    if (ask.leave_player && current_ == 5)
    {
        active_video_.clear();
        service_.request_close_video();
        show(previous_tab());
    }
    if (ask.go_to && ask.screen_index >= 0 && ask.screen_index < 4)
        show(static_cast<std::size_t>(ask.screen_index));
    if (ask.start_login)
    {
        service_.request_sign_in();
        login_return_ = current_ == 4 ? previous_tab() : current_;
        show(4);
    }
    if (ask.cancel_login)
        service_.cancel_sign_in();
    if (ask.sign_out)
    {
        service_.sign_out();
        toasts_.push(ui::StatusKind::info, "Signed out");
    }
    if (context_.settings_changed)
        settings_changed_ = true;

    // Signing in unlocks the personalized feed and lets the app keep a second
    // copy of the token where the user can reach it.
    const bool signed_in = service_.signed_in();
    if (signed_in && !was_signed_in_)
    {
        // The feeds the screens asked for while the account was still missing
        // carry no content: request both again now that the token is loaded.
        service_.request_home(true);
        service_.request_subscriptions(true);
        if (login_return_ < screens_.size() && current_ == 4)
            show(login_return_); // the player picks the video up again by itself
        // The restore message only appears when the install-folder token was
        // really restored and loaded, never on a plain boot or a sign-in
        // where nothing was restored.
        const bool restored = service_.take_token_restored();
        std::string where;
        if (service_.export_token(&where))
        {
            if (restored)
                toasts_.push(ui::StatusKind::success, "Account stored", where);
        }
        else
            toasts_.push(ui::StatusKind::warning, "Account only in the sandbox",
                         "Store a copy: " + where);
    }
    was_signed_in_ = signed_in;

    toasts_.update(dt, feedback_);
    updates_.update(dt, feedback_);

    // The helper has the new version beside the app: after a short grace the
    // app closes itself through the clean exit path so the swap can run.
    if (update_close_in_ >= 0.0f)
    {
        update_close_in_ -= dt;
        if (update_close_in_ <= 0.0f)
            quit_ = true;
    }

    if (save_delay_ > 0.0f)
    {
        save_delay_ -= dt;
        if (save_delay_ <= 0.0f)
            save_settings();
    }
}

void App::draw_chrome(Frame &frame)
{
    ui::Canvas overlay{frame.overlay, fonts_, frame.glass_texture, clock_};
    if (current_ < 4)
    {
        // YouTube's layout: the rail on the left, the section title on top of
        // the content.
        ui::Canvas rail_canvas{frame.overlay, fonts_, frame.glass_texture, clock_};
        rail_.draw(rail_canvas);
        if (!version_.empty())
        {
            // The running version, and the newer release once one is known.
            std::string line = version_;
            if (!update_latest_.empty())
                line += "   latest " + update_latest_;
            ui::text(overlay.list, fonts_.mono, line, 360.0f, 1046.0f, 20.0f,
                     gfx::Color::rgb(0x777777));
        }
        ui::text(overlay.list, fonts_.regular, screens_[current_]->hint(), 1820.0f, 1046.0f, 20.0f,
                 gfx::Color::rgb(0x777777), gfx::Align::right);
    }
    else
    {
        ui::text(overlay.list, fonts_.regular, screens_[current_]->hint(), 1720.0f, 1046.0f, 20.0f,
                 gfx::Color::rgb(0xbbbbbb), gfx::Align::right);
    }
    toasts_.set_bounds({0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight});
    updates_.draw(overlay);
    toasts_.draw(overlay);
}

void App::compose(gfx::Renderer &renderer)
{
    frame_.reset();
    frame_.backdrop.mode = gfx::BackdropMode::none;
    frame_.backdrop.colors[0] = gfx::Color::rgb(0x0f0f0f);
    frame_.backdrop.time = clock_;
    screens_[current_]->draw(context_, frame_);
    draw_chrome(frame_);

    // The hint row is written by the player screen into the chrome.
    renderer.begin();
    renderer.backdrop(frame_.backdrop, 1.0f);
    renderer.draw(frame_.scene);
    if (frame_.glass)
        renderer.glass();
    renderer.draw(frame_.overlay);
}

void App::rehearse()
{
    frame_.reset();
    screens_[current_]->draw(context_, frame_);
    draw_chrome(frame_);
}

void App::begin_tour(const std::string &only)
{
    tour_ = TourState{};
    tour_.only = only;
    tour_.index = 0;
    rail_focused_ = false;
    // A tour owns the frame: keep the update UI out of it.
    touring_ = true;
    updates_.clear(true);
    update_card_ = 0;
    if (!only.empty())
    {
        bool found = false;
        for (std::size_t i = 0; i < screens_.size(); ++i)
        {
            if (only == screens_[i]->id())
            {
                tour_.index = i;
                found = true;
                break;
            }
        }
        tour_.finished = !found;
    }
    else
    {
        tour_.finished = screens_.empty();
    }
}

std::string tour_picture_name(const Screen &screen, std::size_t index, const char *suffix)
{
    char name[96];
    if (suffix == nullptr || suffix[0] == '\0')
        std::snprintf(name, sizeof(name), "%02zu-%s", index + 1, screen.id());
    else
        std::snprintf(name, sizeof(name), "%02zu-%s-%s", index + 1, screen.id(), suffix);
    return name;
}

InputFrame App::tour_step(float dt)
{
    InputFrame input;
    input.connected = true;
    if (tour_.finished)
        return input;
    if (!tour_.started)
    {
        tour_.started = true;
        show(tour_.index);
        tour_.step = 0;
        tour_.clock = 0.0f;
        tour_.intro_done = false;
        tour_.stats = TourState::Stats{};
        return input;
    }
    if (!tour_capture_.empty())
        return input; // the caller has not saved the picture yet

    tour_.clock += dt;
    const std::span<const TourStep> script = screens_[tour_.index]->tour();
    if (!tour_.intro_done)
    {
        if (tour_.clock < kIntroSeconds)
            return input;
        tour_.intro_done = true;
        tour_.clock = 0.0f;
        tour_capture_ = tour_picture_name(*screens_[tour_.index], tour_.index, nullptr);
        return input;
    }
    if (tour_.step < script.size())
    {
        const TourStep &now = script[tour_.step];
        input.stick_x = now.stick_x;
        input.stick_y = now.stick_y;
        input.held = now.hold;
        input.trigger_l = now.trigger_l;
        input.trigger_r = now.trigger_r;
        if (tour_.clock < now.wait)
            return input;
        if (now.capture != nullptr && !tour_.pictured)
        {
            tour_.pictured = true;
            tour_capture_ = tour_picture_name(*screens_[tour_.index], tour_.index, now.capture);
            return input;
        }
        tour_.pictured = false;
        input.pressed = now.press;
        input.held = now.press | now.hold;
        input.nav = now.nav;
        ++tour_.step;
        tour_.clock = 0.0f;
        return input;
    }
    if (!screens_[tour_.index]->tour().empty() && tour_.clock < kOutroSeconds)
        return input;

    char line[200];
    const TourState::Stats &stats = tour_.stats;
    std::snprintf(line, sizeof(line),
                  "tour %02zu %-12s frames=%d avg_ms=%.2f worst_ms=%.2f draws=%zu shapes=%zu",
                  tour_.index + 1, screens_[tour_.index]->id(), stats.frames,
                  stats.frames > 0 ? stats.total_ms / stats.frames : 0.0,
                  static_cast<double>(stats.worst_ms), stats.draw_calls, stats.instances);
    tour_report_.push_back(line);

    ++tour_.index;
    if (tour_.index >= screens_.size() || !tour_.only.empty())
    {
        // A single-screen run stops there: its pictures are the evidence.
        tour_.finished = true;
        return input;
    }
    // A walk may have left the rail focused (home's confirm does): a fresh
    // screen starts with its content focused.
    rail_focused_ = false;
    show(tour_.index);
    tour_.step = 0;
    tour_.clock = 0.0f;
    tour_.intro_done = false;
    tour_.pictured = false;
    tour_.stats = TourState::Stats{};
    return input;
}

void App::tour_record(float frame_ms, std::size_t draw_calls, std::size_t instances)
{
    if (tour_.finished || !tour_.started)
        return;
    TourState::Stats &stats = tour_.stats;
    ++stats.frames;
    stats.total_ms += static_cast<double>(frame_ms);
    if (frame_ms > stats.worst_ms)
        stats.worst_ms = frame_ms;
    if (draw_calls > stats.draw_calls)
        stats.draw_calls = draw_calls;
    if (instances > stats.instances)
        stats.instances = instances;
}

} // namespace hui::app

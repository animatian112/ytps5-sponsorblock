// youtube-tv - The app implementation.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"

#include "app/screens.hpp"
#include "app/theme.hpp"
#include "platform/ps5/system.hpp"
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
      thumbs_(std::make_unique<ThumbCache>(batch, &delete_texture, 192)),
      resume_(data_root_.empty() ? std::string() : data_root_ + "/resume.txt"),
      context_{fonts_,  telemetry_,       settings_,      false, *thumbs_,
               batch,   service_,         toasts_,        nav_,  active_video_,
               resume_, pending_channel_, pending_browse_}
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
    rail_.style.row_height = 66.0f;
    rail_.style.panel = false; // set each frame: only the open rail has its panel
    rail_.style.current_plate = true;
    rail_.style.icon_size = 34.0f;
    rail_.set_entries({{"Search", {}, {}, false, false, false, 0},
                       {"Home", {}, {}, false, false, false, 1},
                       {"Subscriptions", {}, {}, false, false, false, 2},
                       {"History", {}, {}, false, false, false, 3},
                       {"Playlists", {}, {}, false, false, false, 4},
                       {"Music", {}, {}, false, false, false, 5},
                       {"Gaming", {}, {}, false, false, false, 6},
                       {"News", {}, {}, false, false, false, 7},
                       {"Live", {}, {}, false, false, false, 8},
                       {"Sports", {}, {}, false, false, false, 9},
                       {"Settings", {}, {}, false, false, false, 10}});
    rail_.icon = [](ui::Canvas &canvas, const gfx::Rect &box, const ui::NavEntry &, int index,
                    float, gfx::Color ink)
    {
        const gfx::Color page = youtube_theme().page;
        const float centre_x = box.cx();
        const float centre_y = box.cy();
        const float s = box.w * 0.5f;
        switch (index)
        {
        case 1: // home
            canvas.list.triangle({centre_x - s, centre_y - s * 0.92f, s * 1.7f, s * 0.9f}, ink);
            canvas.list.rounded_rect(
                {centre_x - s * 0.62f, centre_y - s * 0.16f, s * 1.24f, s * 0.96f}, 2.0f, ink);
            break;
        case 0: // search
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
        case 3: // history: a clock
            canvas.list.ring(centre_x, centre_y, s * 0.82f, s * 0.15f, ink);
            canvas.list.line(centre_x, centre_y, centre_x, centre_y - s * 0.5f, s * 0.14f, ink);
            canvas.list.line(centre_x, centre_y, centre_x + s * 0.38f, centre_y + s * 0.1f,
                             s * 0.14f, ink);
            break;
        case 4: // playlists: lines and a play mark
            for (int row = 0; row < 3; ++row)
                canvas.list.rounded_rect({centre_x - s * 0.85f,
                                          centre_y - s * 0.6f + static_cast<float>(row) * s * 0.45f,
                                          s * (row == 2 ? 0.8f : 1.3f), s * 0.16f},
                                         2.0f, ink);
            canvas.list.triangle({centre_x + s * 0.2f, centre_y + s * 0.1f, s * 0.6f, s * 0.6f},
                                 ink, 0.0f, 1.5708f);
            break;
        case 5: // music: a note
            canvas.list.circle(centre_x - s * 0.35f, centre_y + s * 0.5f, s * 0.3f, ink);
            canvas.list.line(centre_x - s * 0.08f, centre_y + s * 0.5f, centre_x - s * 0.08f,
                             centre_y - s * 0.75f, s * 0.14f, ink);
            canvas.list.line(centre_x - s * 0.08f, centre_y - s * 0.75f, centre_x + s * 0.55f,
                             centre_y - s * 0.45f, s * 0.14f, ink);
            break;
        case 6: // gaming: a pad
            canvas.list.rounded_rect(
                {centre_x - s * 0.9f, centre_y - s * 0.45f, s * 1.8f, s * 0.9f}, s * 0.4f, ink);
            canvas.list.circle(centre_x - s * 0.42f, centre_y, s * 0.16f, page);
            canvas.list.circle(centre_x + s * 0.42f, centre_y, s * 0.16f, page);
            break;
        case 7: // news: a page with lines
            canvas.list.rounded_rect(
                {centre_x - s * 0.75f, centre_y - s * 0.8f, s * 1.5f, s * 1.6f}, 3.0f, ink);
            for (int row = 0; row < 3; ++row)
                canvas.list.rounded_rect({centre_x - s * 0.5f,
                                          centre_y - s * 0.45f + static_cast<float>(row) * s * 0.4f,
                                          s * 1.0f, s * 0.14f},
                                         1.0f, page);
            break;
        case 8: // live: a dot in two rings
            canvas.list.circle(centre_x, centre_y, s * 0.25f, ink);
            canvas.list.ring(centre_x, centre_y, s * 0.55f, s * 0.12f, ink);
            canvas.list.ring(centre_x, centre_y, s * 0.88f, s * 0.12f, ink);
            break;
        case 9: // sports: a ball
            canvas.list.ring(centre_x, centre_y, s * 0.8f, s * 0.15f, ink);
            canvas.list.line(centre_x - s * 0.8f, centre_y, centre_x + s * 0.8f, centre_y,
                             s * 0.12f, ink);
            canvas.list.line(centre_x, centre_y - s * 0.8f, centre_x, centre_y + s * 0.8f,
                             s * 0.12f, ink);
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
    rail_.set_bounds({0.0f, 128.0f, 320.0f, 880.0f});

    // In the order of the kXxxScreen indices (screens.hpp).
    screens_.push_back(make_search_screen(context_));
    screens_.push_back(make_home_screen(context_));
    screens_.push_back(make_subscriptions_screen(context_));
    screens_.push_back(make_history_screen(context_));
    screens_.push_back(make_playlists_screen(context_));
    screens_.push_back(make_topic_screen(context_, "music", "Music", "UC-9-kyTW8ZkZNDHQJ6FgpwQ"));
    screens_.push_back(make_topic_screen(context_, "gaming", "Gaming", "UCOpNcN46UbXVtpKMrmU4Abg"));
    screens_.push_back(make_topic_screen(context_, "news", "News", "UCYfdidRxbB8Qhf0Nx7ioOYw"));
    screens_.push_back(make_topic_screen(context_, "live", "Live", "UC4R8DWoMoI7CAwX8_LjQHig"));
    screens_.push_back(make_topic_screen(context_, "sports", "Sports", "UCEgdi0XIXXZ-qJOFPf4JSKw"));
    screens_.push_back(make_settings_screen(context_));
    screens_.push_back(make_login_screen(context_));
    screens_.push_back(make_player_screen(context_));
    screens_.push_back(make_browse_screen(context_));
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
    // The SponsorBlock build checks its own release feed
    // (animatian112/ytps5-sponsorblock), never upstream's.
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
    if (index < kTabCount)
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

namespace
{

enum CardPick
{
    kPickChannel = 1,
    kPickWatchLater,
    kPickPlaylist,
    kPickNotInterested,
    kPickDontRecommend,
    kPickRefresh,
    kPickOpen,
    kPickPlaylistBase = 1000, // + the index into menu_playlists_
};
// How long Cross is held before the card menu opens instead of the video.
constexpr float kHoldSeconds = 0.45f;

} // namespace

void App::filter_card_hold(InputFrame &input, float dt)
{
    const std::uint32_t bit = action_bit(Action::confirm);
    const yt::FeedItem *item = screens_[current_]->focused_item();
    if (hold_active_)
    {
        if (input.nav != Direction::none)
        {
            hold_active_ = false; // moved away while holding: neither tap nor menu
            input.pressed &= ~bit;
        }
        else if (!input.is_held(Action::confirm))
        {
            hold_active_ = false;
            input.pressed |= bit; // let go early: the ordinary press
        }
        else
        {
            input.pressed &= ~bit;
            hold_time_ += dt;
            if (hold_time_ >= kHoldSeconds)
            {
                hold_active_ = false;
                feedback_.rumble(0.25f, 0.05f);
                open_card_menu(menu_item_);
            }
        }
        return;
    }
    if (item == nullptr)
        return;
    if (input.is_pressed(Action::confirm) && input.is_held(Action::confirm))
    {
        // Wait for the release (a press) or the hold (the menu).
        hold_active_ = true;
        hold_time_ = 0.0f;
        menu_item_ = *item;
        input.pressed &= ~bit;
    }
    else if (input.is_pressed(Action::west))
    {
        menu_item_ = *item;
        open_card_menu(menu_item_);
        input.pressed &= ~action_bit(Action::west);
    }
}

void App::open_card_menu(const yt::FeedItem &item)
{
    menu_item_ = item;
    std::vector<CardMenu::Entry> entries;
    const bool signed_in = service_.signed_in();
    if (item.is_playlist())
        entries.push_back({"Open playlist", kPickOpen});
    else
        entries.push_back({"Play video", kPickOpen});
    if (!item.channel_id.empty())
        entries.push_back({"Open channel", kPickChannel});
    if (signed_in && !item.is_playlist())
    {
        entries.push_back({"Add to Watch later", kPickWatchLater});
        entries.push_back({"Add to playlist", kPickPlaylist});
    }
    if (signed_in && !item.not_interested_token.empty())
        entries.push_back({"Not interested", kPickNotInterested});
    if (signed_in && !item.dont_recommend_token.empty())
        entries.push_back({"Don't recommend channel", kPickDontRecommend});
    if (current_ == kHomeScreen)
        entries.push_back({"Refresh Home", kPickRefresh});
    card_menu_.open(item.title, std::move(entries));
}

void App::open_playlist_menu()
{
    menu_playlists_.clear();
    std::vector<CardMenu::Entry> entries;
    entries.push_back({"Watch later", kPickWatchLater});
    for (const yt::FeedItem &list : service_.feed(yt::FeedKind::library).items)
    {
        // Watch later has its own entry; Liked videos is filled by the like button.
        if (!list.is_playlist() || list.video_id == "VLWL" || list.video_id == "VLLL")
            continue;
        entries.push_back(
            {list.title, kPickPlaylistBase + static_cast<int>(menu_playlists_.size())});
        menu_playlists_.push_back(list);
        if (menu_playlists_.size() >= 50)
            break;
    }
    card_menu_.open("Add to playlist", std::move(entries));
}

void App::refresh_home()
{
    service_.request_home(true);
    if (service_.signed_in())
    {
        service_.request_history();
        service_.request_subscriptions(true);
    }
    toasts_.push(ui::StatusKind::info, "Refreshing Home", {}, 1.5f);
}

void App::apply_card_pick(int pick)
{
    const yt::FeedItem &item = menu_item_;
    if (pick >= kPickPlaylistBase)
    {
        const std::size_t index = static_cast<std::size_t>(pick - kPickPlaylistBase);
        if (index < menu_playlists_.size())
            service_.add_to_playlist(menu_playlists_[index].video_id.substr(2), item.video_id,
                                     "Added to " + menu_playlists_[index].title);
        return;
    }
    switch (pick)
    {
    case kPickOpen:
        context_.nav.open_video = true;
        context_.nav.video_id = item.video_id;
        break;
    case kPickChannel:
        pending_channel_ = {item.channel_id, item.author, {}, {}};
        context_.nav.open_channel = true;
        break;
    case kPickWatchLater:
        service_.add_to_playlist("WL", item.video_id, "Added to Watch later");
        break;
    case kPickPlaylist:
    {
        const yt::FeedView library = service_.feed(yt::FeedKind::library);
        if (library.state == yt::Load::ready && !library.items.empty())
            open_playlist_menu();
        else
        {
            waiting_playlists_ = true;
            if (library.state != yt::Load::loading)
                service_.request_library();
        }
        break;
    }
    case kPickNotInterested:
        service_.send_feedback(item.not_interested_token,
                               "Got it. You'll see fewer videos like this");
        service_.hide_item(item.video_id);
        break;
    case kPickDontRecommend:
        service_.send_feedback(item.dont_recommend_token,
                               "Got it. This channel won't be recommended");
        service_.hide_item(item.video_id);
        break;
    case kPickRefresh:
        refresh_home();
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
    if (current_ < kTabCount && !screens_[current_]->consumes_section_keys())
    {
        if (input.is_pressed(Action::page_prev) && current_ > 0)
            show(current_ - 1);
        else if (input.is_pressed(Action::page_next) && current_ + 1 < kTabCount)
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
    if (rail_focused_ || update_held || card_menu_.active())
    {
        if (card_menu_.active() && !update_held)
        {
            const int pick = card_menu_.handle(input, feedback_);
            if (pick >= 0)
                apply_card_pick(pick);
        }
        hold_active_ = false;
        screen_input = InputFrame{};
        screen_input.connected = input.connected;
    }
    else
        filter_card_hold(screen_input, dt);
    card_menu_.update(dt);
    // "Save to playlist" before the account's playlists had loaded: the list
    // opens once they are in.
    if (waiting_playlists_)
    {
        const yt::FeedView library = service_.feed(yt::FeedKind::library);
        if (library.state == yt::Load::ready || library.state == yt::Load::failed)
        {
            waiting_playlists_ = false;
            open_playlist_menu();
        }
    }
    // A card action came back from YouTube.
    if (service_.action_revision() != seen_action_revision_)
    {
        seen_action_revision_ = service_.action_revision();
        const std::pair<bool, std::string> result = service_.action_result();
        toasts_.push(result.first ? ui::StatusKind::success : ui::StatusKind::warning,
                     result.second, {}, 2.5f);
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
    // Collapsed, the rail is bare icons; its panel comes with the labels.
    rail_.style.panel = rail_.expansion() > 0.05f;

    // Apply what the screen asked for.
    const Nav &ask = context_.nav;
    if (ask.add_to_playlist && !ask.playlist_item.video_id.empty())
    {
        menu_item_ = ask.playlist_item;
        apply_card_pick(kPickPlaylist);
    }
    if (ask.quit)
        quit_ = true;
    if (ask.open_video && ask.video_id.size() > 2 && ask.video_id.rfind("VL", 0) == 0)
    {
        // A playlist card: its videos on the browse screen.
        pending_browse_ = ask.video_id;
        show(kBrowseScreen);
    }
    else if (ask.open_video && !ask.video_id.empty())
    {
        active_video_ = ask.video_id;
        const std::size_t player = kPlayerScreen;
        if (current_ != player)
        {
            // Circle in the player comes back here: a playlist page too.
            player_return_ = current_ == kBrowseScreen ? kBrowseScreen : previous_tab();
            show(player); // the player screen reads the id when it enters
        }
    }
    if (ask.open_channel && !pending_channel_.channel_id.empty())
    {
        // The player's Channel button: the search screen shows the page.
        active_video_.clear();
        service_.request_close_video();
        show(kSearchScreen);
    }
    if (ask.leave_browse && current_ == kBrowseScreen)
        show(previous_tab());
    if (ask.leave_player && current_ == kPlayerScreen)
    {
        active_video_.clear();
        service_.request_close_video();
        show(player_return_ == kBrowseScreen ? kBrowseScreen : previous_tab());
    }
    if (ask.go_to && ask.screen_index >= 0 &&
        static_cast<std::size_t>(ask.screen_index) < kTabCount)
        show(static_cast<std::size_t>(ask.screen_index));
    if (ask.start_login)
    {
        service_.request_sign_in();
        login_return_ = current_ == kLoginScreen ? previous_tab() : current_;
        show(kLoginScreen);
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
        if (login_return_ < screens_.size() && current_ == kLoginScreen)
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

void App::draw_sidebar(ui::Canvas &canvas)
{
    gfx::DrawList &list = canvas.list;
    const float open = rail_.expansion();
    const auto mix = [open](float closed, float opened)
    { return closed + (opened - closed) * open; };
    const gfx::Color grey = gfx::Color::rgb(0x9e9e9e);
    const gfx::Color white = gfx::Color::rgb(0xffffff);
    if (open > 0.01f)
        list.rounded_rect({0.0f, 0.0f, mix(104.0f, 540.0f), gfx::kVirtualHeight}, 0.0f,
                          gfx::Color::rgb(0x1e1e1e, std::min(1.0f, open * 1.5f)));

    // The search orb (entry 0) and the account's picture.
    const int focus = rail_focused_ ? rail_.focus() : -1;
    const float orb_y = mix(92.0f, 112.0f);
    const float orb_r = mix(30.0f, 50.0f);
    const float search_x = mix(52.0f, 160.0f);
    list.circle(search_x, orb_y, orb_r, gfx::Color::rgb(0xe53935));
    if (focus == 0)
        list.ring(search_x, orb_y, orb_r + 7.0f, 4.0f, white);
    if (rail_.icon)
    {
        const float icon = orb_r * 1.05f;
        rail_.icon(canvas, {search_x - icon * 0.5f, orb_y - icon * 0.5f, icon, icon},
                   rail_.entries()[0], 0, 0.0f, white);
    }
    const std::pair<std::string, std::string> account = service_.account();
    const float avatar_x = search_x + orb_r * 2.0f + mix(14.0f, 26.0f);
    if (!account.second.empty())
    {
        const std::uint32_t avatar = thumbs_->texture(account.second);
        const gfx::Rect at{avatar_x - orb_r, orb_y - orb_r, orb_r * 2.0f, orb_r * 2.0f};
        if (avatar != 0)
            list.image(avatar, at, gfx::kFullUv, white, orb_r);
        else
            list.circle(at.cx(), at.cy(), orb_r, gfx::Color::rgb(0x3a3a3a));
    }

    // The sections: icon and name, the current one white and larger, the
    // others grey; the one the focus is on while the panel is open is white.
    const auto &entries = rail_.entries();
    const int current = rail_.current();
    const float top = mix(210.0f, 250.0f);
    const float pitch = mix(72.0f, 76.0f);
    for (std::size_t i = 1; i < entries.size(); ++i)
    {
        const int index = static_cast<int>(i);
        const float y = top + static_cast<float>(i - 1) * pitch;
        const bool lit = index == focus || (focus < 0 && index == current);
        const bool chosen = index == current;
        const gfx::Color ink = lit || (open < 0.5f && chosen) ? white : grey;
        const float icon = chosen && open > 0.5f ? 44.0f : 38.0f;
        const float icon_x = mix(52.0f, 150.0f);
        if (rail_.icon)
            rail_.icon(canvas, {icon_x - icon * 0.5f, y - icon * 0.5f, icon, icon}, entries[i],
                       index, lit ? 1.0f : 0.0f, ink);
        if (open > 0.05f)
        {
            list.push_opacity(open);
            const float size = lit ? 40.0f : 34.0f;
            ui::text(list, fonts_.regular, entries[i].label, icon_x + 54.0f, y + size * 0.36f, size,
                     ink);
            list.pop_opacity();
        }
    }
}

void App::draw_chrome(Frame &frame)
{
    ui::Canvas overlay{frame.overlay, fonts_, frame.glass_texture, clock_};
    if (current_ < kTabCount)
    {
        // SmartTube's layout: the sections on the left (a grey panel with
        // the search orb and the account on top while it is open, bare icons
        // while the content has the focus), the clock and the badge top right.
        draw_sidebar(overlay);

        int hour = 0;
        int minute = 0;
        bool twelve = true;
        constexpr float kClockX = 1600.0f;
        if (sys::local_clock(&hour, &minute, &twelve))
        {
            char text[16];
            if (twelve)
                std::snprintf(text, sizeof(text), "%d:%02d %s", hour % 12 == 0 ? 12 : hour % 12,
                              minute, hour < 12 ? "AM" : "PM");
            else
                std::snprintf(text, sizeof(text), "%02d:%02d", hour, minute);
            ui::text(overlay.list, fonts_.semibold, text, kClockX, 98.0f, 50.0f,
                     gfx::Color::rgb(0xffffff), gfx::Align::center);
        }
        int weekday = 0;
        int month = 0;
        int day = 0;
        if (sys::local_date(&weekday, &month, &day) && month >= 1 && month <= 12)
        {
            static const char *const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
            static const char *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            char text[32];
            std::snprintf(text, sizeof(text), "%s %s %d", kDays[weekday % 7], kMonths[month - 1],
                          day);
            ui::text(overlay.list, fonts_.semibold, text, kClockX, 136.0f, 24.0f,
                     gfx::Color::rgb(0xffffff), gfx::Align::center);
        }
        // The app's badge, where SmartTube shows its "S".
        const gfx::Rect badge{1756.0f, 56.0f, 92.0f, 66.0f};
        overlay.list.rounded_rect(badge, 10.0f, gfx::Color::rgb(0xe62117));
        ui::text(overlay.list, fonts_.semibold, "T", badge.cx(), badge.cy() + 17.0f, 48.0f,
                 gfx::Color::rgb(0xffffff), gfx::Align::center);
        if (!version_.empty())
        {
            // The running version, and the newer release once one is known.
            std::string line = version_;
            if (!update_latest_.empty())
                line += "   latest " + update_latest_;
            ui::text(overlay.list, fonts_.mono, line, kContentX, 1046.0f, 20.0f,
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
    card_menu_.draw(overlay, fonts_);
    toasts_.draw(overlay);
}

void App::draw_screen()
{
    // Leanback: the open sidebar pushes the page to the right instead of
    // covering it.
    const float shift = current_ < kTabCount ? rail_.expansion() * (540.0f - 104.0f) : 0.0f;
    if (shift > 0.5f)
        frame_.scene.push_transform(1.0f, 0.0f, 0.0f, shift, 0.0f);
    screens_[current_]->draw(context_, frame_);
    if (shift > 0.5f)
        frame_.scene.pop_transform();
}

void App::compose(gfx::Renderer &renderer)
{
    frame_.reset();
    frame_.backdrop.mode = gfx::BackdropMode::none;
    frame_.backdrop.colors[0] = gfx::Color::rgb(0x0f0f0f);
    frame_.backdrop.time = clock_;
    draw_screen();
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
    draw_screen();
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

// youtube-tv - The app implementation.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"

#include "app/icons.hpp"

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

#include <algorithm>
#include <cstdio>

namespace hui::app
{
namespace
{
// Rail entries that are not screens (NavEntry::tag): My videos, Blocked
// Channels, and the pinned channels from kRailPinBase down.
constexpr int kRailMyVideos = -1;
constexpr int kRailBlocked = -2;
constexpr int kRailPinBase = -100;
} // namespace

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
      context_{
          fonts_,          telemetry_,  settings_,      false,         *thumbs_, batch,
          service_,        toasts_,     nav_,           active_video_, resume_,  pending_channel_,
          pending_browse_, play_queue_, section_items_, sb_excluded_,  false,    Remote{},
          nullptr}
{
    if (!data_root_.empty())
    {
        lounge_ = std::make_unique<yt::Lounge>(data_root_, "SmartTube on PS5");
        context_.lounge = lounge_.get();
    }
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
    load_pins();
    rebuild_rail();
    // The name prompt's keyboard (Add to new playlist).
    prompt_keys_.style.theme = youtube_theme();
    prompt_keys_.style.bindings = ui::KeyboardBindings::standard();
    prompt_keys_.set_layouts({ui::KeyboardLayout::letters(), ui::KeyboardLayout::numeric()});
    prompt_keys_.set_bounds({560.0f, 470.0f, 1080.0f, 520.0f});
    prompt_keys_.on_text = [this](std::string_view text)
    {
        if (prompt_text_.size() < 120)
            prompt_text_ += text;
        prompt_keys_.set_length(static_cast<int>(prompt_text_.size()));
    };
    prompt_keys_.on_backspace = [this]
    {
        // One whole UTF-8 character.
        while (!prompt_text_.empty())
        {
            const unsigned char last = static_cast<unsigned char>(prompt_text_.back());
            prompt_text_.pop_back();
            if ((last & 0xC0) != 0x80)
                break;
        }
        prompt_keys_.set_length(static_cast<int>(prompt_text_.size()));
    };
    prompt_keys_.on_done = [this]
    {
        prompt_active_ = false;
        if (prompt_text_.empty())
            return;
        service_.create_playlist(prompt_text_, prompt_video_, "Created " + prompt_text_);
    };
    rail_.icon = [this](ui::Canvas &canvas, const gfx::Rect &box, const ui::NavEntry &entry, int,
                        float, gfx::Color ink)
    {
        // Entries carry their target in `tag`: a screen index, or a negative
        // action (My videos, Blocked Channels, a pinned channel).
        const gfx::Color page = palette(settings_.color_scheme).sidebar;
        const float centre_x = box.cx();
        const float centre_y = box.cy();
        const float s = box.w * 0.5f;
        const int tag = entry.tag;
        if (tag <= kRailPinBase)
        {
            // A pinned channel: its initial in a circle.
            canvas.list.circle(centre_x, centre_y, s * 0.95f, ink);
            if (canvas.fonts.semibold.font != nullptr && !entry.label.empty())
                ui::text(canvas.list, canvas.fonts.semibold, entry.label.substr(0, 1), centre_x,
                         centre_y + s * 0.4f, s * 1.1f, page, gfx::Align::center);
            return;
        }
        // SmartTube's own icons (assets/icons, MIT, yuliskov).
        const char *name = "settings";
        switch (tag)
        {
        case static_cast<int>(kSearchScreen):
            name = "search";
            break;
        case static_cast<int>(kHomeScreen):
            name = "home";
            break;
        case static_cast<int>(kShortsScreen):
            name = "shorts";
            break;
        case static_cast<int>(kSportsScreen):
            name = "sports";
            break;
        case static_cast<int>(kLiveScreen):
            name = "live";
            break;
        case static_cast<int>(kGamingScreen):
            name = "gaming";
            break;
        case static_cast<int>(kNewsScreen):
            name = "news";
            break;
        case static_cast<int>(kMusicScreen):
            name = "music";
            break;
        case static_cast<int>(kChannelsScreen):
            name = "channels";
            break;
        case kRailMyVideos:
            name = "channel";
            break;
        case kRailBlocked:
            name = "blocked";
            break;
        case static_cast<int>(kSubscriptionsScreen):
            name = "subscriptions";
            break;
        case static_cast<int>(kHistoryScreen):
            name = "history";
            break;
        case static_cast<int>(kPlaylistsScreen):
            name = "playlists";
            break;
        default:
            break;
        }
        draw_icon(canvas.list, name, box, ink);
    };
    rail_.set_bounds({0.0f, 128.0f, 320.0f, 880.0f});

    // In the order of the kXxxScreen indices (screens.hpp).
    screens_.push_back(make_search_screen(context_));
    screens_.push_back(make_home_screen(context_));
    screens_.push_back(make_topic_screen(context_, "shorts", "Shorts", "FEshorts"));
    screens_.push_back(make_topic_screen(context_, "sports", "Sports", "UCEgdi0XIXXZ-qJOFPf4JSKw"));
    screens_.push_back(make_topic_screen(context_, "live", "Live", "UC4R8DWoMoI7CAwX8_LjQHig"));
    screens_.push_back(make_topic_screen(context_, "gaming", "Gaming", "UCOpNcN46UbXVtpKMrmU4Abg"));
    screens_.push_back(make_topic_screen(context_, "news", "News", "UCYfdidRxbB8Qhf0Nx7ioOYw"));
    screens_.push_back(make_topic_screen(context_, "music", "Music", "UC-9-kyTW8ZkZNDHQJ6FgpwQ"));
    screens_.push_back(make_topic_screen(context_, "channels", "Channels", "FEchannels"));
    screens_.push_back(make_subscriptions_screen(context_));
    screens_.push_back(make_history_screen(context_));
    screens_.push_back(make_playlists_screen(context_));
    screens_.push_back(make_settings_screen(context_));
    screens_.push_back(make_login_screen(context_));
    screens_.push_back(make_player_screen(context_));
    screens_.push_back(make_browse_screen(context_));
    screens_.push_back(make_channel_screen(context_));
    load_sb_excluded();
    // Settings > General > Boot to section.
    static const std::size_t kBoot[Settings::kBootSectionCount] = {
        kHomeScreen, kSubscriptionsScreen, kShortsScreen, kHistoryScreen, kPlaylistsScreen};
    if (settings_.boot_section > 0 && settings_.boot_section < Settings::kBootSectionCount)
    {
        current_ = kBoot[settings_.boot_section];
        last_tab_ = current_;
    }
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
    kPickAddAccount,
    kPickSignOut,
    kPickRemoveHistory,
    kPickPlayNext,
    kPickSubscribe,
    kPickBlock,
    kPickPin,
    kPickUnpin,
    kPickClearHistory,
    kPickClearConfirm,
    kPickCancel,
    kPickNewPlaylist,
    kPickShuffle,
    kPickSbExclude,
    kPickUnblockBase = 3000,  // + the index of a blocked channel
    kPickPlaylistBase = 1000, // + the index into menu_playlists_
    kPickAccountBase = 2000,  // + the index into the saved accounts
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
    // SmartTube's video menu, in its order.
    menu_item_ = item;
    std::vector<CardMenu::Entry> entries;
    const bool signed_in = service_.signed_in();
    const bool video = !item.is_playlist() && !item.is_channel();
    if (signed_in && !item.remove_history_token.empty())
        entries.push_back({"Remove from history", kPickRemoveHistory});
    if (signed_in && video)
    {
        entries.push_back({"Add/Remove from playlist", kPickPlaylist});
        entries.push_back({"Add to new playlist", kPickNewPlaylist});
    }
    // Not interested: this video goes (the channel stays); YouTube is told
    // when the card carried its token.
    if (video || !item.not_interested_token.empty())
        entries.push_back({"Not interested", kPickNotInterested});
    if (signed_in && !item.dont_recommend_token.empty())
        entries.push_back({"Don't recommend channel", kPickDontRecommend});
    if (video)
        entries.push_back({"Play next", kPickPlayNext});
    if (video)
        entries.push_back({"Shuffle play", kPickShuffle});
    if (!item.channel_id.empty() && settings_.sb_enabled)
    {
        const bool excluded = std::find(sb_excluded_.begin(), sb_excluded_.end(),
                                        item.channel_id) != sb_excluded_.end();
        entries.push_back({excluded ? "Include this channel in SponsorBlock"
                                    : "Exclude this channel from SponsorBlock",
                           kPickSbExclude});
    }
    entries.push_back({"Open channel", kPickChannel});
    if (signed_in)
        entries.push_back({"Subscribe to the channel", kPickSubscribe});
    if (!item.author.empty() || item.is_channel() || !item.channel_id.empty())
        entries.push_back({"Block the channel", kPickBlock});
    const std::string channel = item.is_channel() ? item.video_id : item.channel_id;
    if (pinned(channel))
        entries.push_back({"Remove channel from the sidebar", kPickUnpin});
    else
        entries.push_back({"Add channel to the sidebar", kPickPin});
    if (current_ == kHistoryScreen &&
        !service_.feed(yt::FeedKind::history).clear_history_token.empty())
        entries.push_back({"Clear history", kPickClearHistory});
    if (item.is_playlist())
        entries.push_back({"Open playlist", kPickOpen});
    if (current_ == kHomeScreen)
        entries.push_back({"Refresh Home", kPickRefresh});
    if (entries.empty())
        entries.push_back({"Play video", kPickOpen});
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

void App::open_account_menu()
{
    // SmartTube's account orb: the remembered accounts to switch between.
    const std::vector<yt::Service::AccountInfo> accounts = service_.saved_accounts();
    std::vector<CardMenu::Entry> entries;
    for (std::size_t i = 0; i < accounts.size(); ++i)
    {
        const std::string name =
            accounts[i].name.empty() ? "Account " + std::to_string(i + 1) : accounts[i].name;
        entries.push_back({accounts[i].active ? name + "  (current)" : name,
                           kPickAccountBase + static_cast<int>(i)});
    }
    if (accounts.size() < 3)
        entries.push_back({service_.signed_in() ? "Add account" : "Sign in", kPickAddAccount});
    if (service_.signed_in())
        entries.push_back({"Sign out", kPickSignOut});
    card_menu_.open("Accounts", std::move(entries));
}

std::vector<yt::FeedItem> App::current_section(const std::string &video_id) const
{
    // The feed of the screen the card is on; a screen with two (search and
    // a channel page) answers with the one that holds the video.
    std::vector<yt::FeedKind> kinds;
    switch (current_)
    {
    case kHomeScreen:
        kinds = {yt::FeedKind::home};
        break;
    case kSubscriptionsScreen:
        kinds = {yt::FeedKind::subscriptions};
        break;
    case kHistoryScreen:
        kinds = {yt::FeedKind::history};
        break;
    case kSearchScreen:
        kinds = {yt::FeedKind::channel, yt::FeedKind::search};
        break;
    case kPlayerScreen:
        kinds = {yt::FeedKind::related};
        break;
    default:
        kinds = {yt::FeedKind::topic, yt::FeedKind::home};
        break;
    }
    for (const yt::FeedKind kind : kinds)
    {
        std::vector<yt::FeedItem> items = service_.feed(kind).items;
        for (const yt::FeedItem &item : items)
            if (item.video_id == video_id)
                return items;
    }
    return {};
}

yt::Service::Preferences App::preferences() const
{
    yt::Service::Preferences p;
    p.history = settings_.history_enabled;
    p.shorts_home = settings_.hide_shorts_home;
    p.shorts_subscriptions = settings_.hide_shorts_subscriptions;
    p.shorts_history = settings_.hide_shorts_history;
    p.shorts_search = settings_.hide_shorts_search;
    p.watched_home = settings_.hide_watched_home;
    p.watched_subscriptions = settings_.hide_watched_subscriptions;
    p.streams_subscriptions = settings_.hide_streams_subscriptions;
    p.mixes = settings_.hide_mixes;
    const int region = std::clamp(settings_.region, 0, Settings::kRegionCount - 1);
    p.gl = Settings::kRegions[region][0];
    p.hl = Settings::kRegions[region][1];
    return p;
}

void App::rebuild_rail()
{
    // SmartTube's sidebar order; Search is the orb above the list.
    struct Item
    {
        const char *label;
        int tag;
        int section; // Settings::kSectionNames index (-1: always shown)
    };
    static const Item kItems[] = {
        {"Search", static_cast<int>(kSearchScreen), -1},
        {"Home", static_cast<int>(kHomeScreen), -1},
        {"Shorts", static_cast<int>(kShortsScreen), 0},
        {"Sports", static_cast<int>(kSportsScreen), 1},
        {"LIVE", static_cast<int>(kLiveScreen), 2},
        {"Gaming", static_cast<int>(kGamingScreen), 3},
        {"News", static_cast<int>(kNewsScreen), 4},
        {"Music", static_cast<int>(kMusicScreen), 5},
        {"Channels", static_cast<int>(kChannelsScreen), 6},
        {"Subscriptions", static_cast<int>(kSubscriptionsScreen), 7},
        {"History", static_cast<int>(kHistoryScreen), 8},
        {"Playlists", static_cast<int>(kPlaylistsScreen), 9},
        {"My videos", kRailMyVideos, 10},
        {"Blocked Channels", kRailBlocked, 11},
    };
    // Settings > User interface > Sidebar sections hides any of them.
    rail_hidden_ = settings_.hidden_sections;
    std::vector<ui::NavEntry> entries;
    for (const Item &item : kItems)
        if (!settings_.section_hidden(item.section))
            entries.push_back({item.label, {}, {}, false, item.tag < 0, false, item.tag});
    for (std::size_t i = 0; i < pins_.size(); ++i)
        entries.push_back({pins_[i].name.empty() ? pins_[i].channel_id : pins_[i].name,
                           {},
                           {},
                           false,
                           true,
                           false,
                           kRailPinBase - static_cast<int>(i)});
    entries.push_back({"Settings", {}, {}, false, false, false, static_cast<int>(kSettingsScreen)});
    rail_.set_entries(std::move(entries));
}

int App::rail_position(std::size_t screen) const
{
    const auto &entries = rail_.entries();
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        if (entries[i].tag == static_cast<int>(screen))
            return static_cast<int>(i);
    }
    return 0;
}

void App::activate_rail(int position)
{
    const auto &entries = rail_.entries();
    if (position < 0 || position >= static_cast<int>(entries.size()))
        return;
    const int tag = entries[static_cast<std::size_t>(position)].tag;
    if (tag >= 0)
    {
        show(static_cast<std::size_t>(tag)); // the rail keeps the focus
        return;
    }
    if (tag == kRailMyVideos)
    {
        const std::string mine = service_.account_channel();
        if (mine.empty())
            toasts_.push(ui::StatusKind::warning, "Your channel isn't known yet",
                         service_.signed_in() ? "Try again in a moment" : "Sign in first", 2.5f);
        else if (mine.size() != 24 || mine.rfind("UC", 0) != 0)
        {
            pending_channel_ = {mine, service_.account().first, {}, {}};
            context_.nav.open_channel = true;
            rail_focused_ = false;
        }
        else
        {
            // The uploads playlist (UU + the id after "UC"): every video in
            // upload order, newest first, without the channel page's
            // featured shelf on top.
            pending_browse_ = "VLUU" + mine.substr(2);
            rail_focused_ = false;
            show(kBrowseScreen);
        }
        return;
    }
    if (tag == kRailBlocked)
    {
        open_blocked_menu();
        return;
    }
    const std::size_t pin = static_cast<std::size_t>(kRailPinBase - tag);
    if (pin < pins_.size())
    {
        pending_channel_ = pins_[pin];
        context_.nav.open_channel = true;
        rail_focused_ = false;
    }
}

void App::load_sb_excluded()
{
    sb_excluded_.clear();
    std::string data;
    if (data_root_.empty() || !save::read_file(data_root_ + "/sb_excluded.txt", &data, 64u << 10))
        return;
    std::size_t at = 0;
    while (at < data.size())
    {
        std::size_t end = data.find('\n', at);
        if (end == std::string::npos)
            end = data.size();
        const std::string id = data.substr(at, end - at);
        if (id.size() == 24)
            sb_excluded_.push_back(id);
        at = end + 1;
    }
}

void App::save_sb_excluded() const
{
    if (data_root_.empty())
        return;
    std::string data;
    for (const std::string &id : sb_excluded_)
        data += id + '\n';
    (void)save::write_atomic(data_root_ + "/sb_excluded.txt", data);
}

void App::load_pins()
{
    pins_.clear();
    std::string data;
    if (data_root_.empty() || !save::read_file(data_root_ + "/pins.txt", &data, 16u << 10))
        return;
    std::size_t at = 0;
    while (at < data.size() && pins_.size() < kMaxPins)
    {
        std::size_t end = data.find('\n', at);
        if (end == std::string::npos)
            end = data.size();
        const std::string line = data.substr(at, end - at);
        at = end + 1;
        const std::size_t tab = line.find('\t');
        const std::string id = line.substr(0, tab);
        if (id.size() == 24)
            pins_.push_back(
                {id, tab == std::string::npos ? std::string() : line.substr(tab + 1), {}, {}});
    }
}

void App::save_pins() const
{
    if (data_root_.empty())
        return;
    std::string data;
    for (const yt::ChannelItem &pin : pins_)
    {
        std::string name = pin.name;
        for (char &c : name)
            if (c == '\t' || c == '\n')
                c = ' ';
        data += pin.channel_id + '\t' + name + '\n';
    }
    (void)save::write_atomic(data_root_ + "/pins.txt", data);
}

void App::pin_channel(const yt::ChannelItem &channel)
{
    for (const yt::ChannelItem &pin : pins_)
    {
        if (pin.channel_id == channel.channel_id)
        {
            toasts_.push(ui::StatusKind::info, "Already in the sidebar", channel.name, 2.0f);
            return;
        }
    }
    if (pins_.size() >= kMaxPins)
    {
        toasts_.push(ui::StatusKind::warning, "The sidebar is full", "Remove a channel first",
                     2.5f);
        return;
    }
    pins_.push_back(channel);
    save_pins();
    rebuild_rail();
    toasts_.push(ui::StatusKind::success, "Added to the sidebar", channel.name, 2.0f);
}

void App::unpin_channel(const std::string &channel_id)
{
    pins_.erase(std::remove_if(pins_.begin(), pins_.end(), [&](const yt::ChannelItem &pin)
                               { return pin.channel_id == channel_id; }),
                pins_.end());
    save_pins();
    rebuild_rail();
    toasts_.push(ui::StatusKind::info, "Removed from the sidebar", {}, 2.0f);
}

bool App::pinned(const std::string &channel_id) const
{
    for (const yt::ChannelItem &pin : pins_)
        if (!channel_id.empty() && pin.channel_id == channel_id)
            return true;
    return false;
}

void App::open_blocked_menu()
{
    const std::vector<yt::ChannelItem> blocked = service_.blocked_channels();
    if (blocked.empty())
    {
        toasts_.push(ui::StatusKind::info, "No blocked channels",
                     "Hold Cross on a video: Block the channel", 3.0f);
        return;
    }
    std::vector<CardMenu::Entry> entries;
    for (std::size_t i = 0; i < blocked.size(); ++i)
        entries.push_back(
            {"Unblock " + (blocked[i].name.empty() ? blocked[i].channel_id : blocked[i].name),
             kPickUnblockBase + static_cast<int>(i)});
    card_menu_.open("Blocked Channels", std::move(entries));
}

void App::open_name_prompt(const std::string &video_id)
{
    prompt_video_ = video_id;
    prompt_text_.clear();
    prompt_active_ = true;
    prompt_keys_.set_length(0);
    prompt_keys_.enter();
    prompt_keys_.set_active(true);
}

void App::draw_prompt(ui::Canvas &canvas) const
{
    if (!prompt_active_)
        return;
    gfx::DrawList &list = canvas.list;
    list.rounded_rect({0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0.0f,
                      gfx::Color::rgb(0x000000, 0.75f));
    ui::text(list, fonts_.semibold, "New playlist name", 560.0f, 300.0f, 40.0f,
             gfx::Color::rgb(0xffffff));
    const gfx::Rect field{560.0f, 340.0f, 1080.0f, 84.0f};
    list.rounded_rect(field, 12.0f, gfx::Color::rgb(0x262626));
    const bool blink = static_cast<int>(clock_ * 2.0f) % 2 == 0;
    ui::text(
        list, fonts_.regular,
        fonts_.regular.font != nullptr
            ? fonts_.regular.font->fit(prompt_text_ + (blink ? "|" : " "), 34.0f, field.w - 48.0f)
            : prompt_text_,
        field.x + 24.0f, field.cy() + 12.0f, 34.0f, gfx::Color::rgb(0xffffff));
    prompt_keys_.draw(canvas);
    ui::text(list, fonts_.regular, "Done creates the playlist   Circle cancels", 560.0f, 1030.0f,
             22.0f, gfx::Color::rgb(0x9e9e9e));
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
    if (pick >= kPickUnblockBase)
    {
        const auto blocked = service_.blocked_channels();
        const std::size_t index = static_cast<std::size_t>(pick - kPickUnblockBase);
        if (index < blocked.size())
        {
            service_.unblock_channel(index);
            toasts_.push(ui::StatusKind::info, "Unblocked",
                         blocked[index].name.empty() ? blocked[index].channel_id
                                                     : blocked[index].name,
                         2.0f);
        }
        return;
    }
    if (pick >= kPickAccountBase)
    {
        const int index = pick - kPickAccountBase;
        const auto accounts = service_.saved_accounts();
        if (index >= 0 && index < static_cast<int>(accounts.size()) &&
            !accounts[static_cast<std::size_t>(index)].active)
        {
            service_.switch_account(index);
            toasts_.push(ui::StatusKind::info, "Switching account", {}, 2.0f);
        }
        return;
    }
    if (pick == kPickAddAccount)
    {
        if (service_.signed_in())
            service_.add_account();
        else
            service_.request_sign_in();
        login_return_ = current_ == kLoginScreen ? previous_tab() : current_;
        rail_focused_ = false;
        avatar_focus_ = false;
        show(kLoginScreen);
        return;
    }
    if (pick == kPickSignOut)
    {
        service_.sign_out();
        toasts_.push(ui::StatusKind::info, "Signed out");
        return;
    }
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
        if (item.is_channel() || !item.channel_id.empty())
        {
            pending_channel_ = {item.is_channel() ? item.video_id : item.channel_id,
                                item.is_channel() ? item.title : item.author,
                                {},
                                {}};
            context_.nav.open_channel = true;
        }
        else
            service_.resolve_channel(item.video_id, item.author, 0); // opened when it answers
        break;
    case kPickBlock:
    {
        yt::ChannelItem channel;
        channel.channel_id = item.is_channel() ? item.video_id : item.channel_id;
        channel.name = item.is_channel() ? item.title : item.author;
        service_.block_channel(channel);
        toasts_.push(ui::StatusKind::info, "Channel blocked",
                     channel.name + " (Blocked Channels in the sidebar undoes it)", 3.0f);
        break;
    }
    case kPickPin:
        if (item.is_channel() || !item.channel_id.empty())
            pin_channel({item.is_channel() ? item.video_id : item.channel_id,
                         item.is_channel() ? item.title : item.author,
                         {},
                         {}});
        else
            service_.resolve_channel(item.video_id, item.author, 2); // pinned when it answers
        break;
    case kPickUnpin:
        unpin_channel(item.is_channel() ? item.video_id : item.channel_id);
        break;
    case kPickNewPlaylist:
        open_name_prompt(item.video_id);
        break;
    case kPickClearHistory:
        card_menu_.open("Clear all watch history?",
                        {{"Clear history", kPickClearConfirm}, {"Cancel", kPickCancel}});
        break;
    case kPickClearConfirm:
    {
        const std::string token = service_.feed(yt::FeedKind::history).clear_history_token;
        if (!token.empty())
        {
            service_.send_feedback(token, "Watch history cleared");
            service_.request_history();
        }
        break;
    }
    case kPickRemoveHistory:
        service_.send_feedback(item.remove_history_token, "Removed from history");
        service_.hide_item(item.video_id);
        break;
    case kPickPlayNext:
        play_queue_.push_back(item);
        toasts_.push(ui::StatusKind::info, "Plays next", item.title, 2.0f);
        break;
    case kPickSbExclude:
    {
        // SmartTube's "Exclude this channel from SponsorBlock" (and back).
        const auto at = std::find(sb_excluded_.begin(), sb_excluded_.end(), item.channel_id);
        if (at != sb_excluded_.end())
        {
            sb_excluded_.erase(at);
            toasts_.push(ui::StatusKind::info, "SponsorBlock on for this channel", item.author,
                         2.0f);
        }
        else if (item.channel_id.size() == 24)
        {
            sb_excluded_.push_back(item.channel_id);
            toasts_.push(ui::StatusKind::info, "SponsorBlock off for this channel", item.author,
                         2.0f);
        }
        save_sb_excluded();
        break;
    }
    case kPickShuffle:
    {
        // SmartTube's shuffle: the section (Home, the playlist, the channel)
        // plays in random order, starting anywhere in it.
        std::vector<yt::FeedItem> videos;
        for (const yt::FeedItem &candidate : current_section(item.video_id))
            if (candidate.video_id.size() == 11)
                videos.push_back(candidate);
        if (videos.empty())
            videos.push_back(item);
        settings_.playback_mode = 4;
        settings_changed_ = true;
        const std::size_t pick =
            static_cast<std::size_t>(static_cast<std::uint64_t>(clock_ * 7919.0) % videos.size());
        section_items_ = videos;
        shuffle_started_ = true;
        context_.nav.open_video = true;
        context_.nav.video_id = videos[pick].video_id;
        toasts_.push(ui::StatusKind::info, "Shuffle", std::to_string(videos.size()) + " videos",
                     2.0f);
        break;
    }
    case kPickSubscribe:
        if (item.is_channel() || !item.channel_id.empty())
        {
            service_.set_subscribed(item.is_channel() ? item.video_id : item.channel_id, true);
            toasts_.push(ui::StatusKind::success, "Subscribed",
                         item.is_channel() ? item.title : item.author, 2.0f);
        }
        else
            service_.resolve_channel(item.video_id, item.author, 1);
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
        if (service_.signed_in() && !item.not_interested_token.empty())
            service_.send_feedback(item.not_interested_token,
                                   "Got it. You'll see fewer videos like this");
        else
            toasts_.push(ui::StatusKind::info, "Video hidden", item.title, 2.0f);
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
    // Idle time for the screen dimming: any press or stick move resets it.
    idle_s_ = input.nav != Direction::none || input.pressed != 0 ? 0.0f : idle_s_ + dt;
    if (settings_.hidden_sections != rail_hidden_)
        rebuild_rail();
    card_menu_.set_scheme(settings_.color_scheme);
    service_.set_preferences(preferences());
    if (context_.sb_excluded_changed)
    {
        context_.sb_excluded_changed = false;
        save_sb_excluded();
    }
    // An account whose token is kept but did not refresh (no network yet,
    // a hiccup at Google) is tried again every 20 s instead of looking
    // signed out until the app restarts.
    if (started_ && !service_.signed_in() && service_.has_token())
    {
        token_retry_s_ += dt;
        if (token_retry_s_ >= 20.0f)
        {
            token_retry_s_ = 0.0f;
            service_.refresh_token();
        }
    }
    else
        token_retry_s_ = 0.0f;
    context_.nav = Nav{};

    pump_remote();

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
    // The frame the focus moves between the rail and the screen, the screen
    // must not read the press that moved it (Right would also step one card,
    // Circle would also act on the page).
    bool focus_moved = false;
    // A menu opened in this frame does not read the press that opened it.
    const bool menu_was_active = card_menu_.active();
    if (current_ >= kTabCount)
        rail_focused_ = false;
    else if (!update_held && !menu_was_active)
    {
        if (input.is_pressed(Action::touch))
            rail_focused_ = !rail_focused_;
        if (!rail_focused_)
            avatar_focus_ = false;
        if (rail_focused_ && avatar_focus_)
        {
            // The account picture beside the search orb: Cross switches to
            // the next remembered account at once; Square (or Cross with one
            // account) opens the account menu to add or pick one.
            const auto accounts = service_.saved_accounts();
            if (input.is_pressed(Action::confirm) && accounts.size() >= 2)
            {
                std::size_t active = 0;
                for (std::size_t i = 0; i < accounts.size(); ++i)
                    if (accounts[i].active)
                        active = i;
                const std::size_t next = (active + 1) % accounts.size();
                feedback_.play(audio::Cue::select);
                service_.switch_account(static_cast<int>(next));
                toasts_.push(ui::StatusKind::info,
                             "Switching to " + (accounts[next].name.empty()
                                                    ? "account " + std::to_string(next + 1)
                                                    : accounts[next].name),
                             {}, 2.0f);
            }
            else if (input.is_pressed(Action::confirm) || input.is_pressed(Action::west))
            {
                feedback_.play(audio::Cue::select);
                open_account_menu();
            }
            else if (input.nav == Direction::left || input.nav == Direction::down ||
                     input.is_pressed(Action::back))
            {
                avatar_focus_ = false;
                feedback_.play(audio::Cue::focus);
            }
        }
        else if (rail_focused_ && rail_.focus() == 0 && input.nav == Direction::right)
        {
            avatar_focus_ = true;
            feedback_.play(audio::Cue::focus);
        }
        else if (rail_focused_)
        {
            if (input.is_pressed(Action::west))
            {
                rail_pinned_ = !rail_pinned_;
                feedback_.play(audio::Cue::toggle);
            }
            const ui::Event event = rail_.handle(input, feedback_);
            if (event == ui::Event::activated)
                activate_rail(rail_.focus()); // a section keeps the rail; actions act
            else if (event == ui::Event::moved)
                rail_settle_ = 0.0f; // SmartTube opens a section as the focus rests on it
            else if (event == ui::Event::cancelled || input.nav == Direction::right)
            {
                // Back to the content; the rail stays open one more frame so
                // the hand-off reads as a transition, then collapses.
                rail_focused_ = false;
                rail_keep_expanded = true;
                focus_moved = true;
            }
            // Scrolling the sections opens them: once the focus has rested
            // on a section a moment, it shows (Search, the account's own
            // pages and pinned channels still wait for Cross).
            if (rail_focused_ && rail_settle_ >= 0.0f)
            {
                rail_settle_ += dt;
                if (rail_settle_ >= 0.3f)
                {
                    rail_settle_ = -1.0f;
                    const auto &entries = rail_.entries();
                    const int position = rail_.focus();
                    if (position >= 0 && position < static_cast<int>(entries.size()))
                    {
                        const int tag = entries[static_cast<std::size_t>(position)].tag;
                        if (tag >= 0 && tag != static_cast<int>(kSearchScreen) &&
                            static_cast<std::size_t>(tag) != current_)
                        {
                            rail_.set_current(position, false);
                            show(static_cast<std::size_t>(tag));
                        }
                    }
                }
            }
        }
        else if (input.is_pressed(Action::back) && !prompt_active_ &&
                 !screens_[current_]->uses_back())
        {
            // Circle in a section's content goes to the sidebar, as in
            // SmartTube; Circle there comes back.
            rail_focused_ = true;
            rail_settle_ = -1.0f;
            focus_moved = true;
            feedback_.play(audio::Cue::back);
        }
        if (focus_moved)
            avatar_focus_ = false;
    }

    InputFrame screen_input = input;
    if (prompt_active_)
    {
        // The name prompt holds every input until it is done or cancelled.
        if (prompt_keys_.handle(input, feedback_) == ui::Event::cancelled ||
            input.is_pressed(Action::back))
            prompt_active_ = false;
        prompt_keys_.update(dt);
        screen_input = InputFrame{};
        screen_input.connected = input.connected;
    }
    else if (rail_focused_ || focus_moved || update_held || card_menu_.active())
    {
        if (menu_was_active && !update_held)
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
    // A card's channel was looked up: open it or subscribe to it.
    if (service_.resolved_revision() != seen_resolved_revision_)
    {
        seen_resolved_revision_ = service_.resolved_revision();
        const auto [channel, purpose] = service_.resolved_channel();
        if (channel.channel_id.empty())
            toasts_.push(ui::StatusKind::warning, "Channel not found", channel.name, 2.5f);
        else if (purpose == 1)
        {
            service_.set_subscribed(channel.channel_id, true);
            toasts_.push(ui::StatusKind::success, "Subscribed", channel.name, 2.0f);
        }
        else if (purpose == 2)
            pin_channel(channel);
        else
        {
            pending_channel_ = channel;
            context_.nav.open_channel = true;
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
    // While a video plays, pictures load one at a time: the decoder keeps
    // the CPU and the start does not stutter.
    thumbs_->set_quiet(current_ == kPlayerScreen);
    screens_[current_]->update(screen_input, dt, feedback_);
    thumbs_->pump();
    // A video request that neither arrives nor fails would leave the player on
    // "loading" forever; the watchdog ends it and logs why.
    service_.video_watchdog(dt);

    // A grid that ran off its left edge hands the focus to the rail; the screen
    // keeps its own focus until it is returned.
    if (current_ < kTabCount && screens_[current_]->wants_rail_focus())
    {
        rail_focused_ = true;
        rail_settle_ = -1.0f;
    }

    rail_.set_focused(rail_focused_);
    rail_.set_expanded(rail_focused_ || rail_pinned_ || rail_keep_expanded);
    rail_.set_current(rail_position(current_ < kTabCount ? current_ : previous_tab()), false);
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
    if (ask.open_video && ask.video_id.size() == 24 && ask.video_id.rfind("UC", 0) == 0)
    {
        // A channel card: its channel page.
        pending_channel_ = {ask.video_id, {}, {}, {}};
        context_.nav.open_channel = true;
    }
    else if (ask.open_video && ask.video_id.size() > 2 && ask.video_id.rfind("VL", 0) == 0)
    {
        // A playlist card: its videos on the browse screen.
        pending_browse_ = ask.video_id;
        show(kBrowseScreen);
    }
    else if (ask.open_video && !ask.video_id.empty())
    {
        // Shuffle picks from the section the video came from; a suggestion
        // opened in the player keeps the section it had.
        if (current_ != kPlayerScreen && !shuffle_started_)
            section_items_ = current_section(ask.video_id);
        shuffle_started_ = false;
        active_video_ = ask.video_id;
        const std::size_t player = kPlayerScreen;
        if (current_ != player)
        {
            // Circle in the player comes back here: a playlist page too.
            player_return_ =
                current_ == kBrowseScreen || current_ == kChannelScreen ? current_ : previous_tab();
            show(player); // the player screen reads the id when it enters
        }
    }
    if (ask.open_channel && !pending_channel_.channel_id.empty())
    {
        // The channel's own page, as on YouTube; Circle there comes back to
        // where it was opened (the player's section when it was the player).
        if (current_ != kChannelScreen)
            channel_return_ =
                current_ < kTabCount || current_ == kBrowseScreen ? current_ : previous_tab();
        active_video_.clear();
        service_.request_close_video();
        if (current_ == kChannelScreen)
            screens_[current_]->enter(); // already here: open the new channel
        else
            show(kChannelScreen);
    }
    if (ask.leave_channel && current_ == kChannelScreen)
        show(channel_return_);
    if (ask.leave_browse && current_ == kBrowseScreen)
        show(previous_tab());
    if (ask.leave_player && current_ == kPlayerScreen)
    {
        active_video_.clear();
        service_.request_close_video();
        show(player_return_ == kBrowseScreen || player_return_ == kChannelScreen ? player_return_
                                                                                 : previous_tab());
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
    if (ask.add_account)
    {
        service_.add_account();
        login_return_ = current_ == kLoginScreen ? previous_tab() : current_;
        show(kLoginScreen);
    }
    if (ask.switch_account >= 0)
    {
        service_.switch_account(ask.switch_account);
        toasts_.push(ui::StatusKind::info, "Switching account", {}, 2.0f);
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
    if (ask.backup_settings || ask.restore_settings)
    {
        // SmartTube's settings backup: the settings file, the pinned
        // channels and the SponsorBlock exclusions, copied as they are.
        const std::string backup = data_root_ + "/backup";
        (void)save::ensure_directory(backup);
        if (ask.backup_settings)
            save_settings(); // the file on disk is what gets copied: make it current
        int copied = 0;
        for (const char *name : {kSettingsFile, "/pins.txt", "/sb_excluded.txt"})
        {
            const std::string from = (ask.backup_settings ? data_root_ : backup) + name;
            const std::string to = (ask.backup_settings ? backup : data_root_) + name;
            std::string data;
            if (save::read_file(from, &data, 1u << 20) && save::write_atomic(to, data).empty())
                ++copied;
        }
        if (ask.backup_settings)
            toasts_.push(copied > 0 ? ui::StatusKind::success : ui::StatusKind::warning,
                         copied > 0 ? "Settings backed up" : "Nothing backed up", backup, 3.0f);
        else if (copied > 0)
        {
            // The restored values take over now; the settings pages show
            // them after the next start.
            settings_ = load_settings(data_root_);
            load_pins();
            load_sb_excluded();
            rebuild_rail();
            toasts_.push(ui::StatusKind::success, "Settings restored",
                         "Some pages show the new values after a restart", 3.5f);
        }
        else
            toasts_.push(ui::StatusKind::warning, "No backup found", backup, 3.0f);
    }

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

    if (ask.new_tv_code && lounge_ != nullptr)
        lounge_->new_code();
    if (lounge_ != nullptr && lounge_->running())
    {
        const Remote &remote = context_.remote;
        if (current_ == kPlayerScreen)
            lounge_->report(remote.video, remote.position_s, remote.duration_s, remote.state);
        else
            lounge_->report(std::string(), 0.0, 0.0, -1);
    }

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

void App::pump_remote()
{
    // Settings > Remote control: the lounge runs while it is on.
    if (!started_ || lounge_ == nullptr)
        return;
    if (settings_.remote_control != lounge_->running())
    {
        if (settings_.remote_control)
            lounge_->start();
        else
            lounge_->stop();
    }
    Remote &remote = context_.remote;
    for (yt::Lounge::Command &command : lounge_->take_commands())
    {
        using Kind = yt::Lounge::Command::Kind;
        const bool in_player = current_ == kPlayerScreen;
        switch (command.kind)
        {
        case Kind::play_video:
            if (!command.videos.empty())
                remote_list_ = command.videos; // the phone's queue, for Next/Previous
            if (in_player && active_video_ == command.video_id)
                remote.seek_s = command.time_s; // the same video: just move
            else
            {
                remote.start_video = command.video_id;
                remote.start_s = command.time_s;
                context_.nav.open_video = true;
                context_.nav.video_id = command.video_id;
            }
            break;
        case Kind::play:
            remote.paused = 0;
            break;
        case Kind::pause:
            remote.paused = 1;
            break;
        case Kind::seek:
            remote.seek_s = command.time_s;
            break;
        case Kind::stop:
            if (in_player)
                context_.nav.leave_player = true;
            break;
        case Kind::next:
        case Kind::previous:
        {
            // Through the phone's queue when the video is in it, otherwise
            // what the player's own Next / Previous do.
            const auto at = std::find(remote_list_.begin(), remote_list_.end(), active_video_);
            const bool next = command.kind == Kind::next;
            if (at != remote_list_.end() &&
                (next ? at + 1 != remote_list_.end() : at != remote_list_.begin()))
            {
                const std::string &id = next ? *(at + 1) : *(at - 1);
                remote.start_video = id;
                remote.start_s = 0.0;
                context_.nav.open_video = true;
                context_.nav.video_id = id;
            }
            else if (next)
                remote.next = true;
            else
                remote.previous = true;
            break;
        }
        case Kind::volume:
            break; // the console's volume is the TV's
        }
    }
}

void App::draw_sidebar(ui::Canvas &canvas)
{
    gfx::DrawList &list = canvas.list;
    const float open = rail_.expansion();
    const auto mix = [open](float closed, float opened)
    { return closed + (opened - closed) * open; };
    const gfx::Color grey = gfx::Color::rgb(0x8b8b8b);
    const gfx::Color white = gfx::Color::rgb(0xffffff);
    if (open > 0.01f)
        list.rounded_rect(
            {0.0f, 0.0f, mix(104.0f, 540.0f), gfx::kVirtualHeight}, 0.0f,
            palette(settings_.color_scheme).sidebar.with_alpha(std::min(1.0f, open * 1.5f)));

    // The search orb (entry 0) and the account's picture.
    const int focus = rail_focused_ ? rail_.focus() : -1;
    const float orb_y = mix(92.0f, 112.0f);
    const float orb_r = mix(30.0f, 50.0f);
    const float search_x = mix(52.0f, 160.0f);
    list.circle(search_x, orb_y, orb_r, gfx::Color::rgb(0xe52d27));
    if (focus == 0 && !avatar_focus_)
        list.ring(search_x, orb_y, orb_r + 7.0f, 4.0f, white);
    if (rail_.icon)
    {
        const float icon = orb_r * 1.05f;
        rail_.icon(canvas, {search_x - icon * 0.5f, orb_y - icon * 0.5f, icon, icon},
                   rail_.entries()[0], 0, 0.0f, white);
    }
    const std::pair<std::string, std::string> account = service_.account();
    const float avatar_x = search_x + orb_r * 2.0f + mix(14.0f, 26.0f);
    if (avatar_focus_)
        list.ring(avatar_x, orb_y, orb_r + 7.0f, 4.0f, white);
    if (account.second.empty() && (avatar_focus_ || open > 0.5f))
    {
        // No picture yet (or signed out): a grey orb with a person.
        list.circle(avatar_x, orb_y, orb_r, gfx::Color::rgb(0x3a3a3a));
        list.circle(avatar_x, orb_y - orb_r * 0.25f, orb_r * 0.3f, white);
        list.rounded_rect({avatar_x - orb_r * 0.5f, orb_y + orb_r * 0.12f, orb_r, orb_r * 0.45f},
                          orb_r * 0.22f, white);
    }
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
    // More entries than fit: the list scrolls to keep the focus (or the
    // current section) in view, as SmartTube's does.
    const int rows = static_cast<int>(entries.size()) - 1;
    const int visible = std::max(1, static_cast<int>((1050.0f - top) / pitch));
    const int anchor = (focus >= 1 ? focus : current) - 1;
    const int offset = std::clamp(anchor - (visible - 2), 0, std::max(0, rows - visible));
    for (std::size_t i = 1 + static_cast<std::size_t>(offset);
         i < entries.size() && static_cast<int>(i) <= offset + visible; ++i)
    {
        const int index = static_cast<int>(i);
        const float y = top + static_cast<float>(index - 1 - offset) * pitch;
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
        constexpr float kClockX = 1578.0f;
        if (settings_.corner_clock && sys::local_clock(&hour, &minute, &twelve))
        {
            twelve = !settings_.time_24h;
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
        if (settings_.corner_clock && sys::local_date(&weekday, &month, &day) && month >= 1 &&
            month <= 12)
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
        // SmartTube's logo: the red TV with its "S" (assets/icons/logo.png).
        const gfx::Rect badge{1708.0f, 66.0f, 100.0f, 79.0f};
        if (!draw_icon(overlay.list, "logo", badge, gfx::Color::rgb(0xffffff)))
            overlay.list.rounded_rect(badge, 10.0f, gfx::Color::rgb(0xe52d27));
        // No build number or button hints along the bottom, as in SmartTube
        // (Settings > Updates names the version). A newer release only
        // shows as a white dot on the logo.
        if (!update_latest_.empty())
            overlay.list.circle(badge.x + badge.w - 4.0f, badge.y + 4.0f, 9.0f,
                                gfx::Color::rgb(0xffffff));
    }
    // Settings > General > Screen dimming: nothing pressed for a while (and
    // no video playing) darkens the screen until the next press.
    if (settings_.dim_minutes > 0 && current_ != kPlayerScreen)
    {
        const float limit = static_cast<float>(settings_.dim_minutes) * 60.0f;
        const float dim = std::clamp((idle_s_ - limit) / 2.0f, 0.0f, 1.0f) * 0.8f;
        if (dim > 0.0f)
            overlay.list.rounded_rect({0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0.0f,
                                      gfx::Color::rgb(0x000000, dim));
    }
    toasts_.set_bounds({0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight});
    updates_.draw(overlay);
    card_menu_.draw(overlay, fonts_);
    draw_prompt(overlay);
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

// youtube-tv - Settings screen.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/settings_screen.hpp"

#include "app/icons.hpp"

#include "app/theme.hpp"
#include "yt/http.hpp"
#include "yt/log.hpp"
#include "yt/service.hpp"
#include "yt/update.hpp"
#include "ui/components.hpp"

#include <pthread.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

namespace hui::app
{
namespace
{

// ---- Network test --------------------------------------------------------------------------
// Settings > Network test: one request to each service the app needs, on a
// worker thread, each answer shown as a toast and logged. Any HTTP status
// means the service was reached; only a transport error is a failure.
struct NetTest
{
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> results; // label, outcome
    bool running = false;
};

NetTest &net_test()
{
    static NetTest test;
    return test;
}

void *net_test_main(void *)
{
    static const char *const kTargets[][2] = {
        {"Plain HTTP (DNS + network)", "http://connectivitycheck.gstatic.com/generate_204"},
        {"Google HTTPS", "https://www.google.com/generate_204"},
        {"YouTube", "https://www.youtube.com/generate_204"},
        {"Google sign-in", "https://oauth2.googleapis.com/device/code"},
        {"SponsorBlock", "https://sponsor.ajay.app/api/skipSegments?videoID=dQw4w9WgXcQ"},
    };
    for (const auto &target : kTargets)
    {
        std::string host = target[1];
        host = host.substr(host.find("//") + 2);
        host = host.substr(0, host.find('/'));
        const std::string lookup = yt::http::describe_lookup(host);
        const yt::http::Response response = yt::http::get(target[1], {}, 12);
        std::string outcome = (response.ok() ? "OK (HTTP " + std::to_string(response.status) + ")"
                                             : "FAILED: " + response.error) +
                              " - " + lookup;
        yt::log::line("nettest: %s %s -> %s", target[0], target[1], outcome.c_str());
        std::lock_guard<std::mutex> lock(net_test().mutex);
        net_test().results.emplace_back(target[0], std::move(outcome));
    }
    std::lock_guard<std::mutex> lock(net_test().mutex);
    net_test().running = false;
    return nullptr;
}

void start_net_test()
{
    std::lock_guard<std::mutex> lock(net_test().mutex);
    if (net_test().running)
        return;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0)
        return;
    pthread_attr_setstacksize(&attributes, 1u << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, net_test_main, nullptr) == 0)
    {
        pthread_detach(thread);
        net_test().running = true;
    }
    pthread_attr_destroy(&attributes);
}

enum : int
{
    kCodecAvc = 1,
    kCodecVp9,
    kCodecAv1,
    kMaxHeight,
    kDecodeMode,
    kHaptics,
    kSbEnabled,
    kSbMode,
    kSbScope,
    kSaveAccount,
    kSignOut,
    kCheckUpdates,
    kNetTest,
    kSignIn,
    kAbout,
    kAddAccount,
    kAccountBase = 100, // + the index of a remembered account
};

constexpr std::size_t kMaxAccounts = 3;

constexpr int kColumns = 6;
constexpr float kTileW = 236.0f;
constexpr float kTileH = 236.0f;
constexpr float kTileGap = 18.0f;
constexpr float kGridX = 170.0f;
constexpr float kGridY = 200.0f;

constexpr int kHeights[] = {720, 1080, 1440, 2160};

int height_to_index(int height)
{
    for (int i = 0; i < 4; ++i)
    {
        if (kHeights[i] == height)
            return i;
    }
    return 1; // 1080p
}

int index_to_height(int index)
{
    if (index < 0)
        index = 0;
    if (index > 3)
        index = 3;
    return kHeights[index];
}

// The stepper's order is the one a user reads (Software, Auto, Hardware); the
// enum's is auto = 0, software = 1, hardware = 2, so the two are mapped.
int decode_mode_to_index(yt::DecodeMode mode)
{
    switch (mode)
    {
    case yt::DecodeMode::software:
        return 0;
    case yt::DecodeMode::hardware:
        return 2;
    case yt::DecodeMode::auto_detect:
    default:
        return 1;
    }
}

yt::DecodeMode index_to_decode_mode(int index)
{
    if (index <= 0)
        return yt::DecodeMode::software;
    if (index >= 2)
        return yt::DecodeMode::hardware;
    return yt::DecodeMode::auto_detect;
}

const char *decode_mode_description(yt::DecodeMode mode)
{
    switch (mode)
    {
    case yt::DecodeMode::software:
        return "Software: ffmpeg on the CPU (every codec)";
    case yt::DecodeMode::hardware:
        return "Hardware: the console decoder where available (AV1 stays software)";
    case yt::DecodeMode::auto_detect:
    default:
        return "Auto: the console decoder for AVC/VP9, software for AV1";
    }
}

const char *sb_mode_description(int mode)
{
    switch (mode)
    {
    case 1:
        return "Offer Triangle: skip when a segment starts";
    case 2:
        return "Only mark segments on the seek bar; Triangle skips";
    case 0:
    default:
        return "Skip at once and show what was skipped";
    }
}

const char *sb_scope_description(int scope)
{
    switch (scope)
    {
    case 0:
        return "Paid sponsor reads";
    case 1:
        return "Sponsors, self-promotion and subscribe reminders";
    case 2:
    default:
        return "Also intros, outros, recaps and non-music parts";
    }
}

const TourStep kTour[] = {
    {1.0f, 0, Direction::none, "tiles"},
    {0.7f, 0, Direction::right, nullptr},
    {0.7f, hui::action_bit(Action::confirm), Direction::none, nullptr},
    {0.9f, 0, Direction::none, "toggles"},
    {0.7f, hui::action_bit(Action::down), Direction::none, nullptr},
    {0.7f, hui::action_bit(Action::down), Direction::none, nullptr},
    {0.7f, hui::action_bit(Action::down), Direction::none, nullptr},
    {0.9f, 0, Direction::none, "stepper"},
};

} // namespace

ui::Form &SettingsScreen::add_tile(const char *label, Icon icon)
{
    Tile tile;
    tile.label = label;
    tile.icon = icon;
    tile.form = std::make_unique<ui::Form>();
    tile.form->style.theme = youtube_theme();
    tile.form->set_bounds({480.0f, 220.0f, 960.0f, 740.0f});
    tiles_.push_back(std::move(tile));
    return *tiles_.back().form;
}

SettingsScreen::SettingsScreen(Context &context) : context_(context)
{
    using S = Settings;
    add_tile("Accounts", Icon::accounts);
    rebuild_accounts();

    // ---- User interface (SmartTube's "Main UI") ----
    ui::Form &ui_form = add_tile("User interface", Icon::main_ui);
    ui_form.add_header("Look");
    {
        std::vector<int> values;
        std::vector<std::string> labels;
        for (int i = 0; i < S::kColorSchemeCount; ++i)
        {
            values.push_back(i);
            labels.emplace_back(S::kColorSchemes[i]);
        }
        add_steps(ui_form, "Color scheme", &S::color_scheme, values, labels,
                  "The sidebar's and the menus' colors");
    }
    add_steps(ui_form, "Video grid scale", &S::grid_columns, {5, 4, 3},
              {"Small", "Normal", "Large"}, "Cards a row in Subscriptions, History and the grids");
    add_steps(ui_form, "Card title lines", &S::title_lines, {1, 2, 3}, {"1", "2", "3"});
    add_flag(ui_form, "Rounded card corners", &S::rounded_cards);
    add_flag(ui_form, "Home: top right corner clock", &S::corner_clock);
    add_flag(ui_form, "24-hour clock", &S::time_24h);
    ui_form.add_header("Sidebar sections");
    for (int i = 0; i < S::kSectionCount; ++i)
        add_section(ui_form, i);

    // ---- Player ----
    ui::Form &player = add_tile("Player", Icon::player);
    player.add_header("Playback");
    {
        std::vector<int> values;
        std::vector<std::string> labels;
        static const char *const kSpeedLabels[] = {"0.25x", "0.5x", "0.75x", "1x",
                                                   "1.25x", "1.5x", "1.75x", "2x"};
        for (int i = 0; i < S::kSpeedCount; ++i)
        {
            values.push_back(i);
            labels.emplace_back(kSpeedLabels[i]);
        }
        add_steps(player, "Video speed", &S::speed_index, values, labels,
                  "The speed every video starts at");
    }
    add_flag(player, "Remember speed", &S::remember_speed,
             "A speed picked in the player stays for the next video");
    add_steps(player, "Playback mode", &S::playback_mode, {0, 1, 2, 3},
              {"Next video", "Repeat", "Pause", "Close"},
              "When a video ends: the next one, the same again, a pause, or back out");
    add_flag(player, "Loop Shorts", &S::loop_shorts, "A Short starts again when it ends");
    add_flag(player, "Remember position", &S::resume, "Continue a video where you left it");
    add_flag(player, "Remember position of short videos", &S::resume_short,
             "Also videos shorter than 5 minutes");
    add_steps(player, "Seek interval", &S::seek_seconds, {5, 10, 15, 20, 30, 60},
              {"5 sec", "10 sec", "15 sec", "20 sec", "30 sec", "60 sec"},
              "Left/right on the seek bar, L1/R1 while playing");
    player.add_header("Controls");
    add_steps(player, "Auto-hide UI", &S::ui_hide_seconds, {3, 4, 5, 8, 10, 0},
              {"3 sec", "4 sec", "5 sec", "8 sec", "10 sec", "Never"});
    add_steps(player, "OK button behavior", &S::ok_behavior, {0, 1, 2}, {"UI", "Pause", "Both"},
              "What Cross does while the controls are hidden: show them, pause, or both");
    add_flag(player, "Show clock in controls bar", &S::show_clock);
    add_flag(player, "Show remaining time", &S::show_remaining);
    add_flag(player, "Show quality info", &S::show_quality);
    add_flag(player, "Show button tooltips", &S::show_tooltips);
    add_flag(player, "Suggestions", &S::suggestions, "The row of videos under the controls");
    player.add_header("Video formats");
    player.add_toggle(kCodecAvc, "AVC (H.264)", context_.settings.codec_avc);
    player.add_toggle(kCodecVp9, "VP9", context_.settings.codec_vp9);
    player.add_toggle(kCodecAv1, "AV1", context_.settings.codec_av1);
    add_flag(player, "Prefer AVC over VP9", &S::prefer_avc,
             "Steadier on the CPU decoder; 1080p at most");

    ui::FormRow &resolution = player.add_stepper(
        kMaxHeight, "Maximum resolution", height_to_index(context_.settings.max_height), 0, 3, 1);
    // The codec ceilings (VP9 720p, AV1 480p) cap the stream below this value
    // while decoding runs on the CPU; the selector's log line names what it picked.
    resolution.description = "Software decoding caps VP9 at 720p and AVC at 1080p; Auto lifts it";
    resolution.stepper.format = [](int value) -> std::string
    {
        static const char *const kLabels[] = {"720p", "1080p", "1440p", "2160p"};
        if (value < 0 || value > 3)
            return "1080p";
        return kLabels[value];
    };

    ui::FormRow &decode = player.add_stepper(
        kDecodeMode, "Decoding", decode_mode_to_index(context_.settings.decode_mode), 0, 2, 1);
    decode.description = decode_mode_description(context_.settings.decode_mode);
    decode.stepper.format = [](int value) -> std::string
    {
        static const char *const kLabels[] = {"Software", "Auto", "Hardware"};
        if (value < 0 || value > 2)
            return "Auto";
        return kLabels[value];
    };

    // ---- Subtitles ----
    ui::Form &subtitles = add_tile("Subtitles", Icon::subtitles);
    {
        std::vector<int> values;
        std::vector<std::string> labels;
        for (int i = 0; i < S::kSubtitleLanguageCount; ++i)
        {
            values.push_back(i);
            labels.emplace_back(S::kSubtitleLanguages[i][1]);
        }
        add_steps(subtitles, "Subtitle language", &S::subtitle_language, values, labels,
                  "Turned on by itself when a video has it");
    }
    add_steps(subtitles, "Text size", &S::subtitle_size, {0, 1, 2}, {"Small", "Medium", "Large"});
    add_flag(subtitles, "Background", &S::subtitle_background, "A dark box behind the text");

    // ---- General ----
    ui::Form &general = add_tile("General", Icon::general);
    {
        std::vector<int> values;
        std::vector<std::string> labels;
        for (int i = 0; i < S::kBootSectionCount; ++i)
        {
            values.push_back(i);
            labels.emplace_back(S::kBootSections[i]);
        }
        add_steps(general, "Boot to section", &S::boot_section, values, labels,
                  "The section the app opens on");
    }
    add_flag(general, "History", &S::history_enabled,
             "Off pauses history: YouTube is not told what you watch");
    add_steps(general, "Screen dimming", &S::dim_minutes, {0, 1, 3, 5, 10, 30},
              {"Never", "1 min", "3 min", "5 min", "10 min", "30 min"},
              "Darken the screen when nothing happens (not while a video plays)");
    general.add_toggle(kHaptics, "Haptics", context_.settings.haptics).description =
        "Rumble on clicks and drags";
    general.add_header("Hide content");
    add_flag(general, "Hide Shorts from Home", &S::hide_shorts_home);
    add_flag(general, "Hide Shorts from Subscriptions", &S::hide_shorts_subscriptions);
    add_flag(general, "Hide Shorts from History", &S::hide_shorts_history);
    add_flag(general, "Hide Shorts from search results", &S::hide_shorts_search);
    add_flag(general, "Hide watched videos from Home", &S::hide_watched_home);
    add_flag(general, "Hide watched videos from Subscriptions", &S::hide_watched_subscriptions);
    add_flag(general, "Hide streams from Subscriptions", &S::hide_streams_subscriptions);
    add_flag(general, "Hide Mixes", &S::hide_mixes);

    // ---- Search ----
    ui::Form &search = add_tile("Search", Icon::search);
    add_flag(search, "Show keyboard automatically", &S::keyboard_auto,
             "The keyboard opens as soon as Search does");

    // ---- SponsorBlock ----
    ui::Form &sponsor = add_tile("SponsorBlock", Icon::sponsorblock);
    sponsor.add_toggle(kSbEnabled, "Enable", context_.settings.sb_enabled).description =
        "Community-marked segments from sponsor.ajay.app";
    ui::FormRow &sb_mode =
        sponsor.add_stepper(kSbMode, "Choose action", context_.settings.sb_mode, 0, 2, 1);
    sb_mode.description = sb_mode_description(context_.settings.sb_mode);
    sb_mode.stepper.format = [](int value) -> std::string
    {
        static const char *const kLabels[] = {"Skip", "Ask", "Do nothing"};
        if (value < 0 || value > 2)
            return "Skip";
        return kLabels[value];
    };
    ui::FormRow &sb_scope =
        sponsor.add_stepper(kSbScope, "Categories", context_.settings.sb_scope, 0, 2, 1);
    sb_scope.description = sb_scope_description(context_.settings.sb_scope);
    sb_scope.stepper.format = [](int value) -> std::string
    {
        static const char *const kLabels[] = {"Sponsors", "+ Promotion", "Everything"};
        if (value < 0 || value > 2)
            return "Everything";
        return kLabels[value];
    };
    add_flag(sponsor, "Color markers on progress bar", &S::sb_markers);
    add_flag(sponsor, "Don't skip segments again", &S::sb_once,
             "A segment skipped once plays when you seek back to it");

    // One-shot tiles.
    tiles_.push_back({"Network test", Icon::network, nullptr, kNetTest});
    tiles_.push_back({"Updates", Icon::updates, nullptr, kCheckUpdates});
    tiles_.push_back({"About", Icon::about, nullptr, kAbout});
}

void SettingsScreen::add_flag(ui::Form &form, const char *label, bool Settings::*flag,
                              const char *description)
{
    Binding binding;
    binding.id = next_binding_id_++;
    binding.flag = flag;
    ui::FormRow &row = form.add_toggle(binding.id, label, context_.settings.*flag);
    if (description != nullptr)
        row.description = description;
    bindings_.push_back(std::move(binding));
}

void SettingsScreen::add_steps(ui::Form &form, const char *label, int Settings::*number,
                               std::vector<int> values, std::vector<std::string> labels,
                               const char *description)
{
    Binding binding;
    binding.id = next_binding_id_++;
    binding.number = number;
    int index = 0;
    for (std::size_t i = 0; i < values.size(); ++i)
        if (values[i] == context_.settings.*number)
            index = static_cast<int>(i);
    ui::FormRow &row =
        form.add_stepper(binding.id, label, index, 0, static_cast<int>(values.size()) - 1, 1);
    row.stepper.format = [labels](int value) -> std::string
    {
        if (value < 0 || value >= static_cast<int>(labels.size()))
            return {};
        return labels[static_cast<std::size_t>(value)];
    };
    if (description != nullptr)
        row.description = description;
    binding.values = std::move(values);
    bindings_.push_back(std::move(binding));
}

void SettingsScreen::add_section(ui::Form &form, int section)
{
    Binding binding;
    binding.id = next_binding_id_++;
    binding.section = section;
    form.add_toggle(binding.id, Settings::kSectionNames[section],
                    !context_.settings.section_hidden(section));
    bindings_.push_back(std::move(binding));
}

bool SettingsScreen::apply_binding(int id)
{
    for (const Binding &binding : bindings_)
    {
        if (binding.id != id)
            continue;
        Settings &settings = context_.settings;
        if (binding.section >= 0)
        {
            const std::uint32_t bit = 1u << binding.section;
            if (form().toggle_value(id))
                settings.hidden_sections &= ~bit;
            else
                settings.hidden_sections |= bit;
        }
        else if (binding.flag != nullptr)
            settings.*binding.flag = form().toggle_value(id);
        else if (binding.number != nullptr)
        {
            const int index = form().stepper_value(id);
            if (index >= 0 && index < static_cast<int>(binding.values.size()))
                settings.*binding.number = binding.values[static_cast<std::size_t>(index)];
        }
        context_.settings_changed = true;
        return true;
    }
    return false;
}

void SettingsScreen::rebuild_accounts()
{
    const std::vector<yt::Service::AccountInfo> accounts = context_.service.saved_accounts();
    std::string key = context_.service.signed_in() ? "in" : "out";
    for (const auto &account : accounts)
        key += "|" + account.name + (account.active ? "*" : "");
    if (key == accounts_key_ && tiles_[0].form != nullptr)
        return;
    accounts_key_ = key;
    auto form = std::make_unique<ui::Form>();
    form->style.theme = youtube_theme();
    form->set_bounds({480.0f, 220.0f, 960.0f, 740.0f});
    form->add_header("Accounts on this console");
    for (std::size_t i = 0; i < accounts.size(); ++i)
    {
        const auto &account = accounts[i];
        const std::string name =
            account.name.empty() ? "Account " + std::to_string(i + 1) : account.name;
        ui::FormRow &row = form->add_action(kAccountBase + static_cast<int>(i),
                                            account.active ? name + "  (current)" : name);
        row.description = account.active ? "Signed in now" : "Switch to this account";
    }
    if (!context_.service.signed_in() && accounts.empty())
        form->add_action(kSignIn, "Sign in").description =
            "Scan the code with your phone: the sign-in page opens with it filled in";
    else
    {
        ui::FormRow &add = form->add_action(kAddAccount, "Add account");
        add.description = accounts.size() >= kMaxAccounts
                              ? "Up to 3 accounts: sign one out first"
                              : "Sign in one more account; switch between them here";
        add.disabled = accounts.size() >= kMaxAccounts;
    }
    form->add_action(kSaveAccount, "Back up account").description =
        "Write a token copy into the app folder";
    form->add_action(kSignOut, "Sign out").description =
        accounts.size() > 1 ? "Remove the current account; the next one takes over"
                            : "Remove the account from this console";
    const bool showing = open_ == 0;
    tiles_[0].form = std::move(form);
    if (showing)
    {
        tiles_[0].form->enter();
        tiles_[0].form->set_active(true);
    }
}

void SettingsScreen::enter()
{
    open_ = -1;
    wants_rail_ = false;
}

void SettingsScreen::apply(int id, ui::Feedback &feedback)
{
    if (apply_binding(id))
        return;
    switch (id)
    {
    case kCodecAvc:
    case kCodecVp9:
    case kCodecAv1:
    {
        const bool on = form().toggle_value(id);
        const int enabled = (context_.settings.codec_avc ? 1 : 0) +
                            (context_.settings.codec_vp9 ? 1 : 0) +
                            (context_.settings.codec_av1 ? 1 : 0);
        // Turning off the last enabled codec must not stick.
        if (!on && enabled <= 1)
        {
            form().set_toggle(id, true);
            context_.toasts.push(ui::StatusKind::warning, "At least one codec must stay enabled");
            ui::play_cue(feedback, form().style, form().style.sounds.refuse);
            return;
        }
        if (id == kCodecAvc)
            context_.settings.codec_avc = on;
        else if (id == kCodecVp9)
            context_.settings.codec_vp9 = on;
        else
            context_.settings.codec_av1 = on;
        context_.settings_changed = true;
        break;
    }
    case kMaxHeight:
        context_.settings.max_height = index_to_height(form().stepper_value(id));
        context_.settings_changed = true;
        break;
    case kDecodeMode:
    {
        const yt::DecodeMode mode = index_to_decode_mode(form().stepper_value(id));
        context_.settings.decode_mode = mode;
        if (ui::FormRow *row = form().row(kDecodeMode))
            row->description = decode_mode_description(mode);
        context_.settings_changed = true;
        break;
    }
    case kHaptics:
        context_.settings.haptics = form().toggle_value(id);
        context_.settings_changed = true;
        break;
    case kSbEnabled:
        context_.settings.sb_enabled = form().toggle_value(id);
        context_.settings_changed = true;
        break;
    case kSbMode:
        context_.settings.sb_mode = form().stepper_value(id);
        if (ui::FormRow *row = form().row(kSbMode))
            row->description = sb_mode_description(context_.settings.sb_mode);
        context_.settings_changed = true;
        break;
    case kSbScope:
        context_.settings.sb_scope = form().stepper_value(id);
        if (ui::FormRow *row = form().row(kSbScope))
            row->description = sb_scope_description(context_.settings.sb_scope);
        context_.settings_changed = true;
        break;
    default:
        break;
    }
}

void SettingsScreen::activate(int id)
{
    if (id >= kAccountBase)
    {
        const int index = id - kAccountBase;
        const auto accounts = context_.service.saved_accounts();
        if (index >= 0 && index < static_cast<int>(accounts.size()) &&
            !accounts[static_cast<std::size_t>(index)].active)
            context_.nav.switch_account = index;
        return;
    }
    if (id == kAddAccount)
    {
        if (context_.service.saved_accounts().size() >= kMaxAccounts)
            context_.toasts.push(ui::StatusKind::warning, "Up to 3 accounts", "Sign one out first");
        else
            context_.nav.add_account = true;
        return;
    }
    if (id == kSignIn)
    {
        if (context_.service.signed_in())
            context_.toasts.push(ui::StatusKind::info, "Already signed in");
        else
            context_.nav.start_login = true;
        return;
    }
    if (id == kAbout)
    {
        context_.toasts.push(ui::StatusKind::info, "SmartTube for PS5",
                             "Unofficial PS5 port in the style of SmartTube (MIT, (c) yuliskov), "
                             "built on ytps5. SponsorBlock segments from sponsor.ajay.app "
                             "(CC BY-NC-SA 4.0).",
                             10.0f);
        return;
    }
    if (id == kSaveAccount)
    {
        // The sandbox copy in /download0 survives reboots and re-jailbreaks;
        // this writes the second copy the user can carry to a new install.
        std::string where;
        if (!context_.service.signed_in())
            context_.toasts.push(ui::StatusKind::warning, "Not signed in");
        else if (context_.service.export_token(&where))
            context_.toasts.push(ui::StatusKind::success, "Account backed up", where);
        else
            context_.toasts.push(ui::StatusKind::warning, "Copy not possible", where);
        return;
    }
    if (id == kNetTest)
    {
        context_.toasts.push(ui::StatusKind::info, "Network test running",
                             "Results appear one by one", 4.0f);
        start_net_test();
        return;
    }
    if (id == kCheckUpdates)
    {
        // The check runs in the background; update() reports how it settled.
        yt::update::check();
        update_watching_ = true;
        update_watch_revision_ = yt::update::revision();
        return;
    }
    if (id != kSignOut)
        return;
    const bool was_signed_in = context_.service.signed_in();
    context_.nav.sign_out = true;
    context_.toasts.push(ui::StatusKind::info, was_signed_in ? "Signed out" : "Not signed in");
}

void SettingsScreen::update_grid(const InputFrame &input, ui::Feedback &feedback)
{
    const int count = static_cast<int>(tiles_.size());
    const int column = focus_ % kColumns;
    int next = focus_;
    switch (input.nav)
    {
    case Direction::left:
        if (column == 0)
        {
            wants_rail_ = true;
            return;
        }
        next = focus_ - 1;
        break;
    case Direction::right:
        next = focus_ + 1;
        break;
    case Direction::up:
        next = focus_ - kColumns;
        break;
    case Direction::down:
        next = std::min(focus_ + kColumns, count - 1);
        break;
    default:
        break;
    }
    if (next != focus_)
    {
        if (next >= 0 && next < count && (input.nav != Direction::right || next % kColumns != 0))
        {
            focus_ = next;
            feedback.play(audio::Cue::focus);
        }
        else
            feedback.play(audio::Cue::error);
        return;
    }
    if (!input.is_pressed(Action::confirm) || focus_ < 0 || focus_ >= count)
        return;
    feedback.play(audio::Cue::select);
    Tile &tile = tiles_[static_cast<std::size_t>(focus_)];
    if (tile.form == nullptr)
    {
        activate(tile.action);
        return;
    }
    open_ = focus_;
    if (tile.icon == Icon::accounts)
        rebuild_accounts();
    form().enter();
    form().set_active(true);
}

void SettingsScreen::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    clock_ += dt;
    wants_rail_ = false;
    if (open_ == 0)
        rebuild_accounts(); // a switch or a new sign-in changes the list
    if (open_ < 0)
        update_grid(input, feedback);
    else
    {
        switch (form().handle(input, feedback))
        {
        case ui::Event::changed:
            apply(form().changed_id(), feedback);
            break;
        case ui::Event::activated:
            activate(form().changed_id());
            break;
        case ui::Event::cancelled:
            open_ = -1; // Circle: back to the tiles
            break;
        default:
            break;
        }
        if (open_ >= 0)
            form().update(dt);
    }

    {
        std::vector<std::pair<std::string, std::string>> done;
        {
            std::lock_guard<std::mutex> lock(net_test().mutex);
            done.swap(net_test().results);
        }
        for (auto &result : done)
        {
            const bool ok = result.second.starts_with("OK");
            context_.toasts.push(ok ? ui::StatusKind::success : ui::StatusKind::danger,
                                 result.first, result.second, 15.0f);
        }
    }

    if (!update_watching_)
        return;
    const std::uint64_t revision = yt::update::revision();
    if (revision == update_watch_revision_)
        return;
    update_watch_revision_ = revision;
    const yt::update::Snapshot snapshot = yt::update::snapshot();
    switch (snapshot.state)
    {
    case yt::update::State::idle:
    case yt::update::State::checking:
        break; // still working
    case yt::update::State::up_to_date:
        update_watching_ = false;
        context_.toasts.push(ui::StatusKind::success, "Up to date",
                             snapshot.current_version.empty()
                                 ? std::string("This is the newest version")
                                 : "Version " + snapshot.current_version);
        break;
    case yt::update::State::no_release:
        // The repository has no published release yet: nothing to update to, and
        // nothing to retry - report it once, like "up to date".
        update_watching_ = false;
        context_.toasts.push(ui::StatusKind::success, "No release published yet",
                             "Nothing to update to");
        break;
    case yt::update::State::available:
        update_watching_ = false;
        context_.toasts.push(ui::StatusKind::info, "Update available",
                             "Version " + snapshot.latest_version);
        break;
    case yt::update::State::failed:
        update_watching_ = false;
        context_.toasts.push(ui::StatusKind::warning, "Update check failed", snapshot.error);
        break;
    default: // downloading or staged: the app's own card owns that
        update_watching_ = false;
        break;
    }
}

namespace
{

// SmartTube's settings tiles (assets/icons, MIT, yuliskov).
const char *icon_name(SettingsScreen::Icon icon)
{
    switch (icon)
    {
    case SettingsScreen::Icon::accounts:
        return "set_account";
    case SettingsScreen::Icon::player:
        return "set_player";
    case SettingsScreen::Icon::sponsorblock:
        return "set_block";
    case SettingsScreen::Icon::main_ui:
        return "set_main_ui";
    case SettingsScreen::Icon::subtitles:
        return "set_subtitles";
    case SettingsScreen::Icon::search:
        return "set_search";
    case SettingsScreen::Icon::general:
        return "set_general";
    case SettingsScreen::Icon::network:
        return "set_network";
    case SettingsScreen::Icon::updates:
        return "set_updates";
    case SettingsScreen::Icon::about:
        return "set_about";
    }
    return "set_general";
}

} // namespace

void SettingsScreen::draw_icon(gfx::DrawList &list, Icon icon, const gfx::Rect &box) const
{
    const gfx::Color ink = gfx::Color::rgb(0xffffff);
    const gfx::Color hole = gfx::Color::rgb(0x1e1e1e);
    const float cx = box.cx();
    const float cy = box.cy();
    const float s = box.w * 0.5f;
    switch (icon)
    {
    case Icon::accounts: // a white square with a person cut out
        list.rounded_rect({cx - s, cy - s, s * 2.0f, s * 2.0f}, 6.0f, ink);
        list.circle(cx, cy - s * 0.25f, s * 0.32f, hole);
        list.rounded_rect({cx - s * 0.62f, cy + s * 0.2f, s * 1.24f, s * 0.6f}, s * 0.3f, hole);
        list.rounded_rect({cx - s * 0.62f, cy + s * 0.5f, s * 1.24f, s * 0.3f}, 0.0f, hole);
        break;
    case Icon::player: // a clapperboard
        list.rounded_rect({cx - s, cy - s * 0.35f, s * 2.0f, s * 1.25f}, 6.0f, ink);
        list.rounded_rect({cx - s, cy - s * 0.85f, s * 2.0f, s * 0.38f}, 4.0f, ink);
        for (int i = 0; i < 4; ++i)
        {
            const float x = cx - s * 0.7f + static_cast<float>(i) * s * 0.5f;
            list.line(x, cy - s * 0.85f, x + s * 0.22f, cy - s * 0.47f, s * 0.12f, hole);
        }
        break;
    case Icon::sponsorblock: // a shield with a play mark
        list.rounded_rect({cx - s * 0.8f, cy - s, s * 1.6f, s * 1.1f}, 10.0f, ink);
        list.triangle({cx - s * 0.8f, cy + s * 0.05f, s * 1.6f, s * 0.95f}, ink, 0.0f, 3.14159265f);
        list.triangle({cx - s * 0.22f, cy - s * 0.42f, s * 0.6f, s * 0.6f}, hole, 0.0f, 1.5707963f);
        break;
    case Icon::main_ui:
    case Icon::subtitles:
    case Icon::search:
    case Icon::general: // a gear
        for (int i = 0; i < 8; ++i)
        {
            const float angle = static_cast<float>(i) * 0.785398f;
            list.line(cx, cy, cx + std::cos(angle) * s * 0.95f, cy + std::sin(angle) * s * 0.95f,
                      s * 0.36f, ink);
        }
        list.circle(cx, cy, s * 0.72f, ink);
        list.circle(cx, cy, s * 0.3f, hole);
        break;
    case Icon::network: // a globe
        list.ring(cx, cy, s * 0.9f, s * 0.12f, ink);
        list.ring(cx, cy, s * 0.42f, s * 0.1f, ink);
        list.line(cx - s * 0.9f, cy, cx + s * 0.9f, cy, s * 0.1f, ink);
        list.line(cx, cy - s * 0.9f, cx, cy + s * 0.9f, s * 0.1f, ink);
        break;
    case Icon::updates: // a cloud with an arrow up
        list.circle(cx - s * 0.45f, cy + s * 0.1f, s * 0.42f, ink);
        list.circle(cx + s * 0.05f, cy - s * 0.2f, s * 0.55f, ink);
        list.circle(cx + s * 0.55f, cy + s * 0.15f, s * 0.38f, ink);
        list.rounded_rect({cx - s * 0.85f, cy + s * 0.1f, s * 1.75f, s * 0.42f}, s * 0.2f, ink);
        list.triangle({cx - s * 0.3f, cy - s * 0.35f, s * 0.6f, s * 0.36f}, hole);
        list.rounded_rect({cx - s * 0.1f, cy - s * 0.02f, s * 0.2f, s * 0.42f}, 0.0f, hole);
        break;
    case Icon::about: // an "i" in a circle
        list.circle(cx, cy, s * 0.9f, ink);
        list.circle(cx, cy - s * 0.42f, s * 0.13f, hole);
        list.rounded_rect({cx - s * 0.1f, cy - s * 0.18f, s * 0.2f, s * 0.68f}, 2.0f, hole);
        break;
    }
}

void SettingsScreen::draw(const Context &context, Frame &frame) const
{
    ui::Canvas canvas{frame.scene, context.fonts, frame.glass_texture, clock_};
    gfx::DrawList &list = canvas.list;
    if (open_ >= 0)
    {
        // One group's options, under its name.
        const Tile &tile = tiles_[static_cast<std::size_t>(open_)];
        ui::text(list, context.fonts.semibold, tile.label, 480.0f, 170.0f, 40.0f,
                 gfx::Color::rgb(0xf1f1f1));
        tile.form->draw(canvas);
        if (tile.icon == Icon::accounts)
            ui::text(list, context.fonts.regular,
                     context.service.signed_in() ? "Signed in" : "Not signed in", 480.0f, 1000.0f,
                     22.0f, gfx::Color::rgb(0x999999));
        return;
    }
    // SmartTube's tiles: dark squares, a white icon, the name under it; the
    // focused tile lightens and its name sits on a white strip.
    for (std::size_t i = 0; i < tiles_.size(); ++i)
    {
        const int index = static_cast<int>(i);
        const float x = kGridX + static_cast<float>(index % kColumns) * (kTileW + kTileGap);
        const float y = kGridY + static_cast<float>(index / kColumns) * (kTileH + kTileGap);
        const bool focused = index == focus_;
        const gfx::Rect tile{x, y, kTileW, kTileH};
        list.rounded_rect(tile, 0.0f, gfx::Color::rgb(focused ? 0x2c2c2c : 0x1e1e1e));
        const gfx::Rect art{tile.cx() - 56.0f, y + 48.0f, 112.0f, 112.0f};
        if (!app::draw_icon(list, icon_name(tiles_[i].icon), art, gfx::Color::rgb(0xffffff)))
            draw_icon(list, tiles_[i].icon, art);
        const gfx::Rect strip{x, y + kTileH - 54.0f, kTileW, 54.0f};
        if (focused)
            list.rounded_rect(strip, 0.0f, gfx::Color::rgb(0xffffff));
        ui::text(list, context.fonts.regular, tiles_[i].label, strip.cx(), strip.cy() + 9.0f, 24.0f,
                 focused ? gfx::Color::rgb(0x111111) : gfx::Color::rgb(0xffffff),
                 gfx::Align::center);
    }
}

std::span<const TourStep> SettingsScreen::tour() const
{
    return kTour;
}

std::unique_ptr<Screen> make_settings_screen(Context &context)
{
    return std::make_unique<SettingsScreen>(context);
}

} // namespace hui::app

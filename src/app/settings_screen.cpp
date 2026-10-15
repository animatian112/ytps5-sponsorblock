// youtube-tv - Settings screen.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/settings_screen.hpp"

#include "app/theme.hpp"
#include "yt/http.hpp"
#include "yt/log.hpp"
#include "yt/service.hpp"
#include "yt/update.hpp"
#include "ui/components.hpp"

#include <pthread.h>

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
};

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
    {1.0f, 0, Direction::none, "toggles"},
    {0.7f, hui::action_bit(Action::down), Direction::none, nullptr},
    {0.7f, hui::action_bit(Action::down), Direction::none, nullptr},
    {0.7f, hui::action_bit(Action::down), Direction::none, nullptr},
    {0.9f, 0, Direction::none, "stepper"},
};

} // namespace

SettingsScreen::SettingsScreen(Context &context) : context_(context)
{
    form_.style.theme = youtube_theme();
    form_.set_bounds({600.0f, 200.0f, 880.0f, 760.0f});

    form_.add_header("Playback");
    form_.add_toggle(kCodecAvc, "AVC (H.264)", context_.settings.codec_avc);
    form_.add_toggle(kCodecVp9, "VP9", context_.settings.codec_vp9);
    form_.add_toggle(kCodecAv1, "AV1", context_.settings.codec_av1);

    ui::FormRow &resolution = form_.add_stepper(
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

    ui::FormRow &decode = form_.add_stepper(
        kDecodeMode, "Decoding", decode_mode_to_index(context_.settings.decode_mode), 0, 2, 1);
    decode.description = decode_mode_description(context_.settings.decode_mode);
    decode.stepper.format = [](int value) -> std::string
    {
        static const char *const kLabels[] = {"Software", "Auto", "Hardware"};
        if (value < 0 || value > 2)
            return "Auto";
        return kLabels[value];
    };

    form_.add_toggle(kHaptics, "Haptics", context_.settings.haptics).description =
        "Rumble on clicks and drags";

    form_.add_header("SponsorBlock");
    form_.add_toggle(kSbEnabled, "Skip segments", context_.settings.sb_enabled).description =
        "Community-marked segments from sponsor.ajay.app";
    ui::FormRow &sb_mode =
        form_.add_stepper(kSbMode, "When a segment starts", context_.settings.sb_mode, 0, 2, 1);
    sb_mode.description = sb_mode_description(context_.settings.sb_mode);
    sb_mode.stepper.format = [](int value) -> std::string
    {
        static const char *const kLabels[] = {"Skip", "Ask", "Only mark"};
        if (value < 0 || value > 2)
            return "Skip";
        return kLabels[value];
    };
    ui::FormRow &sb_scope =
        form_.add_stepper(kSbScope, "Segments", context_.settings.sb_scope, 0, 2, 1);
    sb_scope.description = sb_scope_description(context_.settings.sb_scope);
    sb_scope.stepper.format = [](int value) -> std::string
    {
        static const char *const kLabels[] = {"Sponsors", "+ Promotion", "Everything"};
        if (value < 0 || value > 2)
            return "Everything";
        return kLabels[value];
    };

    form_.add_header("Account");
    form_.add_action(kSaveAccount, "Back up account").description =
        "Write a token copy into the app folder";
    form_.add_action(kSignOut, "Sign out");
    form_.add_header("About");
    form_.add_action(kNetTest, "Network test").description =
        "Try YouTube, Google sign-in and SponsorBlock and show what answers";
    form_.add_action(kCheckUpdates, "Check for updates").description =
        "Ask github.com/animatian112/ytps5-sponsorblock for a newer build";
}

void SettingsScreen::enter()
{
    form_.enter();
    form_.set_active(true);
    form_.focus_row(kCodecAvc);
}

void SettingsScreen::apply(int id, ui::Feedback &feedback)
{
    switch (id)
    {
    case kCodecAvc:
    case kCodecVp9:
    case kCodecAv1:
    {
        const bool on = form_.toggle_value(id);
        const int enabled = (context_.settings.codec_avc ? 1 : 0) +
                            (context_.settings.codec_vp9 ? 1 : 0) +
                            (context_.settings.codec_av1 ? 1 : 0);
        // Turning off the last enabled codec must not stick.
        if (!on && enabled <= 1)
        {
            form_.set_toggle(id, true);
            context_.toasts.push(ui::StatusKind::warning, "At least one codec must stay enabled");
            ui::play_cue(feedback, form_.style, form_.style.sounds.refuse);
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
        context_.settings.max_height = index_to_height(form_.stepper_value(id));
        context_.settings_changed = true;
        break;
    case kDecodeMode:
    {
        const yt::DecodeMode mode = index_to_decode_mode(form_.stepper_value(id));
        context_.settings.decode_mode = mode;
        if (ui::FormRow *row = form_.row(kDecodeMode))
            row->description = decode_mode_description(mode);
        context_.settings_changed = true;
        break;
    }
    case kHaptics:
        context_.settings.haptics = form_.toggle_value(id);
        context_.settings_changed = true;
        break;
    case kSbEnabled:
        context_.settings.sb_enabled = form_.toggle_value(id);
        context_.settings_changed = true;
        break;
    case kSbMode:
        context_.settings.sb_mode = form_.stepper_value(id);
        if (ui::FormRow *row = form_.row(kSbMode))
            row->description = sb_mode_description(context_.settings.sb_mode);
        context_.settings_changed = true;
        break;
    case kSbScope:
        context_.settings.sb_scope = form_.stepper_value(id);
        if (ui::FormRow *row = form_.row(kSbScope))
            row->description = sb_scope_description(context_.settings.sb_scope);
        context_.settings_changed = true;
        break;
    default:
        break;
    }
}

void SettingsScreen::activate(int id)
{
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

void SettingsScreen::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    clock_ += dt;
    switch (form_.handle(input, feedback))
    {
    case ui::Event::changed:
        apply(form_.changed_id(), feedback);
        break;
    case ui::Event::activated:
        activate(form_.changed_id());
        break;
    default:
        break;
    }
    form_.update(dt);

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

void SettingsScreen::draw(const Context &context, Frame &frame) const
{
    ui::Canvas canvas{frame.scene, context.fonts, frame.glass_texture, clock_};
    ui::text(canvas.list, context.fonts.display, "Settings", 600.0f, 150.0f, 44.0f,
             gfx::Color::rgb(0xf1f1f1));
    form_.draw(canvas);
    ui::text(canvas.list, context.fonts.regular, "YouTube for PS5 + SponsorBlock - PPSA98260",
             600.0f, 980.0f, 20.0f, gfx::Color::rgb(0x777777));
    ui::text(canvas.list, context.fonts.regular,
             "Based on ytps5. Segments: sponsor.ajay.app (CC BY-NC-SA 4.0)", 600.0f, 1012.0f, 20.0f,
             gfx::Color::rgb(0x777777));
    ui::text(canvas.list, context.fonts.regular,
             context.service.signed_in() ? "Signed in - token in /download0/hui/auth.json"
                                         : "Not signed in",
             1400.0f, 980.0f, 20.0f, gfx::Color::rgb(0x999999), gfx::Align::right);
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

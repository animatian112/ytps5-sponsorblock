// youtube-tv - Settings screen.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/settings_screen.hpp"

#include "app/theme.hpp"
#include "yt/service.hpp"
#include "yt/update.hpp"
#include "ui/components.hpp"

namespace hui::app
{
namespace
{

enum : int
{
    kCodecAvc = 1,
    kCodecVp9,
    kCodecAv1,
    kMaxHeight,
    kHaptics,
    kSaveAccount,
    kSignOut,
    kCheckUpdates,
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
    resolution.description = "Streams are capped per codec while decoding runs on the CPU";
    resolution.stepper.format = [](int value) -> std::string
    {
        static const char *const kLabels[] = {"720p", "1080p", "1440p", "2160p"};
        if (value < 0 || value > 3)
            return "1080p";
        return kLabels[value];
    };

    form_.add_toggle(kHaptics, "Haptics", context_.settings.haptics).description =
        "Rumble on clicks and drags";

    form_.add_header("Account");
    form_.add_action(kSaveAccount, "Back up account").description =
        "Write a token copy into the app folder";
    form_.add_action(kSignOut, "Sign out");
    form_.add_header("About");
    form_.add_action(kCheckUpdates, "Check for updates").description =
        "Ask the release feed for a newer version";
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
    case kHaptics:
        context_.settings.haptics = form_.toggle_value(id);
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
    ui::text(canvas.list, context.fonts.regular, "YouTube for PS5 - PPSA99350", 600.0f, 980.0f,
             20.0f, gfx::Color::rgb(0x777777));
    ui::text(canvas.list, context.fonts.regular,
             "Playback through YouTube InnerTube (TVHTML5), ad-free", 600.0f, 1012.0f, 20.0f,
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

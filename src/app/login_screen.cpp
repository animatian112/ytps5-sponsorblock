// youtube-tv - Sign-in: the OAuth2 device flow shown as a QR code and an
// eight-character TV code. The service runs the flow and its poll on a
// worker; this screen only mirrors the snapshot and uploads the QR bitmap.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/login_screen.hpp"

#include "app/theme.hpp"
#include "app/thumbs.hpp"
#include "ui/components.hpp"
#include "ui/motion.hpp"
#include "yt/auth.hpp"
#include "yt/qr.hpp"
#include "yt/service.hpp"

#include <algorithm>
#include <string>

namespace hui::app
{
namespace
{

const TourStep kTour[] = {
    {1.4f, 0, Direction::none, "code"},
    {0.8f, hui::action_bit(Action::west), Direction::none, nullptr},
};

const gfx::Color kInk = gfx::Color::rgb(0xf1f1f1);
const gfx::Color kMuted = gfx::Color::rgb(0xaaaaaa);

constexpr float kPanelX = 220.0f;
constexpr float kPanelY = 260.0f;
constexpr float kPanelSize = 560.0f;
constexpr float kColumnX = 860.0f;
// The QR is built for the panel's inner area (panel minus padding) in device
// pixels; the draw call then renders it at almost exactly this size.
constexpr int kQrPixels = 480;

} // namespace

LoginScreen::LoginScreen(Context &context) : context_(context)
{
    seen_revision_ = context_.service.login_revision();
}

void LoginScreen::enter()
{
    announced_ = false;
    const yt::LoginView view = context_.service.login();
    // Start the flow unless one is already running; the tour and the PC
    // snapshot drive the rest with scripted input.
    if (!context_.service.signed_in() && view.state != yt::Load::loading)
        context_.service.request_sign_in();
}

void LoginScreen::build_qr(const std::string &url, const std::string &user_code)
{
    // Build the bitmap at the size it will be shown: the renderer samples
    // linearly, so a one-pixel-per-module texture would come out blurry.
    const int modules = yt::qr_modules(url);
    const int quiet = 4;
    const int scale = modules > 0 ? std::max(1, kQrPixels / (modules + quiet * 2)) : 1;
    yt::QrImage qr;
    if (!yt::make_qr(url, scale, &qr) || !qr.valid())
        return;
    const std::uint32_t texture =
        context_.thumbs.upload("qr:" + user_code, qr.rgba.data(), qr.width, qr.height);
    if (texture == 0)
        return;
    qr_texture_ = texture;
    qr_width_ = qr.width;
    qr_height_ = qr.height;
    qr_key_ = user_code;
}

void LoginScreen::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    (void)feedback;
    clock_ += dt;

    const std::uint64_t revision = context_.service.login_revision();
    const yt::LoginView view = context_.service.login();
    if (revision != seen_revision_ || (qr_texture_ == 0 && !view.user_code.empty()))
    {
        seen_revision_ = revision;
        if (!view.user_code.empty() && view.user_code != qr_key_)
            build_qr(yt::prefilled_verification_url(view.verification_url, view.user_code),
                     view.user_code);
    }

    if (input.is_pressed(Action::back))
    {
        // The app decides where the screen goes back to (the player, when the
        // sign-in was started from a failed playback).
        context_.nav.cancel_login = true;
        return;
    }
    if (input.is_pressed(Action::west))
    {
        // A fresh code: drop the old QR and let the service start over.
        qr_texture_ = 0;
        qr_width_ = 0;
        qr_height_ = 0;
        qr_key_.clear();
        announced_ = false;
        context_.service.cancel_sign_in();
        context_.service.request_sign_in();
        return;
    }

    if (view.state == yt::Load::ready || context_.service.signed_in())
    {
        if (!announced_)
        {
            announced_ = true;
            context_.toasts.push(ui::StatusKind::success, "Signed in");
        }
        // No navigation here: the app returns to the screen that asked for the
        // sign-in (and a player that was waiting retries by itself).
    }
}

void LoginScreen::draw(const Context &context, Frame &frame) const
{
    ui::Canvas canvas{frame.scene, context.fonts, frame.glass_texture, clock_};
    const yt::LoginView view = context.service.login();

    // The white plate carries the QR (or a hint while it is built).
    const gfx::Rect panel{kPanelX, kPanelY, kPanelSize, kPanelSize};
    canvas.list.rounded_rect(panel, 24.0f, gfx::Color::rgb(0xffffff));
    if (qr_texture_ != 0 && qr_width_ > 0 && qr_height_ > 0)
    {
        const gfx::Rect inner = panel.inset(40.0f);
        const float scale = std::min(inner.w / static_cast<float>(qr_width_),
                                     inner.h / static_cast<float>(qr_height_));
        const float width = static_cast<float>(qr_width_) * scale;
        const float height = static_cast<float>(qr_height_) * scale;
        const gfx::Rect target{inner.x + (inner.w - width) * 0.5f,
                               inner.y + (inner.h - height) * 0.5f, width, height};
        canvas.list.image(qr_texture_, target, gfx::kFullUv, gfx::Color::rgb(0xffffff), 0.0f);
    }
    else
    {
        ui::text(canvas.list, context.fonts.regular, "Loading code...", panel.cx(), panel.cy(),
                 28.0f, gfx::Color::rgb(0x555555), gfx::Align::center);
    }

    // The right column: instruction, the code, the URL and the flow state.
    ui::text(canvas.list, context.fonts.display, "Sign in", kColumnX, 300.0f, 48.0f, kInk);
    float y = ui::paragraph(canvas.list, context.fonts.regular,
                            "Scan the code with your phone's camera: the sign-in page opens with "
                            "this code already filled in. Just pick your account and allow. "
                            "Or open the address below and type it:",
                            kColumnX, 360.0f, 24.0f, 800.0f, 34.0f, kInk);
    y += 40.0f;

    const std::string code = view.user_code.empty() ? std::string("----") : view.user_code;
    ui::text(canvas.list, context.fonts.mono, code, kColumnX, y, 64.0f, gfx::Color::rgb(0xffffff),
             gfx::Align::left, 6.0f);
    y += 44.0f;

    const std::string url =
        view.verification_url.empty() ? std::string("google.com/device") : view.verification_url;
    ui::text(canvas.list, context.fonts.regular, url, kColumnX, y, 22.0f, kMuted);
    y += 46.0f;

    if (view.state == yt::Load::loading)
    {
        ui::text(canvas.list, context.fonts.regular, "Waiting for confirmation...", kColumnX, y,
                 24.0f, kMuted);
        const float pulse = ui::breathe(clock_);
        canvas.list.circle(kColumnX + 300.0f, y - 8.0f, 6.0f + 4.0f * pulse,
                           youtube_theme().accent);
    }
    else if (view.state == yt::Load::failed)
    {
        const std::string error = view.error.empty() ? std::string("Sign-in failed") : view.error;
        ui::text(canvas.list, context.fonts.regular, error, kColumnX, y, 24.0f,
                 youtube_theme().danger);
        ui::text(canvas.list, context.fonts.regular, "Square: new code", kColumnX, y + 36.0f, 22.0f,
                 kMuted);
    }
}

std::span<const TourStep> LoginScreen::tour() const
{
    return kTour;
}

std::unique_ptr<Screen> make_login_screen(Context &context)
{
    return std::make_unique<LoginScreen>(context);
}

} // namespace hui::app

// youtube-tv - Sign-in: the OAuth2 device flow as a QR code and a TV code,
// mirrored from the service's login snapshot.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/context.hpp"
#include "app/screen.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace hui::app
{

class LoginScreen final : public Screen
{
  public:
    explicit LoginScreen(Context &context);

    const char *id() const override
    {
        return "login";
    }
    const char *title() const override
    {
        return "Sign in";
    }
    const char *hint() const override
    {
        return "Circle cancel   Options quit";
    }
    void enter() override;
    void update(const InputFrame &input, float dt, ui::Feedback &feedback) override;
    void draw(const Context &context, Frame &frame) const override;
    std::span<const TourStep> tour() const override;

  private:
    void build_qr(const std::string &url, const std::string &user_code);

    Context &context_;
    std::uint32_t qr_texture_ = 0;
    int qr_width_ = 0; // modules + quiet zone, one bitmap pixel each
    int qr_height_ = 0;
    std::string qr_key_; // the user code the texture was built for
    std::uint64_t seen_revision_ = 0;
    bool announced_ = false; // "Signed in" was toasted
    float clock_ = 0.0f;
};

} // namespace hui::app

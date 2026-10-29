// youtube-tv - The PlayStation system keyboard (libSceImeDialog).
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/system_keyboard.hpp"

#include <cstdint>

namespace hui::ps5
{

// The system's text input dialog, loaded on first use (the module, then its
// entry points by name, as videodec.cpp loads the decoder). Every step logs
// to yt.log ("ime: ..."), so a console where it does not open says why.
class ImeDialog final : public app::SystemKeyboard
{
  public:
    bool open(const std::string &title, const std::string &text, std::string *error) override;
    State poll(std::string *text) override;
    void abort() override;

  private:
    bool load(std::string *error);

    bool loaded_ = false;
    bool load_failed_ = false;
    bool open_ = false;
    // UTF-16, as the dialog reads and writes them; alive while it is open.
    static constexpr int kMaxLength = 120;
    char16_t buffer_[kMaxLength + 1] = {};
    char16_t title_[64] = {};
    char16_t placeholder_[64] = {};
};

} // namespace hui::ps5

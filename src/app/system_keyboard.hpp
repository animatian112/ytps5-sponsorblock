// youtube-tv - The console's own keyboard, when the platform has one.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Search can type with the PlayStation's system keyboard (the dialog games
// use: predictive text, voice input, a USB keyboard) instead of the
// SmartTube-style keys. The console build registers one (platform/ps5/
// ime_dialog.cpp); the PC build has none and keeps the app's own keys.

#pragma once

#include <string>

namespace hui::app
{

class SystemKeyboard
{
  public:
    enum class State
    {
        running,
        done,      // *text holds what was typed
        cancelled, // closed without Done
        failed,    // the dialog broke: use the app's own keys
    };
    virtual ~SystemKeyboard() = default;
    // Shows the dialog with `text` in it; false with *error when it cannot.
    virtual bool open(const std::string &title, const std::string &text, std::string *error) = 0;
    // Called every frame while open.
    virtual State poll(std::string *text) = 0;
    // Closes a dialog that is not answering.
    virtual void abort() = 0;
};

// The platform's keyboard (null: none).
void set_system_keyboard(SystemKeyboard *keyboard);
SystemKeyboard *system_keyboard();

} // namespace hui::app

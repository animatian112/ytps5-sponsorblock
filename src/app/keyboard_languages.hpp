// youtube-tv - The languages the on-screen keyboard types (Settings > Search).
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Each language is a layout of letters in the shape of the PlayStation's own
// keyboard. The keyboard holds the letters of every language turned on in
// Settings::keyboard_languages, then the symbols; its globe key (and L3)
// moves between the languages, R2 between the letters and the symbols.
//
// To add a language: append an entry to kLanguages in keyboard_languages.cpp
// with a function that returns its letters. Entries are never reordered or
// removed, since Settings stores them by position (a bit per entry).

#pragma once

#include "ui/components/keyboard.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace hui
{
struct Settings;
} // namespace hui

namespace hui::app
{

struct KeyboardLanguage
{
    const char *name; // in Settings ("Russian")
    const char *tag;  // on the globe key that switches to it ("RU")
    ui::KeyboardLayout (*letters)(bool with_globe);
};

std::span<const KeyboardLanguage> keyboard_languages();
// The entry whose tag this is, or -1.
int keyboard_language_index(std::string_view tag);

// The letters of each language `mask` turns on (bit i: keyboard_languages()[i];
// English when none is), followed by the symbols. With one language there is
// no globe key.
std::vector<ui::KeyboardLayout> keyboard_layouts(int mask);

// Keeps a keyboard in step with the settings: rebuilds its layouts when the
// languages change, opens on the language used last, and notes a switch in
// Settings::keyboard_language. Call once a frame, after the keyboard's
// handle(). True when it changed the settings (they should be saved).
class KeyboardLanguageSync
{
  public:
    bool sync(ui::Keyboard &keys, Settings &settings);

  private:
    int mask_ = -1; // the languages the keyboard was built with; -1: not yet
};

} // namespace hui::app

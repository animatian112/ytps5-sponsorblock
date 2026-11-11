// youtube-tv - The app's words in the viewer's language.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every interface string stays English in the code. A language's table
// (src/core/lang/<code>.inc, built in) maps each English string to its
// translation, and the fonts show the translation in its place
// (gfx::set_text_filter): a string put together at run time ("Skipped " +
// a category, a number + " views") is translated piece by piece when its
// start or end is a string of the table.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace hui::i18n
{

// Switches the app's language: "en", "es", "pt-BR", ... A code without a
// table (or "en") shows English. Returns the language now in use.
std::string set_language(std::string_view code);
// The language in use ("en" when none was set).
const std::string &language();
// The code a YouTube request names for the language ("pt" for "pt-BR").
std::string youtube_code(std::string_view code);

// A string that starts with this (an invisible word joiner) is shown as
// written: a title, a description, a comment.
inline const std::string kVerbatim = "\xE2\x81\xA0";

// The translation of one interface string, or the string itself.
std::string tr(std::string_view english);
// What the fonts show for a string: its translation, put together from
// pieces when needed. The view stays valid until the next call on the thread.
std::string_view shown(std::string_view text);

// Upper case for the scripts the fonts carry (Latin, Greek, Cyrillic).
std::string upper(std::string_view text);
// One code point's upper case (itself when it has none).
std::uint32_t upper(std::uint32_t codepoint);

} // namespace hui::i18n

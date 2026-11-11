// youtube-tv - The languages the app's own words are shown in (Settings > General).
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app is written in English; tr() looks a phrase up in the catalog
// (i18n_catalog.cpp) and returns it in the language picked, or unchanged when
// the catalog has no line for it. The English text is the key, so the code
// reads as before and a phrase nobody translated yet still shows.
//
//   ui::text(list, font, tr("Sign in"), ...);
//   toasts.push(kind, tr("Added to the sidebar"), channel.name);
//   toasts.push(kind, trf("Added to {}", playlist.title));
//
// Only the app's own words go through tr(): video titles, channel names and
// anything else YouTube sends are shown as they come (YouTube already answers
// in the language of Settings > General > Language/Country).
//
// To translate a new phrase: wrap it in tr() or trf() and add its line to the
// catalog. tests/unit/i18n_test.cpp fails while a phrase in tr() has no line,
// or a line lacks a language or a {} of the English.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

namespace hui::i18n
{

// The languages, in the catalog's column order. Settings::ui_language stores
// 1 + one of these (0 follows the console), so entries are only appended.
enum class Language : std::uint8_t
{
    english,
    italian,
    spanish,
    russian,
    chinese, // Simplified
    japanese,
};
constexpr int kLanguageCount = 6;

struct LanguageInfo
{
    const char *code;   // BCP 47 ("it", "zh-Hans")
    const char *native; // the language's own name for itself, as Settings lists it
};
const LanguageInfo &info(Language language);

// One catalog line: the English phrase and its translations, in Language
// order after English. A phrase may hold {} where trf() puts a value; every
// translation has as many as the English.
struct Entry
{
    const char *english;
    const char *translated[kLanguageCount - 1];
};
std::span<const Entry> catalog();

// The language the app shows. English until set; safe to read from any thread.
Language current();
void set_language(Language language);

// The language a console set to SceSystemParamLang `console` reads, or
// English for one the catalog has no column for.
Language from_console(int console);
// Settings::ui_language (0: follow the console) and the console's language.
Language resolve(int setting, int console);

// The phrase in the current language; `english` itself when the catalog has
// no line for it (or the language is English). The result lives as long as
// the program, like the literal passed in.
const char *tr(const char *english);
// Puts each value where the pattern has {}, in order. Extra {} stay as they
// are and extra values are dropped, so a translation can never read past the
// values it was given.
std::string format(std::string_view pattern, std::initializer_list<std::string_view> values);

template <typename... Values> std::string trf(const char *english, const Values &...values)
{
    return format(tr(english), {std::string_view(values)...});
}

// ---- dates and times, in the current language ----
// "10:34 AM" or "22:34" (zh/ja: "下午 10:34").
std::string clock_time(int hour, int minute, bool twelve_hour);
// The clock's date: "Fri Oct 16", "ven 16 ott", "10月16日 周五". weekday 0 is
// Sunday, month 1 January.
std::string short_date(int weekday, int month, int day);
// An upload date: "May 1, 2024", "1 mag 2024", "2024年5月1日".
std::string long_date(int year, int month, int day);
// How long ago, from a day count: "today", "3 days ago", "2 weeks ago" ...
// (weeks under a month, months under a year).
std::string days_ago(long long days);

} // namespace hui::i18n

// The two that every screen calls, without the namespace.
namespace hui
{
using i18n::tr;
using i18n::trf;
} // namespace hui

// youtube-tv - How old a feed item is, read off its "3 hours ago" line.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/feed_age.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <set>
#include <string>

namespace yt
{

namespace
{

struct Unit
{
    const char *word; // lower case; matched anywhere after the number
    long long seconds;
};

// Longest and most specific first: "month" before "mo", "minute" before "min".
constexpr long long kMinute = 60;
constexpr long long kHour = 60 * kMinute;
constexpr long long kDay = 24 * kHour;
constexpr Unit kUnits[] = {
    // English
    {"second", 1},
    {"minute", kMinute},
    {"hour", kHour},
    {"day", kDay},
    {"week", 7 * kDay},
    {"month", 30 * kDay},
    {"year", 365 * kDay},
    // Spanish, Portuguese, Italian
    {"segundo", 1},
    {"minuto", kMinute},
    {"hora", kHour},
    {"ora", kHour},
    {"d\xC3\xAD"
     "a",
     kDay},
    {"dia", kDay},
    {"giorn", kDay},
    {"semana", 7 * kDay},
    {"settiman", 7 * kDay},
    {"mes", 30 * kDay},
    {"m\xC3\xAAs", 30 * kDay},
    {"mese", 30 * kDay},
    {"a\xC3\xB1o", 365 * kDay},
    {"ano", 365 * kDay},
    {"anno", 365 * kDay},
    {"anni", 365 * kDay},
    // French
    {"seconde", 1},
    {"heure", kHour},
    {"jour", kDay},
    {"semaine", 7 * kDay},
    {"mois", 30 * kDay},
    {" an", 365 * kDay},
    // German, Dutch
    {"sekunde", 1},
    {"stunde", kHour},
    {"tag", kDay},
    {"woche", 7 * kDay},
    {"monat", 30 * kDay},
    {"jahr", 365 * kDay},
    {"minuut", kMinute},
    {"minuten", kMinute},
    {"uur", kHour},
    {"dag", kDay},
    {"maand", 30 * kDay},
    {"jaar", 365 * kDay},
    // Polish, Turkish
    {"sekund", 1},
    {"minut", kMinute},
    {"godzin", kHour},
    {"dzie", kDay},
    {"dni", kDay},
    {"tydzie", 7 * kDay},
    {"tygod", 7 * kDay},
    {"miesi", 30 * kDay},
    {"rok", 365 * kDay},
    {"lat", 365 * kDay},
    {"saniye", 1},
    {"dakika", kMinute},
    {"saat", kHour},
    {"g\xC3\xBCn", kDay},
    {"hafta", 7 * kDay},
    {" ay", 30 * kDay},
    {"y\xC4\xB1l", 365 * kDay},
    // Russian
    {"\xD1\x81\xD0\xB5\xD0\xBA", 1},            // сек
    {"\xD0\xBC\xD0\xB8\xD0\xBD", kMinute},      // мин
    {"\xD1\x87\xD0\xB0\xD1\x81", kHour},        // час
    {"\xD0\xB4\xD0\xBD", kDay},                 // дн
    {"\xD0\xB4\xD0\xB5\xD0\xBD\xD1\x8C", kDay}, // день
    {"\xD0\xBD\xD0\xB5\xD0\xB4", 7 * kDay},     // нед
    {"\xD0\xBC\xD0\xB5\xD1\x81", 30 * kDay},    // мес
    {"\xD0\xB3\xD0\xBE\xD0\xB4", 365 * kDay},   // год
    {"\xD0\xBB\xD0\xB5\xD1\x82", 365 * kDay},   // лет
};

// Short forms right after the number: "21y ago", "3d", "5mo", "2w", "4h".
long long short_unit(std::string_view rest)
{
    const std::string_view word = rest;
    if (word == "s")
        return 1;
    if (word == "m" || word == "min" || word == "mins")
        return kMinute;
    if (word == "h" || word == "hr" || word == "hrs")
        return kHour;
    if (word == "d")
        return kDay;
    if (word == "w" || word == "wk" || word == "wks")
        return 7 * kDay;
    if (word == "mo" || word == "mos")
        return 30 * kDay;
    if (word == "y" || word == "yr" || word == "yrs")
        return 365 * kDay;
    return 0;
}

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // The capitals the Cyrillic words above may start with (Ч, Д, Н, М, Г, Л...)
    // are rare in this line; Latin capitals are enough.
    return out;
}

} // namespace

long long age_seconds(std::string_view detail)
{
    const std::string_view dot = "\xE2\x80\xA2";
    const std::size_t at = detail.rfind(dot);
    const std::string part =
        lower(at == std::string_view::npos ? detail : detail.substr(at + dot.size()));
    // The first number in the part ("3 hours ago", "hace 3 horas"); none is
    // "1" in words ("a day ago", "an hour ago") when a unit follows.
    std::size_t digit = 0;
    while (digit < part.size() && !std::isdigit(static_cast<unsigned char>(part[digit])))
        ++digit;
    long long count = 1;
    std::size_t after = 0;
    if (digit < part.size())
    {
        count = 0;
        after = digit;
        while (after < part.size() && std::isdigit(static_cast<unsigned char>(part[after])))
            count = count * 10 + (part[after++] - '0');
    }
    std::string_view rest = std::string_view(part).substr(after);
    if (digit < part.size())
    {
        // A short form right after the number ("21y ago", "3d ago").
        std::string_view word = rest;
        while (!word.empty() && word.front() == ' ')
            word.remove_prefix(1);
        std::size_t letters = 0;
        while (letters < word.size() && std::isalpha(static_cast<unsigned char>(word[letters])))
            ++letters;
        if (const long long unit = short_unit(word.substr(0, letters)); unit > 0)
            return count * unit;
    }
    // The whole line, for languages that put the unit before the number
    // ("il y a 3 jours", "vor 2 Tagen") or after it.
    const std::string_view search = digit < part.size() ? std::string_view(part) : rest;
    // The unit that appears first wins; at the same spot, the longer word.
    std::size_t best = std::string::npos;
    std::size_t best_length = 0;
    long long seconds = 0;
    for (const Unit &unit : kUnits)
    {
        const std::size_t found = search.find(unit.word);
        const std::size_t length = std::string_view(unit.word).size();
        if (found == std::string::npos)
            continue;
        if (best == std::string::npos || found < best || (found == best && length > best_length))
        {
            best = found;
            best_length = length;
            seconds = unit.seconds;
        }
    }
    if (seconds == 0)
        return -1;
    return count * seconds;
}

void sort_newest_first(std::vector<FeedItem> *items)
{
    struct Keyed
    {
        long long age;
        bool short_video;
        std::size_t order;
        FeedItem item;
    };
    std::vector<Keyed> keyed;
    std::set<std::string> seen;
    long long previous = 0;
    for (std::size_t i = 0; i < items->size(); ++i)
    {
        FeedItem &item = (*items)[i];
        if (!item.video_id.empty() && !seen.insert(item.video_id).second)
            continue;
        long long age = age_seconds(item.detail);
        if (age < 0)
            age = previous; // a live stream or an undated card: where it was
        previous = age;
        const bool short_video = item.short_form;
        item.shelf.clear(); // one list now, not the feed's rows
        keyed.push_back({age, short_video, i, std::move(item)});
    }
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const Keyed &a, const Keyed &b)
                     {
                         if (a.short_video != b.short_video)
                             return !a.short_video;
                         return a.age < b.age;
                     });
    items->clear();
    for (Keyed &entry : keyed)
        items->push_back(std::move(entry.item));
}

} // namespace yt

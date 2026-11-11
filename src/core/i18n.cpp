// youtube-tv - The app's words in the viewer's language.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/i18n.hpp"

#include "gfx/font.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hui::i18n
{

namespace
{

// The tables: "English<TAB>translation" lines (src/core/lang/*.tsv).
struct Builtin
{
    const char *code;
    const char *table;
};

constexpr Builtin kBuiltins[] = {
    {
        "es",
#include "core/lang/es.inc"
    },
    {
        "pt-BR",
#include "core/lang/pt-BR.inc"
    },
    {
        "fr",
#include "core/lang/fr.inc"
    },
    {
        "de",
#include "core/lang/de.inc"
    },
    {
        "it",
#include "core/lang/it.inc"
    },
    {
        "ru",
#include "core/lang/ru.inc"
    },
    {
        "pl",
#include "core/lang/pl.inc"
    },
    {
        "tr",
#include "core/lang/tr.inc"
    },
    {
        "nl",
#include "core/lang/nl.inc"
    },
};

struct Table
{
    std::unordered_map<std::string, std::string> exact;
    // The same strings in capitals, for labels the theme draws in capitals.
    std::unordered_map<std::string, std::string> capitals;
    // Strings that start (prefixes) or end (suffixes) a string put together
    // at run time, longest first.
    std::vector<std::pair<std::string, std::string>> prefixes;
    std::vector<std::pair<std::string, std::string>> suffixes;
};

std::string g_language = "en";
Table g_table;
// Bumped by each switch, so each thread's cache starts over.
std::uint32_t g_generation = 1;

bool has_letter(std::string_view text)
{
    for (const char c : text)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            return true;
    }
    return false;
}

void parse(const char *blob, Table *table)
{
    std::string_view rest(blob);
    while (!rest.empty())
    {
        const std::size_t end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
        const std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos || tab == 0 || line.front() == '#')
            continue;
        std::string english(line.substr(0, tab));
        std::string translation(line.substr(tab + 1));
        if (translation.empty() || translation == english)
            continue;
        if (english.size() >= 3 && has_letter(english))
        {
            const char last = english.back();
            if (last == ' ' || last == '(')
                table->prefixes.emplace_back(english, translation);
            if (english.front() == ' ')
                table->suffixes.emplace_back(english, translation);
        }
        table->capitals.emplace(upper(english), upper(translation));
        table->exact.emplace(std::move(english), std::move(translation));
    }
    const auto longest = [](const auto &a, const auto &b)
    { return a.first.size() > b.first.size(); };
    std::sort(table->prefixes.begin(), table->prefixes.end(), longest);
    std::sort(table->suffixes.begin(), table->suffixes.end(), longest);
}

const std::string *find_exact(std::string_view text)
{
    const auto it = g_table.exact.find(std::string(text));
    return it == g_table.exact.end() ? nullptr : &it->second;
}

// The translation of the part of a string between a known start and end:
// itself when no rule fits. depth keeps "a b c" from recursing forever.
bool compose(std::string_view text, std::string *out, int depth)
{
    if (const std::string *hit = find_exact(text))
    {
        *out = *hit;
        return true;
    }
    if (depth > 2)
        return false;
    for (const auto &[english, translation] : g_table.prefixes)
    {
        if (text.size() > english.size() && text.substr(0, english.size()) == english)
        {
            std::string rest;
            if (!compose(text.substr(english.size()), &rest, depth + 1))
                rest.assign(text.substr(english.size()));
            *out = translation + rest;
            return true;
        }
    }
    for (const auto &[english, translation] : g_table.suffixes)
    {
        if (text.size() > english.size() && text.substr(text.size() - english.size()) == english)
        {
            const std::string_view head = text.substr(0, text.size() - english.size());
            std::string front;
            if (!compose(head, &front, depth + 1))
                front.assign(head);
            *out = front + translation;
            return true;
        }
    }
    return false;
}

bool all_capitals(std::string_view text)
{
    bool letter = false;
    for (const char c : text)
    {
        if (c >= 'a' && c <= 'z')
            return false;
        letter = letter || (c >= 'A' && c <= 'Z');
    }
    return letter;
}

std::string_view filter(std::string_view text)
{
    return shown(text);
}

void append_utf8(std::string *out, std::uint32_t cp)
{
    if (cp < 0x80)
    {
        out->push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800)
    {
        out->push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
    else if (cp < 0x10000)
    {
        out->push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
    else
    {
        out->push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

} // namespace

std::string set_language(std::string_view code)
{
    std::string wanted(code);
    // A language the app has no table for falls back to its family's one.
    if (wanted == "pt" || wanted == "pt-PT")
        wanted = "pt-BR";
    const Builtin *found = nullptr;
    for (const Builtin &builtin : kBuiltins)
    {
        if (wanted == builtin.code)
            found = &builtin;
    }
    if (found == nullptr)
    {
        const std::string family = wanted.substr(0, wanted.find('-'));
        for (const Builtin &builtin : kBuiltins)
        {
            if (family == builtin.code)
                found = &builtin;
        }
    }
    const std::string now = found != nullptr ? found->code : "en";
    if (now == g_language)
        return g_language;
    g_language = now;
    g_table = Table{};
    ++g_generation;
    if (found != nullptr)
    {
        parse(found->table, &g_table);
        gfx::set_text_filter(&filter);
    }
    else
    {
        gfx::set_text_filter(nullptr);
    }
    return g_language;
}

const std::string &language()
{
    return g_language;
}

std::string youtube_code(std::string_view code)
{
    if (code == "pt-BR")
        return "pt";
    return std::string(code.empty() ? std::string_view("en") : code);
}

std::string tr(std::string_view english)
{
    if (const std::string *hit = find_exact(english))
        return *hit;
    return std::string(english);
}

std::string_view shown(std::string_view text)
{
    if (g_table.exact.empty() || text.empty() || text.substr(0, kVerbatim.size()) == kVerbatim)
        return text;
    // What was shown before, per thread: most strings are drawn every frame.
    struct Cache
    {
        std::uint32_t generation = 0;
        std::unordered_map<std::string, std::string> shown; // empty: as given
    };
    // A plain pointer: the console's runtime has no destructors for
    // thread_local objects. One small cache a drawing thread, kept for good.
    thread_local Cache *cache = nullptr;
    if (cache == nullptr)
        cache = new Cache;
    if (cache->generation != g_generation || cache->shown.size() > 4096)
    {
        cache->shown.clear();
        cache->generation = g_generation;
    }
    std::string key(text);
    if (const auto it = cache->shown.find(key); it != cache->shown.end())
        return it->second.empty() ? text : std::string_view(it->second);
    std::string result;
    if (!compose(text, &result, 0) && all_capitals(text))
    {
        if (const auto it = g_table.capitals.find(key); it != g_table.capitals.end())
            result = it->second;
    }
    const auto [it, inserted] = cache->shown.emplace(std::move(key), std::move(result));
    (void)inserted;
    return it->second.empty() ? text : std::string_view(it->second);
}

std::uint32_t upper(std::uint32_t c)
{
    if (c < 0x80)
        return c >= 'a' && c <= 'z' ? c - 0x20 : c;
    if (c == 0x131) // Turkish dotless i
        return 'I';
    if ((c >= 0xe0 && c <= 0xfe && c != 0xf7))
        return c - 0x20;
    if (c == 0xff)
        return 0x178;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177))
        return (c & 1u) != 0 ? c - 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e))
        return (c & 1u) == 0 ? c - 1 : c;
    if (c == 0x3c2) // final sigma
        return 0x3a3;
    if (c >= 0x3b1 && c <= 0x3c9)
        return c - 0x20;
    if (c >= 0x430 && c <= 0x44f)
        return c - 0x20;
    if (c >= 0x450 && c <= 0x45f)
        return c - 0x50;
    return c;
}

std::string upper(std::string_view text)
{
    const bool turkish = g_language == "tr";
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size();)
    {
        const std::uint32_t c = gfx::next_codepoint(text, &index);
        append_utf8(&out, turkish && c == 'i' ? 0x130 : upper(c));
    }
    return out;
}

} // namespace hui::i18n

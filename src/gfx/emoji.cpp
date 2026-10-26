// youtube-tv - Colour emoji in text: Twemoji pictures in place of glyphs.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/emoji.hpp"

#include "gfx/font.hpp"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace hui::gfx::emoji
{

namespace
{

struct Entry
{
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct Table
{
    std::string data;
    std::size_t blob = 0; // where the pictures start
    std::vector<Entry> entries;
    std::unordered_map<std::string, int> ids; // key -> id (1-based)
    TextureFunction texture = nullptr;
};

Table &table()
{
    static Table instance;
    return instance;
}

constexpr int kMaxSequence = 10; // code points, fe0f not counted

std::uint32_t read32(const std::string &data, std::size_t at)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(data[at])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[at + 1])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[at + 2])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[at + 3])) << 24;
}

// Where an emoji may start: pictographs and symbols, regional indicators,
// and keycap bases (digits, # and *) when a keycap mark follows.
bool may_start(std::uint32_t codepoint)
{
    return codepoint == 0xa9 || codepoint == 0xae || codepoint >= 0x2000;
}

bool keycap_base(std::uint32_t codepoint)
{
    return (codepoint >= '0' && codepoint <= '9') || codepoint == '#' || codepoint == '*';
}

} // namespace

bool load(std::string data)
{
    Table &t = table();
    t.entries.clear();
    t.ids.clear();
    if (data.size() < 12 || data.compare(0, 8, "HUIEMOJ1") != 0)
        return false;
    const std::uint32_t count = read32(data, 8);
    std::size_t at = 12;
    std::vector<std::pair<std::string, Entry>> parsed;
    parsed.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (at + 2 > data.size())
            return false;
        const std::size_t length =
            static_cast<unsigned char>(data[at]) |
            static_cast<std::size_t>(static_cast<unsigned char>(data[at + 1])) << 8;
        at += 2;
        if (at + length + 8 > data.size())
            return false;
        std::string key = data.substr(at, length);
        at += length;
        Entry entry;
        entry.offset = read32(data, at);
        entry.size = read32(data, at + 4);
        at += 8;
        parsed.emplace_back(std::move(key), entry);
    }
    for (auto &[key, entry] : parsed)
    {
        if (at + entry.offset + entry.size > data.size())
            return false;
        t.entries.push_back(entry);
        t.ids.emplace(std::move(key), static_cast<int>(t.entries.size()));
    }
    t.blob = at;
    t.data = std::move(data);
    return true;
}

std::size_t count()
{
    return table().entries.size();
}

int match(std::string_view text, std::size_t at, std::size_t *consumed)
{
    const Table &t = table();
    if (t.ids.empty() || at >= text.size())
        return 0;
    std::size_t index = at;
    const std::uint32_t first = next_codepoint(text, &index);
    if (!may_start(first))
    {
        // A keycap: a digit, # or * then (fe0f and) 20e3.
        if (!keycap_base(first))
            return 0;
        std::size_t look = index;
        std::uint32_t next = look < text.size() ? next_codepoint(text, &look) : 0;
        if (next == 0xfe0f && look < text.size())
            next = next_codepoint(text, &look);
        if (next != 0x20e3)
            return 0;
    }
    // The code points from here (fe0f skipped), each with the byte offset
    // just past it, then the longest key the pack has.
    std::string key;
    char hex[16];
    std::snprintf(hex, sizeof hex, "%x", static_cast<unsigned>(first));
    key = hex;
    int best = 0;
    std::size_t best_end = 0;
    if (const auto found = t.ids.find(key); found != t.ids.end())
    {
        best = found->second;
        best_end = index;
    }
    int taken = 1;
    bool after_joiner = false;
    const bool flag = first >= 0x1f1e6 && first <= 0x1f1ff;
    while (index < text.size() && taken < kMaxSequence)
    {
        std::size_t next_index = index;
        const std::uint32_t codepoint = next_codepoint(text, &next_index);
        if (codepoint == 0xfe0f)
        {
            index = next_index;
            continue;
        }
        // What extends a sequence: anything after a joiner, the joiner and
        // the keycap mark, skin tones, tag characters (subdivision flags),
        // and the second regional indicator of a flag.
        const bool extends = after_joiner || codepoint == 0x200d || codepoint == 0x20e3 ||
                             (codepoint >= 0x1f3fb && codepoint <= 0x1f3ff) ||
                             (codepoint >= 0xe0020 && codepoint <= 0xe007f) ||
                             (flag && taken == 1 && codepoint >= 0x1f1e6 && codepoint <= 0x1f1ff);
        if (!extends)
            break;
        std::snprintf(hex, sizeof hex, "-%x", static_cast<unsigned>(codepoint));
        key += hex;
        index = next_index;
        ++taken;
        after_joiner = codepoint == 0x200d;
        if (const auto found = t.ids.find(key); found != t.ids.end())
        {
            best = found->second;
            best_end = index;
        }
    }
    if (best == 0)
        return 0;
    // A variation selector right after the match is part of it.
    if (best_end < text.size())
    {
        std::size_t after = best_end;
        if (next_codepoint(text, &after) == 0xfe0f)
            best_end = after;
    }
    *consumed = best_end - at;
    return best;
}

std::string_view png(int id)
{
    const Table &t = table();
    if (id <= 0 || static_cast<std::size_t>(id) > t.entries.size())
        return {};
    const Entry &entry = t.entries[static_cast<std::size_t>(id - 1)];
    return std::string_view(t.data).substr(t.blob + entry.offset, entry.size);
}

bool invisible(std::uint32_t codepoint)
{
    return codepoint == 0xfe0f || codepoint == 0xfe0e || codepoint == 0x200d ||
           codepoint == 0x20e3 || (codepoint >= 0x1f3fb && codepoint <= 0x1f3ff) ||
           (codepoint >= 0xe0020 && codepoint <= 0xe007f);
}

void set_texture_function(TextureFunction function)
{
    table().texture = function;
}

std::uint32_t texture(int id)
{
    const TextureFunction function = table().texture;
    return function != nullptr ? function(id) : 0u;
}

} // namespace hui::gfx::emoji

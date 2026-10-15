// youtube-tv - Watch history: the playback-statistics pings.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/watch.hpp"

#include <cstdio>

namespace yt::watch
{
namespace
{

std::string seconds_text(double seconds)
{
    if (!(seconds > 0.0))
        seconds = 0.0;
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", seconds);
    return text;
}

} // namespace

std::string make_cpn(std::uint64_t seed)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::uint64_t state = seed ^ 0x9e3779b97f4a7c15ull;
    std::string out;
    for (int i = 0; i < 16; ++i)
    {
        // splitmix64: enough spread for a nonce, no libc randomness needed.
        state += 0x9e3779b97f4a7c15ull;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        z ^= z >> 31;
        out += kAlphabet[z & 63];
    }
    return out;
}

std::string stats_url(const std::string &base,
                      const std::vector<std::pair<std::string, std::string>> &fields)
{
    const std::size_t mark = base.find('?');
    std::string out = base.substr(0, mark);
    std::string query;
    if (mark != std::string::npos)
    {
        std::size_t at = mark + 1;
        while (at <= base.size())
        {
            std::size_t end = base.find('&', at);
            if (end == std::string::npos)
                end = base.size();
            const std::string pair = base.substr(at, end - at);
            const std::string key = pair.substr(0, pair.find('='));
            bool drop = key.empty() || key == "fexp";
            for (const auto &field : fields)
                drop = drop || key == field.first;
            if (!drop)
            {
                query += query.empty() ? "" : "&";
                query += pair;
            }
            at = end + 1;
        }
    }
    for (const auto &field : fields)
    {
        query += query.empty() ? "" : "&";
        query += field.first + "=" + field.second;
    }
    return query.empty() ? out : out + "?" + query;
}

std::string playback_ping(const std::string &playback_url, const std::string &cpn)
{
    return stats_url(playback_url, {{"ver", "2"}, {"cpn", cpn}});
}

std::string watchtime_ping(const std::string &watchtime_url, const std::string &cpn, double from_s,
                           double to_s, double length_s)
{
    return stats_url(watchtime_url, {{"ver", "2"},
                                     {"cpn", cpn},
                                     {"st", seconds_text(from_s)},
                                     {"et", seconds_text(to_s)},
                                     {"cmt", seconds_text(to_s)},
                                     {"len", seconds_text(length_s)},
                                     {"state", "playing"}});
}

} // namespace yt::watch

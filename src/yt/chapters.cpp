// youtube-tv - Chapters from a video description's timestamps.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/chapters.hpp"

#include <string>

namespace yt
{
namespace
{

bool is_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

// Reads "m:ss" or "h:mm:ss" at `at`; returns the length read (0 for none).
std::size_t read_time(std::string_view text, std::size_t at, double *seconds)
{
    std::size_t cursor = at;
    double total = 0.0;
    int parts = 0;
    while (parts < 3)
    {
        const std::size_t start = cursor;
        double value = 0.0;
        while (cursor < text.size() && is_digit(text[cursor]) && cursor - start < 3)
            value = value * 10.0 + (text[cursor++] - '0');
        if (cursor == start)
            return 0;
        // Every part after the first is two digits.
        if (parts > 0 && cursor - start != 2)
            return 0;
        total = total * 60.0 + value;
        ++parts;
        if (cursor < text.size() && text[cursor] == ':' && cursor + 1 < text.size() &&
            is_digit(text[cursor + 1]))
            ++cursor;
        else
            break;
    }
    if (parts < 2)
        return 0;
    *seconds = total;
    return cursor - at;
}

std::string trim_title(std::string_view text)
{
    static constexpr std::string_view kEdge = " \t\r-\xE2\x80\x93\xE2\x80\x94|:.)]>";
    std::size_t start = 0;
    std::size_t end = text.size();
    while (start < end && kEdge.find(text[start]) != std::string_view::npos)
        ++start;
    while (end > start &&
           std::string_view(" \t\r-|:(").find(text[end - 1]) != std::string_view::npos)
        --end;
    return std::string(text.substr(start, end - start));
}

} // namespace

std::vector<Chapter> parse_chapters(std::string_view description)
{
    std::vector<Chapter> chapters;
    std::size_t at = 0;
    while (at < description.size())
    {
        std::size_t end = description.find('\n', at);
        if (end == std::string_view::npos)
            end = description.size();
        const std::string_view line = description.substr(at, end - at);
        at = end + 1;
        // The first timestamp of the line, at its start or after a bracket.
        for (std::size_t i = 0; i < line.size(); ++i)
        {
            if (!is_digit(line[i]) || (i > 0 && (is_digit(line[i - 1]) || line[i - 1] == ':')))
                continue;
            double seconds = 0.0;
            const std::size_t length = read_time(line, i, &seconds);
            if (length == 0)
                continue;
            std::string title = trim_title(line.substr(i + length));
            if (title.empty())
                title = trim_title(line.substr(0, i));
            if (!chapters.empty() && seconds <= chapters.back().start_s)
                break; // not rising: a list of something else
            chapters.push_back({seconds, title.empty() ? std::string("Chapter") : title});
            break;
        }
    }
    // YouTube's own rule: from 0:00, three or more.
    if (chapters.size() < 3 || chapters.front().start_s != 0.0)
        return {};
    return chapters;
}

int chapter_at(const std::vector<Chapter> &chapters, double position)
{
    int index = -1;
    for (std::size_t i = 0; i < chapters.size(); ++i)
    {
        if (chapters[i].start_s <= position + 0.25)
            index = static_cast<int>(i);
        else
            break;
    }
    return index;
}

} // namespace yt

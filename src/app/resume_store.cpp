// youtube-tv - Where each video was left.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/resume_store.hpp"

#include "core/save_file.hpp"

#include <cstdio>
#include <cstdlib>

namespace hui::app
{

ResumeStore::ResumeStore(std::string path) : path_(std::move(path))
{
    std::string text;
    if (!path_.empty() && save::read_file(path_, &text, 64u << 10))
        entries_ = parse(text);
    if (!path_.empty())
    {
        const std::size_t slash = path_.find_last_of('/');
        speeds_path_ = (slash == std::string::npos ? std::string() : path_.substr(0, slash + 1)) +
                       "channel_speeds.txt";
        text.clear();
        if (save::read_file(speeds_path_, &text, 64u << 10))
        {
            std::size_t at = 0;
            while (at < text.size() && speeds_.size() < 500)
            {
                std::size_t end = text.find('\n', at);
                if (end == std::string::npos)
                    end = text.size();
                const std::string line = text.substr(at, end - at);
                const std::size_t space = line.find(' ');
                if (space != std::string::npos && space > 0)
                {
                    const double speed = std::strtod(line.c_str() + space + 1, nullptr);
                    if (speed >= 0.25 && speed <= 4.0)
                        speeds_.emplace_back(line.substr(0, space), speed);
                }
                at = end + 1;
            }
        }
    }
}

double ResumeStore::channel_speed(const std::string &channel_id) const
{
    for (const auto &entry : speeds_)
        if (entry.first == channel_id)
            return entry.second;
    return 0.0;
}

void ResumeStore::remember_channel_speed(const std::string &channel_id, double speed)
{
    if (channel_id.empty() || channel_id.find_first_of(" \n") != std::string::npos)
        return;
    for (std::size_t i = 0; i < speeds_.size(); ++i)
        if (speeds_[i].first == channel_id)
        {
            speeds_.erase(speeds_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    if (speed > 0.0)
        speeds_.insert(speeds_.begin(), {channel_id, speed});
    if (speeds_.size() > 500)
        speeds_.resize(500);
    if (!speeds_path_.empty())
    {
        std::string out;
        char number[32];
        for (const auto &entry : speeds_)
        {
            std::snprintf(number, sizeof(number), "%.2f", entry.second);
            out += entry.first + " " + number + "\n";
        }
        (void)save::write_atomic(speeds_path_, out);
    }
}

std::vector<std::pair<std::string, double>> ResumeStore::parse(const std::string &text)
{
    std::vector<std::pair<std::string, double>> out;
    std::size_t at = 0;
    while (at < text.size() && out.size() < kMaxEntries)
    {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(at, end - at);
        const std::size_t space = line.find(' ');
        if (space == 11)
        {
            const double seconds = std::strtod(line.c_str() + space + 1, nullptr);
            if (seconds > 0.0)
                out.emplace_back(line.substr(0, space), seconds);
        }
        at = end + 1;
    }
    return out;
}

std::string ResumeStore::format(const std::vector<std::pair<std::string, double>> &entries)
{
    std::string out;
    char number[32];
    for (const auto &entry : entries)
    {
        std::snprintf(number, sizeof(number), "%.1f", entry.second);
        out += entry.first + " " + number + "\n";
    }
    return out;
}

double ResumeStore::position(const std::string &video_id) const
{
    for (const auto &entry : entries_)
    {
        if (entry.first == video_id)
            return entry.second;
    }
    return 0.0;
}

void ResumeStore::remember(const std::string &video_id, double position_s, double length_s)
{
    if (video_id.size() != 11)
        return;
    for (std::size_t i = 0; i < entries_.size(); ++i)
    {
        if (entries_[i].first == video_id)
        {
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    // Barely started, or watched to the end: start over next time.
    const bool keep = position_s >= 10.0 && (length_s <= 0.0 || position_s < length_s - 15.0);
    if (keep)
        entries_.insert(entries_.begin(), {video_id, position_s});
    if (entries_.size() > kMaxEntries)
        entries_.resize(kMaxEntries);
    if (!path_.empty())
        (void)save::write_atomic(path_, format(entries_));
}

} // namespace hui::app

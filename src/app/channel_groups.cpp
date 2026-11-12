// youtube-tv - Channel groups.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/channel_groups.hpp"

#include <algorithm>

namespace hui::app
{
namespace
{

// Tabs and line breaks would break the file's lines; leading and trailing
// spaces only make two names that look the same differ. Long names are cut
// on a character boundary so a UTF-8 sequence is never split.
std::string clean_name(std::string_view name, std::size_t max_bytes)
{
    std::string out(name);
    for (char &c : out)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    const std::size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    const std::size_t last = out.find_last_not_of(' ');
    out = out.substr(first, last - first + 1);
    if (out.size() > max_bytes)
    {
        std::size_t cut = max_bytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;
        out.resize(cut);
    }
    return out;
}

// ASCII only: enough to keep "games" and "Games" from becoming two groups;
// other scripts compare as written.
bool same_name(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z')
            x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = static_cast<char>(y - 'A' + 'a');
        if (x != y)
            return false;
    }
    return true;
}

bool valid_id(std::string_view id)
{
    return id.size() == 24 && id[0] == 'U' && id[1] == 'C';
}

// A card from the Subscriptions feed usually carries its channel's id, but
// not always (the id comes from the card's menu): then the channel's name is
// what is left to match on. Two different ids never match, even with the
// same name, so two channels called "News" stay apart when both are known.
bool same_channel(const ChannelGroups::Channel &a, const ChannelGroups::Channel &b)
{
    if (!a.id.empty() && !b.id.empty())
        return a.id == b.id;
    return !a.name.empty() && a.name == b.name;
}

} // namespace

ChannelGroups::Channel channel_of(const yt::FeedItem &item)
{
    if (item.is_channel())
        return {item.video_id, item.title};
    return {valid_id(item.channel_id) ? item.channel_id : std::string(), item.author};
}

int ChannelGroups::add_group(std::string_view name)
{
    const std::string clean = clean_name(name, kMaxNameBytes);
    if (clean.empty())
        return -1;
    for (std::size_t i = 0; i < groups_.size(); ++i)
        if (same_name(groups_[i].name, clean))
            return static_cast<int>(i);
    if (groups_.size() >= kMaxGroups)
        return -1;
    groups_.push_back({clean, {}});
    ++revision_;
    return static_cast<int>(groups_.size() - 1);
}

void ChannelGroups::remove_group(std::size_t index)
{
    if (index >= groups_.size())
        return;
    groups_.erase(groups_.begin() + static_cast<std::ptrdiff_t>(index));
    ++revision_;
}

bool ChannelGroups::toggle(std::size_t group, const Channel &channel)
{
    if (group >= groups_.size() || (channel.id.empty() && channel.name.empty()))
        return false;
    std::vector<Channel> &channels = groups_[group].channels;
    const auto found = std::find_if(channels.begin(), channels.end(), [&](const Channel &member)
                                    { return same_channel(member, channel); });
    ++revision_;
    if (found != channels.end())
    {
        channels.erase(found);
        if (channels.empty())
            groups_.erase(groups_.begin() + static_cast<std::ptrdiff_t>(group));
        return false;
    }
    if (channels.size() >= kMaxChannels)
        return false;
    channels.push_back(
        {valid_id(channel.id) ? channel.id : std::string(), clean_name(channel.name, 200)});
    return true;
}

bool ChannelGroups::contains(std::size_t group, const Channel &channel) const
{
    if (group >= groups_.size())
        return false;
    const std::vector<Channel> &channels = groups_[group].channels;
    return std::any_of(channels.begin(), channels.end(),
                       [&](const Channel &member) { return same_channel(member, channel); });
}

std::vector<yt::FeedItem> ChannelGroups::filter(const std::vector<yt::FeedItem> &items,
                                                std::size_t group) const
{
    std::vector<yt::FeedItem> out;
    if (group >= groups_.size())
        return out;
    for (const yt::FeedItem &item : items)
        if (contains(group, channel_of(item)))
            out.push_back(item);
    return out;
}

std::string ChannelGroups::serialize() const
{
    std::string data;
    for (const Group &group : groups_)
    {
        data += "# " + group.name + '\n';
        for (const Channel &channel : group.channels)
            data += channel.id + '\t' + channel.name + '\n';
    }
    return data;
}

void ChannelGroups::parse(std::string_view data)
{
    groups_.clear();
    ++revision_;
    std::size_t at = 0;
    while (at < data.size())
    {
        std::size_t end = data.find('\n', at);
        if (end == std::string_view::npos)
            end = data.size();
        std::string_view line = data.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.size() > 2 && line.substr(0, 2) == "# ")
        {
            const std::string name = clean_name(line.substr(2), kMaxNameBytes);
            if (!name.empty() && groups_.size() < kMaxGroups)
                groups_.push_back({name, {}});
            else
                groups_.push_back({}); // its channels are read and dropped below
            continue;
        }
        if (groups_.empty())
            continue; // channels before any group: a damaged file
        const std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos)
            continue;
        Channel channel{std::string(line.substr(0, tab)), clean_name(line.substr(tab + 1), 200)};
        if (!channel.id.empty() && !valid_id(channel.id))
            continue;
        if (channel.id.empty() && channel.name.empty())
            continue;
        std::vector<Channel> &channels = groups_.back().channels;
        if (channels.size() < kMaxChannels)
            channels.push_back(std::move(channel));
    }
    // A group with no name (over the limit) or no channels is not kept: an
    // empty group could never be removed from the card menu.
    groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [](const Group &group)
                                 { return group.name.empty() || group.channels.empty(); }),
                  groups_.end());
}

} // namespace hui::app

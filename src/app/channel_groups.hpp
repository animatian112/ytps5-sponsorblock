// youtube-tv - Channel groups: the viewer's own folders of subscribed channels.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Someone subscribed to hundreds of channels sees them all mixed in one
// Subscriptions feed (issue #18). A group ("Science", "Games") is a named
// list of channels the viewer picks from a card's menu; Subscriptions then
// shows only that group's videos, still newest first, because the feed
// YouTube sends is already in upload order and the filter keeps that order.
//
// Groups live on the console only (<data>/channel_groups.txt): YouTube has no
// such thing to sync with, and nothing about them is sent anywhere.

#pragma once

#include "yt/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hui::app
{

class ChannelGroups
{
  public:
    struct Channel
    {
        std::string id;   // "UC...", 24 characters
        std::string name; // as the card showed it: the match when a card has no id
    };
    struct Group
    {
        std::string name;
        std::vector<Channel> channels;
    };

    // Enough for a row of tabs that still fits on screen, and a file that
    // stays small however long the console is used.
    static constexpr std::size_t kMaxGroups = 20;
    static constexpr std::size_t kMaxChannels = 500; // in one group
    static constexpr std::size_t kMaxNameBytes = 60;

    const std::vector<Group> &groups() const
    {
        return groups_;
    }
    bool empty() const
    {
        return groups_.empty();
    }
    // Changes on every edit, so a screen showing a filtered feed knows to
    // filter again.
    std::uint64_t revision() const
    {
        return revision_;
    }

    // Adds a group and returns its index, or the index of the group that
    // already has this name (case is ignored). -1 for an empty name or when
    // kMaxGroups are in use.
    int add_group(std::string_view name);
    void remove_group(std::size_t index);

    // Puts the channel in the group, or takes it out when it is there.
    // Returns whether it is in the group now. A group whose last channel is
    // taken out goes too: there is no separate "delete group" to find.
    bool toggle(std::size_t group, const Channel &channel);
    bool contains(std::size_t group, const Channel &channel) const;

    // The items of `items` whose channel is in the group, in their order.
    std::vector<yt::FeedItem> filter(const std::vector<yt::FeedItem> &items,
                                     std::size_t group) const;

    // One group a line ("# name"), then its channels ("UC...<TAB>name").
    std::string serialize() const;
    void parse(std::string_view data);

  private:
    std::vector<Group> groups_;
    std::uint64_t revision_ = 0;
};

// The channel a card belongs to: its own id for a channel card, else the
// channel the feed named for it.
ChannelGroups::Channel channel_of(const yt::FeedItem &item);

} // namespace hui::app

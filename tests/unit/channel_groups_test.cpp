// youtube-tv - Channel groups: picking, filtering and the file they live in.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/channel_groups.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{

using hui::app::channel_of;
using hui::app::ChannelGroups;

const std::string kScience = "UCscienceaaaaaaaaaaaaaaa";
const std::string kGames = "UCgamesbbbbbbbbbbbbbbbbb";
const std::string kNews = "UCnewscccccccccccccccccc";

yt::FeedItem video(const std::string &id, const std::string &channel_id, const std::string &author)
{
    yt::FeedItem item;
    item.video_id = id;
    item.channel_id = channel_id;
    item.author = author;
    return item;
}

TEST(ChannelGroups, AddingANameTwiceGivesTheSameGroup)
{
    ChannelGroups groups;
    EXPECT_EQ(groups.add_group("Science"), 0);
    EXPECT_EQ(groups.add_group("  science "), 0);
    EXPECT_EQ(groups.add_group("Games"), 1);
    EXPECT_EQ(groups.add_group("   "), -1);
    ASSERT_EQ(groups.groups().size(), 2u);
    EXPECT_EQ(groups.groups()[0].name, "Science");
}

TEST(ChannelGroups, NoMoreThanTheLimit)
{
    ChannelGroups groups;
    for (std::size_t i = 0; i < ChannelGroups::kMaxGroups; ++i)
        EXPECT_GE(groups.add_group("Group " + std::to_string(i)), 0);
    EXPECT_EQ(groups.add_group("One too many"), -1);
}

TEST(ChannelGroups, ToggleAddsThenRemovesAndAnEmptyGroupGoes)
{
    ChannelGroups groups;
    const int science = groups.add_group("Science");
    const std::uint64_t before = groups.revision();
    EXPECT_TRUE(groups.toggle(static_cast<std::size_t>(science), {kScience, "Veritasium"}));
    EXPECT_GT(groups.revision(), before);
    EXPECT_TRUE(groups.contains(0, {kScience, ""}));
    EXPECT_FALSE(groups.toggle(0, {kScience, "Veritasium"}));
    // Its last channel gone, the group has nothing left to show.
    EXPECT_TRUE(groups.empty());
}

TEST(ChannelGroups, FilterKeepsTheFeedsOrder)
{
    ChannelGroups groups;
    groups.add_group("Science");
    groups.toggle(0, {kScience, "Veritasium"});
    groups.toggle(0, {kNews, "Daily News"});
    // The Subscriptions feed comes newest first.
    const std::vector<yt::FeedItem> feed = {
        video("aaaaaaaaaaa", kNews, "Daily News"),
        video("bbbbbbbbbbb", kGames, "Game Channel"),
        video("ccccccccccc", kScience, "Veritasium"),
        video("ddddddddddd", kNews, "Daily News"),
    };
    const std::vector<yt::FeedItem> shown = groups.filter(feed, 0);
    ASSERT_EQ(shown.size(), 3u);
    EXPECT_EQ(shown[0].video_id, "aaaaaaaaaaa");
    EXPECT_EQ(shown[1].video_id, "ccccccccccc");
    EXPECT_EQ(shown[2].video_id, "ddddddddddd");
    EXPECT_TRUE(groups.filter(feed, 5).empty());
}

TEST(ChannelGroups, ACardWithoutAnIdMatchesByName)
{
    ChannelGroups groups;
    groups.add_group("Science");
    groups.toggle(0, {kScience, "Veritasium"});
    // The id comes from the card's menu and is sometimes missing.
    EXPECT_TRUE(groups.contains(0, channel_of(video("aaaaaaaaaaa", "", "Veritasium"))));
    // Two known ids never match, whatever the names.
    EXPECT_FALSE(groups.contains(0, channel_of(video("bbbbbbbbbbb", kNews, "Veritasium"))));
}

TEST(ChannelGroups, AChannelCardIsItsOwnChannel)
{
    yt::FeedItem card;
    card.video_id = kGames;
    card.title = "Game Channel";
    const ChannelGroups::Channel channel = channel_of(card);
    EXPECT_EQ(channel.id, kGames);
    EXPECT_EQ(channel.name, "Game Channel");
}

TEST(ChannelGroups, TheFileReadsBackTheSame)
{
    ChannelGroups groups;
    groups.add_group("Science");
    groups.toggle(0, {kScience, "Veritasium"});
    groups.toggle(0, {"", "No Id\tChannel"}); // a tab would split the line
    groups.add_group("Games");
    groups.toggle(1, {kGames, "Game Channel"});

    ChannelGroups loaded;
    loaded.parse(groups.serialize());
    ASSERT_EQ(loaded.groups().size(), 2u);
    EXPECT_EQ(loaded.groups()[0].name, "Science");
    ASSERT_EQ(loaded.groups()[0].channels.size(), 2u);
    EXPECT_EQ(loaded.groups()[0].channels[1].name, "No Id Channel");
    EXPECT_EQ(loaded.groups()[1].channels[0].id, kGames);
    EXPECT_EQ(loaded.serialize(), groups.serialize());
}

TEST(ChannelGroups, ADamagedFileKeepsWhatItCan)
{
    ChannelGroups groups;
    groups.parse("UCorphanxxxxxxxxxxxxxxxx\tBefore any group\r\n"
                 "# Science\r\n"
                 "not-an-id\tBroken\r\n"
                 "UCscienceaaaaaaaaaaaaaaa\tVeritasium\r\n"
                 "# Empty\n"
                 "# \n"
                 "no tab here\n");
    ASSERT_EQ(groups.groups().size(), 1u);
    EXPECT_EQ(groups.groups()[0].name, "Science");
    ASSERT_EQ(groups.groups()[0].channels.size(), 1u);
    EXPECT_EQ(groups.groups()[0].channels[0].id, kScience);
}

TEST(ChannelGroups, LongNamesAreCutOnACharacterBoundary)
{
    ChannelGroups groups;
    std::string name;
    for (int i = 0; i < 40; ++i)
        name += "\xC3\xA9"; // é, two bytes: 80 bytes in all
    ASSERT_EQ(groups.add_group(name), 0);
    const std::string &kept = groups.groups()[0].name;
    EXPECT_LE(kept.size(), ChannelGroups::kMaxNameBytes);
    EXPECT_EQ(kept.size() % 2, 0u); // no half of an é at the end
}

} // namespace

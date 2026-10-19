// youtube-tv - Comments, vote counts and the search filter parameter.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/comments.hpp"
#include "yt/innertube.hpp"

#include <gtest/gtest.h>

TEST(Comments, FindsTheSectionToken)
{
    const char *page =
        R"({"contents":{"twoColumnWatchNextResults":{"results":{"results":{"contents":[
      {"itemSectionRenderer":{"sectionIdentifier":"comment-item-section","contents":[
        {"continuationItemRenderer":{"continuationEndpoint":{"continuationCommand":{"token":"FIRST"}}}}]}}]}}}}})";
    EXPECT_EQ(yt::comments_token(page), "FIRST");
    const char *panel = R"({"engagementPanels":[{"engagementPanelSectionListRenderer":{
      "panelIdentifier":"engagement-panel-comments-section","content":{"sectionListRenderer":{
        "contents":[{"continuationItemRenderer":{"continuationEndpoint":{"continuationCommand":{"token":"PANEL"}}}}]}}}}]})";
    EXPECT_EQ(yt::comments_token(panel), "PANEL");
    EXPECT_EQ(yt::comments_token(R"({"contents":{}})"), "");
}

TEST(Comments, ReadsBothWordingsAndTheNextPage)
{
    const char *entities = R"({"onResponseReceivedEndpoints":[
      {"reloadContinuationItemsCommand":{"continuationItems":[{"commentsHeaderRenderer":{}}]}},
      {"reloadContinuationItemsCommand":{"continuationItems":[
        {"commentThreadRenderer":{"replies":{"commentRepliesRenderer":{"contents":[
          {"continuationItemRenderer":{"continuationEndpoint":{"continuationCommand":{"token":"REPLIES"}}}}]}}}},
        {"continuationItemRenderer":{"continuationEndpoint":{"continuationCommand":{"token":"NEXT"}}}}]}}],
      "frameworkUpdates":{"entityBatchUpdate":{"mutations":[
        {"payload":{"commentEntityPayload":{"properties":{"content":{"content":"Great video"},
          "publishedTime":"2 days ago"},"author":{"displayName":"@someone"},
          "toolbar":{"likeCountNotliked":"1.2K","replyCount":"3"}}}},
        {"payload":{"engagementToolbarStateEntityPayload":{}}}]}}})";
    std::vector<yt::Comment> comments;
    std::string next;
    ASSERT_TRUE(yt::parse_comments(entities, &comments, &next));
    ASSERT_EQ(comments.size(), 1u);
    EXPECT_EQ(comments[0].author, "@someone");
    EXPECT_EQ(comments[0].text, "Great video");
    EXPECT_EQ(comments[0].published, "2 days ago");
    EXPECT_EQ(comments[0].likes, "1.2K");
    EXPECT_EQ(comments[0].replies, "3");
    EXPECT_EQ(next, "NEXT");

    const char *renderers = R"({"onResponseReceivedEndpoints":[{"appendContinuationItemsAction":{
      "continuationItems":[{"commentThreadRenderer":{"comment":{"commentRenderer":{
        "authorText":{"simpleText":"Ann"},"contentText":{"runs":[{"text":"First "},{"text":"line"}]},
        "publishedTimeText":{"runs":[{"text":"1 year ago"}]},"voteCount":{"simpleText":"7"}}},
        "replies":{"commentRepliesRenderer":{"contents":[{"commentRenderer":{
          "contentText":{"simpleText":"a reply"}}}]}}}}]}}]})";
    ASSERT_TRUE(yt::parse_comments(renderers, &comments, &next));
    ASSERT_EQ(comments.size(), 1u) << "replies are not top-level comments";
    EXPECT_EQ(comments[0].author, "Ann");
    EXPECT_EQ(comments[0].text, "First line");
    EXPECT_EQ(comments[0].likes, "7");
    EXPECT_EQ(next, "");
}

TEST(Comments, ReadsVoteCountsAndShortensNumbers)
{
    long long likes = 0;
    long long dislikes = 0;
    ASSERT_TRUE(yt::parse_votes(R"({"id":"x","likes":12345,"dislikes":678,"viewCount":9})", &likes,
                                &dislikes));
    EXPECT_EQ(likes, 12345);
    EXPECT_EQ(dislikes, 678);
    EXPECT_FALSE(yt::parse_votes(R"({"title":"Not Found"})", &likes, &dislikes));
    EXPECT_EQ(yt::compact_count(678), "678");
    EXPECT_EQ(yt::compact_count(1000), "1K");
    EXPECT_EQ(yt::compact_count(1234), "1.2K");
    EXPECT_EQ(yt::compact_count(12345), "12K");
    EXPECT_EQ(yt::compact_count(3400000), "3.4M");
}

TEST(Search, FilterParamsMatchYouTubes)
{
    EXPECT_EQ(yt::search_params(0, 0, 0), "");
    EXPECT_EQ(yt::search_params(1, 0, 0), "CAI=");             // sort by upload date
    EXPECT_EQ(yt::search_params(2, 0, 0), "CAM=");             // by view count
    EXPECT_EQ(yt::search_params(0, 3, 0), "EgQIAxAB");         // this week, videos
    EXPECT_EQ(yt::search_params(0, 0, 1), "EgQQARgB");         // under 4 minutes, videos
    EXPECT_EQ(yt::search_params(1, 2, 3), "CAISBggCEAEYAg=="); // date, today, over 20 min
}

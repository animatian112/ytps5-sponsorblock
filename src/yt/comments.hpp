// youtube-tv - A video's comments (read only) and its like/dislike counts.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Comments come from the web client's /next: the watch page names the
// comment section's continuation token, and each continuation answer carries
// a page of comments plus the token of the next page. YouTube words a comment
// two ways (the older commentRenderer, the newer commentEntityPayload in the
// framework updates); both are read.
//
// The counts come from Return YouTube Dislike (returnyoutubedislikeapi.com),
// which keeps the dislike counts YouTube stopped publishing.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace yt
{

struct Comment
{
    std::string author;
    std::string text;
    std::string published; // "2 days ago"
    std::string likes;     // "1.2K", empty for none
    std::string replies;   // "12", empty for none
};

// The comment section's first token in a /next answer (empty: none).
std::string comments_token(std::string_view next_json);

// The comments of a continuation answer, and the next page's token.
bool parse_comments(std::string_view json, std::vector<Comment> *out, std::string *next);

// {"likes":123,"dislikes":45,...} -> counts; false when the answer is not one.
bool parse_votes(std::string_view json, long long *likes, long long *dislikes);

// 1234 -> "1.2K", 3400000 -> "3.4M".
std::string compact_count(long long count);

} // namespace yt

// youtube-tv - A video's comments (read only) and its like/dislike counts.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/comments.hpp"

#include "third_party/yyjson/yyjson.h"

#include <cstdio>
#include <cstring>

namespace yt
{
namespace
{

std::string str(yyjson_val *value)
{
    return yyjson_is_str(value) ? std::string(yyjson_get_str(value)) : std::string();
}

// simpleText, runs or a view model's "content".
std::string text(yyjson_val *value)
{
    if (yyjson_is_str(value))
        return yyjson_get_str(value);
    if (!yyjson_is_obj(value))
        return {};
    if (yyjson_val *simple = yyjson_obj_get(value, "simpleText"); yyjson_is_str(simple))
        return yyjson_get_str(simple);
    if (yyjson_val *content = yyjson_obj_get(value, "content"); yyjson_is_str(content))
        return yyjson_get_str(content);
    std::string out;
    yyjson_val *runs = yyjson_obj_get(value, "runs");
    for (std::size_t i = 0, n = yyjson_arr_size(runs); i < n; ++i)
        out += str(yyjson_obj_get(yyjson_arr_get(runs, i), "text"));
    return out;
}

yyjson_val *path(yyjson_val *value, std::initializer_list<const char *> keys)
{
    for (const char *key : keys)
        value = yyjson_obj_get(value, key);
    return value;
}

// The continuationCommand token anywhere under value (first one met).
std::string first_token(yyjson_val *value, int depth = 0)
{
    if (depth > 30 || value == nullptr)
        return {};
    if (yyjson_is_obj(value))
    {
        if (yyjson_val *command = yyjson_obj_get(value, "continuationCommand"))
            if (std::string token = str(yyjson_obj_get(command, "token")); !token.empty())
                return token;
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(value, &iter);
        yyjson_val *key = nullptr;
        while ((key = yyjson_obj_iter_next(&iter)) != nullptr)
            if (std::string token = first_token(yyjson_obj_iter_get_val(key), depth + 1);
                !token.empty())
                return token;
    }
    else if (yyjson_is_arr(value))
    {
        for (std::size_t i = 0, n = yyjson_arr_size(value); i < n; ++i)
            if (std::string token = first_token(yyjson_arr_get(value, i), depth + 1);
                !token.empty())
                return token;
    }
    return {};
}

template <typename Visit> void walk(yyjson_val *value, Visit &&visit, int depth = 0)
{
    if (depth > 40 || value == nullptr)
        return;
    if (yyjson_is_obj(value))
    {
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(value, &iter);
        yyjson_val *key = nullptr;
        while ((key = yyjson_obj_iter_next(&iter)) != nullptr)
        {
            yyjson_val *child = yyjson_obj_iter_get_val(key);
            if (!visit(yyjson_get_str(key), child))
                walk(child, visit, depth + 1);
        }
    }
    else if (yyjson_is_arr(value))
        for (std::size_t i = 0, n = yyjson_arr_size(value); i < n; ++i)
            walk(yyjson_arr_get(value, i), visit, depth + 1);
}

} // namespace

std::string comments_token(std::string_view next_json)
{
    yyjson_doc *doc = yyjson_read(next_json.data(), next_json.size(), 0);
    if (doc == nullptr)
        return {};
    std::string token;
    // The comment item section of the watch page, else the comments
    // engagement panel.
    walk(yyjson_doc_get_root(doc),
         [&](const char *key, yyjson_val *child)
         {
             if (!token.empty())
                 return true;
             if (std::strcmp(key, "itemSectionRenderer") == 0 &&
                 str(yyjson_obj_get(child, "sectionIdentifier")) == "comment-item-section")
             {
                 token = first_token(child);
                 return true;
             }
             return false;
         });
    if (token.empty())
        walk(yyjson_doc_get_root(doc),
             [&](const char *key, yyjson_val *child)
             {
                 if (!token.empty())
                     return true;
                 if (std::strcmp(key, "engagementPanelSectionListRenderer") != 0)
                     return false;
                 const std::string id = str(yyjson_obj_get(child, "panelIdentifier")) +
                                        str(yyjson_obj_get(child, "targetId"));
                 if (id.find("comment") != std::string::npos)
                     token = first_token(yyjson_obj_get(child, "content"));
                 return true;
             });
    yyjson_doc_free(doc);
    return token;
}

bool parse_comments(std::string_view json, std::vector<Comment> *out, std::string *next)
{
    if (out == nullptr)
        return false;
    out->clear();
    if (next != nullptr)
        next->clear();
    yyjson_doc *doc = yyjson_read(json.data(), json.size(), 0);
    if (doc == nullptr)
        return false;
    yyjson_val *root = yyjson_doc_get_root(doc);

    // The newer wording: one entity per comment in the framework updates.
    yyjson_val *mutations = path(root, {"frameworkUpdates", "entityBatchUpdate", "mutations"});
    for (std::size_t i = 0, n = yyjson_arr_size(mutations); i < n; ++i)
    {
        yyjson_val *entity =
            path(yyjson_arr_get(mutations, i), {"payload", "commentEntityPayload"});
        if (entity == nullptr)
            continue;
        Comment comment;
        yyjson_val *properties = yyjson_obj_get(entity, "properties");
        comment.text = text(yyjson_obj_get(properties, "content"));
        comment.published = str(yyjson_obj_get(properties, "publishedTime"));
        comment.author = str(path(entity, {"author", "displayName"}));
        yyjson_val *toolbar = yyjson_obj_get(entity, "toolbar");
        comment.likes = str(yyjson_obj_get(toolbar, "likeCountNotliked"));
        comment.replies = str(yyjson_obj_get(toolbar, "replyCount"));
        if (comment.likes == "0")
            comment.likes.clear();
        if (comment.replies == "0")
            comment.replies.clear();
        if (!comment.text.empty())
            out->push_back(std::move(comment));
    }
    // The older wording: commentRenderer inside each thread (not the replies).
    if (out->empty())
        walk(root,
             [&](const char *key, yyjson_val *child)
             {
                 if (std::strcmp(key, "commentRepliesRenderer") == 0)
                     return true;
                 if (std::strcmp(key, "commentRenderer") != 0)
                     return false;
                 Comment comment;
                 comment.author = text(yyjson_obj_get(child, "authorText"));
                 comment.text = text(yyjson_obj_get(child, "contentText"));
                 comment.published = text(yyjson_obj_get(child, "publishedTimeText"));
                 comment.likes = text(yyjson_obj_get(child, "voteCount"));
                 if (!comment.text.empty())
                     out->push_back(std::move(comment));
                 return true;
             });

    // The next page: the continuation item that ends the list of comments
    // (the one inside a thread's replies is that thread's, not the list's).
    if (next != nullptr)
    {
        yyjson_val *endpoints = yyjson_obj_get(root, "onResponseReceivedEndpoints");
        for (std::size_t i = 0, n = yyjson_arr_size(endpoints); i < n; ++i)
        {
            yyjson_val *endpoint = yyjson_arr_get(endpoints, i);
            yyjson_val *items =
                path(endpoint, {"reloadContinuationItemsCommand", "continuationItems"});
            if (items == nullptr)
                items = path(endpoint, {"appendContinuationItemsAction", "continuationItems"});
            const std::size_t count = yyjson_arr_size(items);
            if (count == 0)
                continue;
            yyjson_val *last =
                yyjson_obj_get(yyjson_arr_get(items, count - 1), "continuationItemRenderer");
            if (last != nullptr)
                *next = first_token(last);
        }
    }
    yyjson_doc_free(doc);
    return true;
}

bool parse_votes(std::string_view json, long long *likes, long long *dislikes)
{
    yyjson_doc *doc = yyjson_read(json.data(), json.size(), 0);
    if (doc == nullptr)
        return false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *up = yyjson_obj_get(root, "likes");
    yyjson_val *down = yyjson_obj_get(root, "dislikes");
    const bool ok = yyjson_is_num(up) && yyjson_is_num(down);
    if (ok)
    {
        if (likes != nullptr)
            *likes = static_cast<long long>(yyjson_get_num(up));
        if (dislikes != nullptr)
            *dislikes = static_cast<long long>(yyjson_get_num(down));
    }
    yyjson_doc_free(doc);
    return ok;
}

std::string compact_count(long long count)
{
    char text[32];
    if (count < 1000)
        std::snprintf(text, sizeof(text), "%lld", count < 0 ? 0 : count);
    else if (count < 10000)
        std::snprintf(text, sizeof(text), "%.1fK", static_cast<double>(count) / 1000.0);
    else if (count < 1000000)
        std::snprintf(text, sizeof(text), "%lldK", count / 1000);
    else if (count < 10000000)
        std::snprintf(text, sizeof(text), "%.1fM", static_cast<double>(count) / 1000000.0);
    else if (count < 1000000000)
        std::snprintf(text, sizeof(text), "%lldM", count / 1000000);
    else
        std::snprintf(text, sizeof(text), "%.1fB", static_cast<double>(count) / 1000000000.0);
    std::string out = text;
    // "1.0K" reads as "1K".
    if (const std::size_t dot = out.find(".0"); dot != std::string::npos)
        out.erase(dot, 2);
    return out;
}

} // namespace yt

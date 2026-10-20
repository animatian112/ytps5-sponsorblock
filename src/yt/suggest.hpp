// youtube-tv - Search suggestions: YouTube's autocomplete for what is typed.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The completions YouTube's own search box offers (suggestqueries, ds=yt),
// asked on a thread of their own so typing never waits on the network and a
// video loading on the service's worker never holds them up. Only the newest
// query is asked; an answer to an older one is dropped.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace yt
{

// ["query",["completion 1","completion 2",...],...] -> the completions.
std::vector<std::string> parse_suggestions(std::string_view json);

class Suggest
{
  public:
    Suggest();
    ~Suggest();
    Suggest(const Suggest &) = delete;
    Suggest &operator=(const Suggest &) = delete;

    // The language and country of the answers ("en", "US").
    void set_region(std::string hl, std::string gl);
    // Asks for the completions of query (an empty one clears them).
    void ask(const std::string &query);
    // The newest answer and the query it is for; revision moves with each.
    std::vector<std::string> answer(std::string *query = nullptr) const;
    std::uint64_t revision() const;

    struct Impl;

  private:
    std::shared_ptr<Impl> impl_;
};

} // namespace yt

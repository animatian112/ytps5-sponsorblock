// youtube-tv - DeArrow: community titles and thumbnails, as SmartTube shows.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// DeArrow (dearrow.ajay.app, from the SponsorBlock author) replaces clickbait
// titles and thumbnails with ones its users submitted and voted on. Titles
// come from the SponsorBlock server's /api/branding; a submitted thumbnail is
// a moment of the video, rendered by dearrow-thumb.ajay.app. Brandings are
// asked on worker threads of their own, newest first, so a page of cards
// fills in while the rows in view are drawn.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace yt
{

struct Branding
{
    std::string title;          // empty: keep YouTube's
    bool has_thumbnail = false; // false: keep YouTube's
    double thumbnail_s = 0.0;   // the moment the thumbnail shows
};

// The /api/branding answer -> what to show: the first title and the first
// thumbnail, when they are trusted (locked, or votes >= 0) and not the
// original. False when the answer is not readable.
bool parse_branding(std::string_view json, Branding *out);
// A submitted title as shown: a leading '>' (the submitter's "keep my
// casing" mark) is dropped, and so is surrounding white space.
std::string clean_dearrow_title(std::string title);
std::string dearrow_thumbnail_url(const std::string &video_id, double time_s);

class DeArrow
{
  public:
    DeArrow();
    ~DeArrow();
    DeArrow(const DeArrow &) = delete;
    DeArrow &operator=(const DeArrow &) = delete;

    // The SponsorBlock server to ask (Settings > SponsorBlock > alternative).
    void set_alt_server(bool alt);
    // Never waits: true with the branding once it is known; otherwise the
    // video is queued (the newest asked first) and the answer is false.
    bool lookup(const std::string &video_id, Branding *out);
    // Moves whenever a branding arrives.
    std::uint64_t revision() const;

    struct Impl;

  private:
    std::shared_ptr<Impl> impl_;
};

} // namespace yt

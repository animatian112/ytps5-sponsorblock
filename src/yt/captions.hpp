// youtube-tv - Subtitles: a caption track's timed text, fetched in the
// background and looked up by playback position.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace yt::captions
{

struct Cue
{
    double start_s = 0.0;
    double end_s = 0.0;
    std::string text;
};

// YouTube's "json3" timed text: events with tStartMs, dDurationMs and segs of
// utf8 text. Returns false for an answer that is not that shape.
bool parse_json3(std::string_view body, std::vector<Cue> *out);

// Loads one track on a detached worker; poll from the frame thread.
class Loader
{
  public:
    Loader();
    ~Loader();
    Loader(const Loader &) = delete;
    Loader &operator=(const Loader &) = delete;

    void load(const std::string &track_url);
    void clear();
    // The text on screen at `position_s` (lines joined with '\n'), or empty.
    std::string text_at(double position_s);
    // "", "loading", "ok", "error: ..." for the log and the menu.
    std::string status();

    struct Shared; // shared with the detached fetch

  private:
    std::shared_ptr<Shared> shared_;
};

} // namespace yt::captions

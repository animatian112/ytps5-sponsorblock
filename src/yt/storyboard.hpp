// youtube-tv - Seek previews from YouTube's storyboards.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A player answer's playerStoryboardSpecRenderer.spec names sprite sheets of
// small frames: "https://i.ytimg.com/sb/ID/storyboard3_L$L/$N.jpg?sqp=...|"
// then one level per "|": width#height#count#columns#rows#interval_ms#name#sigh.
// A frame of the time t sits on sheet n at column c, row r of that level.

#pragma once

#include <string>
#include <string_view>

namespace yt
{

struct StoryboardFrame
{
    std::string url; // the sheet
    // Where the frame sits on the sheet, 0..1 (a texture rectangle).
    float u = 0.0f;
    float v = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    int width = 0; // one frame in pixels
    int height = 0;
};

class Storyboard
{
  public:
    // Picks the level whose frames are closest to `want_width` pixels wide.
    // False when the spec is empty or unreadable.
    bool parse(std::string_view spec, double duration_s, int want_width = 160);
    bool empty() const
    {
        return count_ == 0;
    }
    // The frame for the time t (clamped to the video).
    StoryboardFrame frame_at(double t) const;

  private:
    std::string base_; // the URL with $L
    int level_ = 0;
    int width_ = 0;
    int height_ = 0;
    int count_ = 0;
    int columns_ = 0;
    int rows_ = 0;
    double interval_s_ = 0.0;
    std::string name_;
    std::string sigh_;
    double duration_s_ = 0.0;
};

} // namespace yt

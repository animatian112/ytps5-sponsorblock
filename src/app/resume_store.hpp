// youtube-tv - Where each video was left, so it continues there.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small text file in the app's data folder, one "video_id seconds" line
// per video, newest first, at most kMaxEntries. A position near the start or
// the end is not kept: the video then simply starts over.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace hui::app
{

class ResumeStore
{
  public:
    static constexpr std::size_t kMaxEntries = 300;

    explicit ResumeStore(std::string path);

    // Seconds to continue from, or 0 to start at the beginning.
    double position(const std::string &video_id) const;
    // Records where a video was left (length 0: unknown) and saves the file.
    void remember(const std::string &video_id, double position_s, double length_s);

    // The file format, for the tests.
    static std::vector<std::pair<std::string, double>> parse(const std::string &text);
    static std::string format(const std::vector<std::pair<std::string, double>> &entries);

  private:
    std::string path_;
    std::vector<std::pair<std::string, double>> entries_; // newest first
};

} // namespace hui::app

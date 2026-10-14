// youtube-tv - SponsorBlock: skip sponsor reads and other marked segments.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Segments come from the community database at sponsor.ajay.app (CC BY-NC-SA
// 4.0) through yt::http on a worker thread of its own; the matching and skip
// rules are SmartTube's (src/sponsorblock/, MIT), so a segment is skipped as
// the playhead enters it, back-to-back segments are jumped in one go, and each
// one is skipped once per video. The frame thread only calls poll().

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace yt::sponsorblock
{

// What the player does when a segment starts (Settings::sb_mode).
enum class Mode : std::uint8_t
{
    skip = 0, // skip at once and show a toast
    ask = 1,  // offer "Triangle: skip" until the segment ends
    mark = 2, // only mark the segments on the seek bar
};

// Which categories are fetched (Settings::sb_scope).
enum class Scope : std::uint8_t
{
    sponsors = 0,  // paid sponsor reads only
    promotion = 1, // + self-promotion and "like and subscribe" reminders
    all = 2,       // + intros, outros, recaps and non-music parts (not filler)
};

unsigned categories_for(Scope scope);

struct Segment
{
    double start_s = 0.0;
    double end_s = 0.0;
    std::string category; // "sponsor", "intro", ...
    std::string label;    // "Sponsor", "Intro", ...
};

enum class Kind : std::uint8_t
{
    none,
    skip, // seek to target_s now
    ask,  // offer a skip to target_s
};

struct Decision
{
    Kind kind = Kind::none;
    double target_s = 0.0;
    std::string label;
};

class Skipper
{
  public:
    Skipper();
    ~Skipper();
    Skipper(const Skipper &) = delete;
    Skipper &operator=(const Skipper &) = delete;

    // Starts loading the segments of a video (or drops them: empty id). Never
    // blocks; the answer arrives a moment later on the worker.
    void load(const std::string &video_id, Scope scope);
    void clear();

    // Changes whenever a new segment list was adopted.
    std::uint64_t revision() const;
    const std::vector<Segment> &segments() const
    {
        return segments_;
    }
    // "ok", "none" (no segments), "error: ..." or "" while loading.
    const std::string &status() const
    {
        return status_;
    }

    // Call every frame while a video plays. Picks up a finished load, then
    // applies SmartTube's rules to the playhead.
    Decision poll(double position_s, double duration_s, bool playing, Mode mode);
    // The skip button: jumps past the segment under the playhead (and any that
    // follow it within 3 s), in every mode. none when not inside one.
    Decision skip_now(double position_s, double duration_s);

  private:
    struct Worker;
    void adopt();

    std::unique_ptr<Worker> worker_;
    std::string video_id_;
    std::vector<Segment> segments_;
    std::string status_;
    std::uint64_t revision_ = 0;
    std::uint64_t adopted_generation_ = 0;
    void *tracker_ = nullptr; // sb_tracker, created on adopt
    Mode tracker_mode_ = Mode::skip;
};

} // namespace yt::sponsorblock

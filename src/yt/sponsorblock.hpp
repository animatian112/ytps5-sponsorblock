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

// SmartTube's per-category settings. The categories in SmartTube's order.
constexpr int kCategoryCount = 10;
// "sponsor", "selfpromo", ... and their names in the settings.
const char *category_id(int index);
const char *category_label(int index);
// What a category does (Settings: Off is "not requested at all").
enum class Action : std::uint8_t
{
    off = 0,    // not fetched
    skip = 1,   // skip silently
    notify = 2, // skip and show a toast
    ask = 3,    // offer "Triangle: skip"
    show = 4,   // only the marker on the seek bar (a highlight: offer the jump)
};
struct Options
{
    Action action[kCategoryCount] = {Action::notify, Action::notify, Action::notify, Action::notify,
                                     Action::notify, Action::notify, Action::notify, Action::off,
                                     Action::show,   Action::show};
    long min_skip_ms = 1000; // shorter segments are never skipped
    bool skip_once = true;   // a segment skipped once plays the next time
    bool alt_server = false; // api.sponsor.ajay.app
    bool operator==(const Options &) const = default;
};

struct Segment
{
    double start_s = 0.0;
    double end_s = 0.0;
    std::string category; // "sponsor", "intro", ...
    std::string label;    // "Sponsor", "Intro", ...
    std::string action;   // "skip", "mute", "poi" (a highlight), "full" (the whole video)
    std::string uuid;     // for the view count report
    int index = -1;       // into category_id()
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
    bool silent = false; // Action::skip: no toast
    std::string uuid;    // the segment skipped, for the view count
};

// Tells the database a segment was skipped (SmartTube's "send skip
// statistics"): one GET-free POST on a detached worker.
void report_viewed(const std::string &uuid, bool alt_server);

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
    // The same with SmartTube's per-category settings; poll(pos, dur,
    // playing) then follows them.
    void load(const std::string &video_id, const Options &options);
    Decision poll(double position_s, double duration_s, bool playing);
    // The highlight (poi_highlight) start, or -1.
    double highlight_s() const;
    // A whole-video label ("Exclusive access", "Sponsor"...), or empty.
    std::string whole_video_label() const;
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
    bool per_category_ = false; // load(Options): the tracker follows options_
    Options options_;
};

} // namespace yt::sponsorblock

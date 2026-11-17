// youtube-tv - Settings > Developer > Run diagnostics: every part of the app, checked.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One pass on a worker thread over what the app depends on: the console
// (memory, the app's folder), the network (each service, listening), YouTube
// (feeds, search, a player answer, the watch page's date) and the paste
// sites the log goes to. Each check is a line on screen and in yt.log; the
// whole report then goes up as a paste, so a phone scans it off the screen.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hui::app
{

enum class DiagStatus : std::uint8_t
{
    info,
    ok,
    warn,
    fail,
};

struct DiagLine
{
    DiagStatus status = DiagStatus::info;
    std::string section; // "Network", "YouTube"...
    std::string name;
    std::string detail;
};

// What the app knows that the worker cannot ask for itself.
struct DiagInput
{
    std::string version;   // "01.000.209"
    std::string data_root; // the app's folder
    std::string language;  // the app's ("es"), and the console's
    std::string console_language;
    std::string gl; // the region YouTube is asked for
    std::string hl;
    bool signed_in = false;
    std::string settings;   // a line of the settings that matter for playback
    std::string last_video; // the video played last ("" for none)
};

class Diagnostics
{
  public:
    // Starts a pass unless one runs; the lines of the last pass are dropped.
    void start(DiagInput input);
    bool running() const;
    std::vector<DiagLine> lines() const;
    // Bumped whenever a line arrives.
    std::uint64_t revision() const;
    // The pass as text: the lines, then the end of yt.log.
    std::string report() const;

    struct State;

  private:
    std::shared_ptr<State> state_;
};

const char *diag_status_word(DiagStatus status);

} // namespace hui::app

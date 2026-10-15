// youtube-tv - Auto-update: check, download and stage a newer release.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The release lives in the project's own GitHub repository (haesele/ytps5):
// the newest release is the source of truth, its asset PPSA98260.zip is the
// payload and the tag is the content version the app compares against.
// Downloading and staging run on their own thread (a release is megabytes and
// streams through the console's payload loader), so the frame only reads a
// snapshot.

#pragma once

#include <cstdint>
#include <string>

namespace yt::update
{

enum class State : unsigned char
{
    idle,
    checking,    // asking the release feed
    up_to_date,  // the running version is the newest
    no_release,  // the repository has no published release yet (feed answers 404)
    available,   // a newer release exists, start() was not called yet
    downloading, // streaming the asset to the helper
    staged,      // the helper has the new version, waiting for the app to close
    failed,
};

struct Snapshot
{
    State state = State::idle;
    std::string current_version; // as the app reports it (param.json)
    std::string latest_version;  // release tag, e.g. "01.000.001"
    std::string notes;           // release body, trimmed
    std::string asset_url;
    std::string error;
    long long asset_size = 0;
    long long downloaded = 0;
    float progress = 0.0f; // 0..1 while downloading
    std::uint64_t revision = 0;
};

// The version the app runs (from /app0/sce_sys/param.json). Call once at
// startup; without it a check can only report "different".
void set_current_version(std::string version);

// Starts a check in the background (no-op while one runs).
void check();
// Starts the download + staging of the release check() found: the helper
// receives the archive, verifies it and waits for the app to close. False when
// there is nothing to install.
bool install();
void cancel();

// Thread-safe snapshot for the frame; copy it and compare revisions.
Snapshot snapshot();
std::uint64_t revision();

// The version string of a release tag, normalised ("v01.000.001" -> "01.000.001").
std::string normalise_version(std::string tag);
// True when `candidate` is newer than `current` (dotted decimal parts).
bool is_newer(std::string_view candidate, std::string_view current);

} // namespace yt::update

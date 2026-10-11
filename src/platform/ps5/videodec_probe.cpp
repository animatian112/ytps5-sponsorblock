// youtube-tv - Startup log for the console's media decoder imports.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decoder has no public API documentation: the research that proved it
// (docs/HARDWARE_DECODE.md) documents behaviour, not argument layouts. The
// import table itself now lives next to the session that calls it
// (src/platform/ps5/videodec.cpp), so there is exactly one place that counts
// what the loader bound; this file only turns that count into the startup log
// line a console run reads first.
//
// 26/26 reachable: the library is loadable for a homebrew title and the NID
// chain is intact, so the hardware path can be attempted. 0/26: the symbols
// stayed null and the session refuses every step by name instead of calling one.

#include "platform/ps5/videodec.hpp"

#include "platform/ps5/system.hpp"

#include <cstddef>

namespace hui::ps5
{

// Logs how many decoder entry points the process carries and how many of them
// the loader bound. Called once at startup: on the console it is the first
// answer to "is libSceVideodec2 reachable from a homebrew title?".
std::size_t report_decoder_imports()
{
    const std::size_t bound = static_cast<std::size_t>(videodec_imports_reachable());
    sys::log("[YTV] videodec2 imports: %zu/%zu reachable", bound,
             static_cast<std::size_t>(kVideodecImportCount));
    return bound;
}

} // namespace hui::ps5

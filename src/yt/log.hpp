// youtube-tv - The app's own log file.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// On the console the standard streams are hard to reach, so the app keeps its
// own line log in the title's storage (/download0/hui/dev/yt.log), which a PC
// can read over FTP as
// /mnt/sandbox/PPSA99350_000/download0/hui/dev/yt.log while the app runs.
// Every stage of a video request writes one line, so "it hangs at loading"
// becomes a specific step.

#pragma once

#include <cstddef>
#include <string>

namespace yt::log
{

// Opens (and truncates when it grew past its cap) the file; empty path or a
// path that cannot be opened leaves the log silent.
void open(const char *path);
void close();

// One timestamped line. Safe from the frame thread and the worker thread.
void line(const char *format, ...) __attribute__((format(printf, 1, 2)));

bool enabled();

// Date and time the running binary was compiled ("Oct 10 2026 18:31:02"):
// tells two builds of the same version apart in the log and on screen.
const char *build_stamp();

// The last lines this process wrote, newest last (at most `max`). The player
// screen shows them while a video is stuck, so a failure is visible on the
// television instead of only in the file.
std::size_t recent(std::string *lines, std::size_t max);

} // namespace yt::log

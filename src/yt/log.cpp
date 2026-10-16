// youtube-tv - The app's own log file.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// On the console the standard streams are hard to reach, so the app keeps its
// own line log in the title's storage (/download0/hui/dev/yt.log), which a PC
// can read over FTP as
// /mnt/sandbox/PPSA98260_000/download0/hui/dev/yt.log while the app runs.
// Every stage of a video request writes one line, and the last lines stay in
// memory so the player screen can show them: a failure has to be visible on
// the television, not only in a file.

#include "yt/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace yt::log
{
namespace
{

// A diagnosis file, not a data store: past this size the next open starts over.
constexpr long kMaxBytes = 256 * 1024;
// Lines kept in memory for the on-screen tail.
constexpr std::size_t kRecent = 16;

std::mutex &mutex()
{
    static std::mutex instance;
    return instance;
}

std::FILE *&file()
{
    static std::FILE *instance = nullptr;
    return instance;
}

std::vector<std::string> &recent_lines()
{
    static std::vector<std::string> instance;
    return instance;
}

} // namespace

void open(const char *path)
{
    if (path == nullptr || path[0] == '\0')
        return;
    std::lock_guard<std::mutex> lock(mutex());
    if (file() != nullptr)
        return;

    long size = 0;
    if (std::FILE *existing = std::fopen(path, "rb"))
    {
        std::fseek(existing, 0, SEEK_END);
        size = std::ftell(existing);
        std::fclose(existing);
    }
    file() = std::fopen(path, size > kMaxBytes ? "w" : "a");
}

void close()
{
    std::lock_guard<std::mutex> lock(mutex());
    if (file() != nullptr)
        std::fclose(file());
    file() = nullptr;
}

bool enabled()
{
    std::lock_guard<std::mutex> lock(mutex());
    return file() != nullptr;
}

const char *build_stamp()
{
    static const char stamp[] = __DATE__ " " __TIME__;
    return stamp;
}

void line(const char *format, ...)
{
    char text[2048]; // room for a full stats URL
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);

    // The tail is kept even when the file could not be opened: the player
    // screen shows it, which is the only diagnosis a television offers.
    std::lock_guard<std::mutex> lock(mutex());
    std::vector<std::string> &lines = recent_lines();
    lines.emplace_back(text);
    while (lines.size() > kRecent)
        lines.erase(lines.begin());

    if (file() == nullptr)
        return;
    std::fprintf(file(), "[ytv] %s\n", text);
    std::fflush(file()); // a crash must not lose the last line
}

std::size_t recent(std::string *lines, std::size_t max)
{
    if (lines == nullptr || max == 0)
        return 0;
    std::lock_guard<std::mutex> lock(mutex());
    const std::vector<std::string> &all = recent_lines();
    const std::size_t count = all.size() < max ? all.size() : max;
    for (std::size_t index = 0; index < count; ++index)
        lines[index] = all[all.size() - count + index];
    return count;
}

} // namespace yt::log

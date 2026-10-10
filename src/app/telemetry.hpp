// youtube-tv - Live numbers about the app itself.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>

namespace hui::app
{

struct Telemetry
{
    static constexpr std::size_t kHistory = 240;
    std::array<float, kHistory> frame_ms{}; // ring buffer, newest at head - 1
    std::size_t head = 0;
    float fps = 0.0f;
    float average_ms = 0.0f;
    std::size_t draw_calls = 0;
    std::size_t instances = 0;
    int voices = 0;      // sounds playing now
    double uptime = 0.0; // seconds since the app opened

    void push(float ms)
    {
        frame_ms[head] = ms;
        head = (head + 1) % kHistory;
    }
    // age 0 is the newest frame.
    float sample(std::size_t age) const
    {
        return frame_ms[(head + kHistory - 1 - age % kHistory) % kHistory];
    }
};

} // namespace hui::app

// ps5-homebrew-ui - DualSense input through scePad.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/input.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace hui::ps5
{

// Reads every buffered sample of the initial user's controller each frame
// (scePadRead batches up to 64), so short taps survive long frames. Follows
// ProsperoLight's radio_input.cpp.
class Pad
{
  public:
    Pad() = default;
    Pad(const Pad &) = delete;
    Pad &operator=(const Pad &) = delete;
    ~Pad();

    bool open();
    void close();
    // Fills out with the samples read since the last call; returns the count.
    // Returns one disconnected sample if neither a controller nor a remote is
    // open. Keep two spare slots in the span: a remote key pressed and let go
    // inside one frame is sent as its own press and release.
    std::size_t read(std::span<PadSample> out);
    // Whether a *controller* is open. A media remote can drive the app on its
    // own, so this is not the same question as "is there any input".
    bool is_open() const
    {
        return handle_ >= 0;
    }

    // Colours the controller's light bar (for example with the active
    // design's accent). Repeated calls with the same colour do nothing.
    void set_light_bar(std::uint8_t r, std::uint8_t g, std::uint8_t b);
    // Starts a short rumble: strength 0..1, for seconds. tick() stops it.
    void rumble(float strength, float seconds);
    // Call once per frame: ends a rumble whose time is up.
    void tick(float dt);

  private:
    // A media remote is a pad of its own and the console does not say which
    // port it is on, so every candidate is opened and read with its own state:
    // stopping at the first that opens lets a port that never speaks hide the
    // real one. Its keys are not in the button word - each is a key code in one
    // byte, which hui::remote_key_bits turns into pad bits.
    struct RemoteSource
    {
        int user = -1;
        int port = 0;
        int handle = -1;
        bool owned = false;         // false when scePadGetHandle lent it to us
        bool snapshot_only = false; // the special port queues no samples
        bool connected = false;
        bool open_logged = false;
        std::uint32_t buttons = 0; // what this source is holding down now
        std::uint32_t ignore = 0;  // down when it opened; ignored until let go
        int last_open_result = 0;
        int open_failures = 0;
        int snapshot_failures = 0; // consecutive refusals from a snapshot port
        std::uint64_t next_open_frame = 0;
        std::uint64_t armed_frame = 0; // the frame it opened on; not read again on it
    };

    // Opens every candidate that is closed and not backing off.
    void open_remote();
    // Takes the source's held keys as the ones to ignore until they are let go.
    void arm_remote(RemoteSource &source);
    // One source's samples for this frame; adds to taps the keys it pressed.
    void read_remote(RemoteSource &source, bool system_has_screen, std::uint32_t *taps);
    bool any_remote_open() const;
    // scePadOpen, falling back to borrowing an already-open handle. *owned says
    // whether closing it is ours to do.
    static int open_port(int user, int port, bool *owned);

    static constexpr std::size_t kRemoteSources = 3;
    RemoteSource remotes_[kRemoteSources];
    std::uint64_t frame_ = 0;
    std::uint32_t last_remote_held_ = 0;
    int handle_ = -1;
    bool owns_pad_ = false;
    int user_ = -1;
    bool owns_user_service_ = false;
    float rumble_left_ = 0.0f;
    std::uint32_t light_bar_ = 0xffffffffu;
};

} // namespace hui::ps5

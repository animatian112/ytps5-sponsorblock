// youtube-tv - Settings > Developer: the player's knobs, read when a video opens.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace yt
{

struct PlayerTuning
{
    int decoder_threads = 0; // 0: automatic (4, or 6 above 1080p)
    int skip_deblock = 1;    // 0 never, 1 above 1080p, 2 always
    int convert_threads = 4; // the RGBA conversion's threads
    int av_offset_ms = 0;    // the picture this much later (+) or earlier (-) than the sound
    // Hardware decoding: pictures handed to the screen as NV12 for the GPU to
    // convert (only when the screen can take them), and the decoder's depth.
    bool gpu_convert = false;
    int hardware_depth = 1;
};

// Set by the app from its settings; read by the platform player as it opens
// a stream (thread-safe).
void set_player_tuning(const PlayerTuning &tuning);
PlayerTuning player_tuning();

} // namespace yt

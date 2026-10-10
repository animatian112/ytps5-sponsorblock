// youtube-tv - Stream selection: which adaptive format plays.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "yt/types.hpp"

#include <vector>

namespace yt
{

// True when the settings allow this video codec at all.
bool codec_enabled(const CodecChoice &choice, Codec codec);

// The video format to play: the highest height not above choice.max_height
// among the enabled codecs, preferring AV1, then VP9, then AVC, and the
// higher bitrate inside one codec. nullptr when nothing matches (the caller
// shows "Kein Stream für aktive Codecs").
const Format *select_video_format(const std::vector<Format> &formats, const CodecChoice &choice);

// The audio format to play: Opus before AAC, higher bitrate first.
const Format *select_audio_format(const std::vector<Format> &formats);

} // namespace yt

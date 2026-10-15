// youtube-tv - Stream selection: which adaptive format plays.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "yt/types.hpp"

#include <string>
#include <vector>

namespace yt
{

// True when the settings allow this video codec at all.
bool codec_enabled(const CodecChoice &choice, Codec codec);

// Software-decode height ceilings, in lines. The console has no usable video
// decoder to hand yet: videodec_probe only proves the hardware surface can be
// imported and nothing decodes through it, so libavcodec decodes every frame on
// the CPU. AVC is by far the cheapest of the three at 1080p, VP9 needs roughly
// twice the cycles at the same size, and AV1 at HD cannot run in real time at
// all (it turns into a black screen). Hence the descending ceilings; they rise
// to the display's native height once hardware decoding lands.
inline constexpr int kSoftwareCeilingAvc = 1080;
inline constexpr int kSoftwareCeilingVp9 = 720;
inline constexpr int kSoftwareCeilingAv1 = 480;

// The height this codec may be selected at: its software-decode ceiling, or the
// user's own cap when that is lower. 0 means the codec carries no ceiling.
int codec_height_ceiling(Codec codec, const CodecChoice &choice);

// The video format to play: at equal height AVC before VP9 before AV1 (the
// software-decode order, not YouTube's hardware order), the highest height that
// fits both choice.max_height and the codec's ceiling, and the higher bitrate
// inside one codec. The URL kinds are ranked apart: a plain URL opens as it
// stands, while a signatureCipher format needs the descrambler first (an
// unproven heuristic) whose output googlevideo can refuse with 403. A plain
// format is picked whenever one survives the caps and the ceilings; only when
// none does the best ciphered format under the caps take over (its over-ceiling
// fallback included) - a refused ciphered URL still beats no stream, and the
// caller retries past it. In DecodeMode::hardware an AV1 format is never
// returned - the console has no AV1 decoder - though the codec ceilings still
// apply. When the ceilings filter everything out, the lowest-height enabled
// format is returned instead of nothing - a 480p AVC stream still beats a
// black screen - and that over-ceiling plain fallback comes only after the
// ciphered pool has nothing left. nullptr only when no enabled codec has a
// usable format inside choice.max_height (the caller shows "Kein Stream für
// aktive Codecs").
const Format *select_video_format(const std::vector<Format> &formats, const CodecChoice &choice);

// One line describing a selection: "itag=137 1920x1080 AVC (plain)
// ceiling=1080p" - the URL kind the format carries - plus "fallback=above
// ceiling" when the format only got picked because nothing inside the software
// ceiling was left. nullptr becomes "none". Used for the selector's own log
// line and available to the callers.
std::string describe_selection(const Format *format, const CodecChoice &choice);

// The audio format to play. On a video with several sound tracks: the one
// named by `track` (an audioTrack.id) when it is offered, else the original
// (audioIsDefault, or "original" in its name) - never a dub YouTube happened to
// list first. Then: no DRC copy, Opus before AAC, higher bitrate first.
const Format *select_audio_format(const std::vector<Format> &formats,
                                  const std::string &track = {});

// The distinct sound tracks of an answer, in the order offered (empty for a
// video with a single unnamed track).
std::vector<AudioTrack> audio_tracks(const std::vector<Format> &formats);

} // namespace yt

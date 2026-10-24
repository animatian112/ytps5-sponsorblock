// youtube-tv - Stream selection: which adaptive format plays.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/codec.hpp"

#include "yt/log.hpp"

namespace yt
{

bool codec_enabled(const CodecChoice &choice, Codec codec)
{
    switch (codec)
    {
    case Codec::avc:
        return choice.avc;
    case Codec::vp9:
        return choice.vp9;
    case Codec::av1:
        return choice.av1;
    default:
        return false;
    }
}

int codec_height_ceiling(Codec codec, const CodecChoice &choice)
{
    int ceiling = 0;
    switch (codec)
    {
    case Codec::avc:
        // AVC always decodes on the CPU (the hardware path is VP9's), and
        // stops at 1080p on YouTube anyway.
        ceiling = kSoftwareCeilingAvc;
        break;
    case Codec::vp9:
        // Decoding: Hardware sends VP9 to the console's decoder chip, which
        // takes any height the user allows; Auto and Software decode on the
        // CPU and keep its ceiling.
        ceiling = choice.decode == DecodeMode::hardware ? 4320 : kSoftwareCeilingVp9;
        break;
    case Codec::av1:
        ceiling = kSoftwareCeilingAv1;
        break;
    default:
        return 0; // audio and unknown carry no ceiling of their own
    }
    // The user's cap is the overall limit: it lowers the software ceiling.
    if (choice.max_height > 0 && choice.max_height < ceiling)
        ceiling = choice.max_height;
    return ceiling;
}

std::string describe_selection(const Format *format, const CodecChoice &choice)
{
    if (format == nullptr)
        return "none";
    std::string text = "itag=" + std::to_string(format->itag) + " " +
                       std::to_string(format->width) + "x" + std::to_string(format->height) + " " +
                       codec_label(format->codec) + (format->ciphered ? " (ciphered)" : " (plain)");
    const int ceiling = codec_height_ceiling(format->codec, choice);
    text += ceiling > 0 ? " ceiling=" + std::to_string(ceiling) + "p" : " ceiling=none";
    if (ceiling > 0 && format->height > ceiling)
        text += " fallback=above ceiling";
    return text;
}

namespace
{

// Preference among equal heights: AVC, then VP9, then AV1 - the order that
// keeps software decoding real time. (The hardware decoder, once it lands,
// prefers the reverse.) Ties inside one codec go to the higher bitrate.
int codec_rank(Codec codec)
{
    switch (codec)
    {
    case Codec::avc:
        return 3;
    case Codec::vp9:
        return 2;
    case Codec::av1:
        return 1;
    default:
        return 0;
    }
}

// True when `candidate` beats `current` at the same height.
bool beats_at_equal_height(const Format &candidate, const Format &current)
{
    const int rank = codec_rank(candidate.codec);
    const int current_rank = codec_rank(current.codec);
    return rank > current_rank || (rank == current_rank && candidate.bitrate > current.bitrate);
}

// The two formats one URL kind can end on: the highest inside its codec
// ceiling, and - when nothing fits - the lowest above it.
struct Pool
{
    const Format *best = nullptr;     // highest format inside its codec ceiling
    const Format *fallback = nullptr; // lowest format above its codec ceiling
};

} // namespace

const Format *select_video_format(const std::vector<Format> &formats, const CodecChoice &choice)
{
    // The two URL kinds are scored apart: a plain URL opens as it stands,
    // while a signatureCipher format needs the descrambler first (an unproven
    // heuristic) whose output googlevideo can refuse with 403. A plain format
    // is therefore picked whenever one survives the caps and the ceilings;
    // only when none does the best ciphered format under the caps take over -
    // a refused ciphered URL still beats no stream, and the caller retries
    // past it. The lowest over-ceiling plain format stays the last resort.
    if (choice.exact_itag > 0)
    {
        // One format picked from the full list: exactly that one.
        for (const Format &format : formats)
            if (format.itag == choice.exact_itag && is_video_codec(format.codec) &&
                !format.url.empty())
            {
                log::line("video: selection %s (picked format)",
                          describe_selection(&format, choice).c_str());
                return &format;
            }
    }
    if (choice.exact_height > 0)
    {
        // The quality menu's pick: the best format at exactly that height,
        // plain URLs before ciphered ones.
        const Format *exact = nullptr;
        for (const Format &format : formats)
        {
            if (!is_video_codec(format.codec) || !codec_enabled(choice, format.codec) ||
                format.url.empty() || format.height != choice.exact_height)
                continue;
            // Hardware never takes AV1; Auto plays above 1080p on the hardware
            // decoder too, so there AV1 only plays when nothing else has the height.
            if (format.codec == Codec::av1 && (choice.decode == DecodeMode::hardware ||
                                               (choice.decode == DecodeMode::auto_detect &&
                                                format.height > 1080 && exact != nullptr)))
                continue;
            if (exact != nullptr && exact->codec == Codec::av1 && format.codec != Codec::av1 &&
                choice.decode == DecodeMode::auto_detect && format.height > 1080 &&
                (exact->ciphered || !format.ciphered))
            {
                exact = &format;
                continue;
            }
            if (exact == nullptr || (exact->ciphered && !format.ciphered) ||
                (exact->ciphered == format.ciphered && beats_at_equal_height(format, *exact)))
                exact = &format;
        }
        if (exact != nullptr)
        {
            log::line("video: selection %s (picked height)",
                      describe_selection(exact, choice).c_str());
            return exact;
        }
    }
    Pool plain;
    Pool ciphered;
    bool av1_skipped = false;
    for (const Format &format : formats)
    {
        if (!is_video_codec(format.codec) || !codec_enabled(choice, format.codec))
            continue;
        // The console has no AV1 decoder: in hardware mode AV1 can never play,
        // so it is dropped here, before it could win on height or fall back.
        if (choice.decode == DecodeMode::hardware && format.codec == Codec::av1)
        {
            if (!av1_skipped)
            {
                log::line("video: AV1 skipped, no hardware decoder for it");
                av1_skipped = true;
            }
            continue;
        }
        if (format.url.empty() || format.height <= 0)
            continue;
        if (choice.max_height > 0 && format.height > choice.max_height)
            continue; // the user's cap is hard: the fallback never crosses it
        Pool &pool = format.ciphered ? ciphered : plain;
        const int ceiling = codec_height_ceiling(format.codec, choice);
        if (ceiling > 0 && format.height > ceiling)
        {
            // Above the codec's software ceiling: keep the lowest one around as
            // a last resort, a low stream still beats a black screen.
            if (pool.fallback == nullptr || format.height < pool.fallback->height ||
                (format.height == pool.fallback->height &&
                 beats_at_equal_height(format, *pool.fallback)))
                pool.fallback = &format;
            continue;
        }
        if (pool.best == nullptr || format.height > pool.best->height ||
            (format.height == pool.best->height && beats_at_equal_height(format, *pool.best)))
            pool.best = &format;
    }
    // Plain first while any plain format fits the caps and the ceilings, then
    // the ciphered pool under the caps, and only then the over-ceiling plain
    // fallback - a low stream still beats a black screen.
    const Format *picked = plain.best;
    if (picked == nullptr)
        picked = ciphered.best != nullptr ? ciphered.best : ciphered.fallback;
    if (picked == nullptr)
        picked = plain.fallback;
    log::line("video: selection %s", describe_selection(picked, choice).c_str());
    return picked;
}

namespace
{

bool names_original(const std::string &name)
{
    std::string lower = name;
    for (char &c : lower)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return lower.find("original") != std::string::npos;
}

} // namespace

const Format *select_audio_format(const std::vector<Format> &formats, const std::string &track)
{
    // Which track: the asked one if offered, else the default/original one.
    bool asked_offered = false;
    bool any_default = false;
    bool any_original = false;
    bool any_plain = false; // a non-DRC copy exists
    for (const Format &format : formats)
    {
        if (!is_audio_codec(format.codec) || format.url.empty())
            continue;
        asked_offered = asked_offered || (!track.empty() && format.audio_track_id == track);
        any_default = any_default || format.audio_default;
        any_original = any_original || names_original(format.audio_track_name);
        any_plain = any_plain || !format.drc;
    }
    const auto wanted = [&](const Format &format)
    {
        if (asked_offered)
            return format.audio_track_id == track;
        if (any_default)
            return format.audio_default;
        if (any_original)
            return names_original(format.audio_track_name);
        return true;
    };

    const Format *best = nullptr;
    for (const Format &format : formats)
    {
        if (!is_audio_codec(format.codec) || format.url.empty() || !wanted(format))
            continue;
        if (any_plain && format.drc)
            continue;
        if (best == nullptr)
        {
            best = &format;
            continue;
        }
        const bool opus = format.codec == Codec::opus;
        const bool best_opus = best->codec == Codec::opus;
        if (opus != best_opus)
        {
            if (opus)
                best = &format;
            continue;
        }
        if (format.bitrate > best->bitrate)
            best = &format;
    }
    return best;
}

std::vector<AudioTrack> audio_tracks(const std::vector<Format> &formats)
{
    std::vector<AudioTrack> out;
    for (const Format &format : formats)
    {
        if (!is_audio_codec(format.codec) || format.audio_track_id.empty())
            continue;
        bool known = false;
        for (AudioTrack &track : out)
        {
            if (track.id == format.audio_track_id)
            {
                known = true;
                track.is_default = track.is_default || format.audio_default;
            }
        }
        if (!known)
            out.push_back(
                {format.audio_track_id,
                 format.audio_track_name.empty() ? format.audio_track_id : format.audio_track_name,
                 format.audio_default});
    }
    return out;
}

} // namespace yt

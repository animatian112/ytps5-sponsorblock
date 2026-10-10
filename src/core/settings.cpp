// ps5-homebrew-ui - Player settings and their save format.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/settings.hpp"

#include "core/bytes.hpp"

#include <algorithm>

namespace hui
{

namespace
{
constexpr std::uint8_t kVersion = 2;
}

std::string encode_settings(const Settings &settings)
{
    bytes::Writer w;
    w.put(kVersion);
    w.put(static_cast<std::uint8_t>(settings.music_volume));
    w.put(static_cast<std::uint8_t>(settings.sfx_volume));
    w.put(static_cast<std::uint8_t>(settings.ui_volume));
    w.put_bool(settings.reduced_motion);
    w.put_bool(settings.swap_confirm);
    w.put_bool(settings.show_fps);
    w.put_bool(settings.haptics);
    w.put_bool(settings.light_bar);
    w.put(static_cast<std::uint8_t>(settings.resolution));
    w.put(static_cast<std::uint8_t>(settings.hint_size));
    w.put(static_cast<std::uint8_t>(settings.concept_index));
    w.put_bool(settings.codec_avc);
    w.put_bool(settings.codec_vp9);
    w.put_bool(settings.codec_av1);
    w.put(static_cast<std::uint16_t>(settings.max_height));
    w.put(static_cast<std::uint8_t>(settings.video_volume));
    return w.data();
}

bool decode_settings(std::string_view data, Settings *settings)
{
    bytes::Reader r(data);
    if (r.get<std::uint8_t>() != kVersion)
        return false;
    Settings s;
    s.music_volume = std::clamp<int>(r.get<std::uint8_t>(), 0, 10);
    s.sfx_volume = std::clamp<int>(r.get<std::uint8_t>(), 0, 10);
    s.ui_volume = std::clamp<int>(r.get<std::uint8_t>(), 0, 10);
    s.reduced_motion = r.get_bool();
    s.swap_confirm = r.get_bool();
    s.show_fps = r.get_bool();
    s.haptics = r.get_bool();
    s.light_bar = r.get_bool();
    s.resolution = std::clamp<int>(r.get<std::uint8_t>(), 0, Settings::kResolutionCount - 1);
    s.hint_size = std::clamp<int>(r.get<std::uint8_t>(), 0, 2);
    s.concept_index = r.get<std::uint8_t>();
    s.codec_avc = r.get_bool();
    s.codec_vp9 = r.get_bool();
    s.codec_av1 = r.get_bool();
    s.max_height = r.get<std::uint16_t>();
    if (s.max_height != 720 && s.max_height != 1080 && s.max_height != 1440 && s.max_height != 2160)
        s.max_height = 1080;
    if (!s.codec_avc && !s.codec_vp9 && !s.codec_av1)
        s.codec_avc = true;
    s.video_volume = std::clamp<int>(r.get<std::uint8_t>(), 0, 10);
    if (!r.finished())
        return false;
    *settings = s;
    return true;
}

bool Settings::codec_on(const char *id) const
{
    if (id == nullptr)
        return false;
    if (std::string_view(id) == "AVC")
        return codec_avc;
    if (std::string_view(id) == "VP9")
        return codec_vp9;
    if (std::string_view(id) == "AV1")
        return codec_av1;
    return false;
}

bool Settings::is_last_codec(const char *id) const
{
    const int enabled = (codec_avc ? 1 : 0) + (codec_vp9 ? 1 : 0) + (codec_av1 ? 1 : 0);
    return enabled <= 1 && codec_on(id);
}

bool Settings::set_codec(const char *id, bool on)
{
    if (id == nullptr)
        return false;
    if (!on && is_last_codec(id))
        return false;
    if (std::string_view(id) == "AVC")
        codec_avc = on;
    else if (std::string_view(id) == "VP9")
        codec_vp9 = on;
    else if (std::string_view(id) == "AV1")
        codec_av1 = on;
    else
        return false;
    return true;
}

} // namespace hui

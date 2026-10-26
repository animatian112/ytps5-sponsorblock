// ps5-homebrew-ui - DualSense input through scePad.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/pad.hpp"

#include "platform/ps5/system.hpp"
#include "yt/log.hpp"

#include <cstddef>
#include <cstdint>

namespace
{

// scePadRead sample layout (120 bytes), as typed in ProsperoLight.
struct RawPadSample
{
    std::uint32_t buttons;
    std::uint8_t left_x;
    std::uint8_t left_y;
    std::uint8_t right_x;
    std::uint8_t right_y;
    std::uint8_t l2;
    std::uint8_t r2;
    std::uint8_t reserved0[66];
    std::int32_t connected;
    std::uint64_t timestamp_us;
    std::uint8_t extension[16];
    std::uint8_t connected_count;
    std::uint8_t reserved1[15];
};
static_assert(sizeof(RawPadSample) == 120);
static_assert(offsetof(RawPadSample, connected) == 0x4c);
static_assert(offsetof(RawPadSample, timestamp_us) == 0x50);
static_assert(offsetof(RawPadSample, connected_count) == 0x68);

constexpr int kAlreadyInitialized = static_cast<int>(0x80960003);
// The pad port type of a media remote (the PS4 SDK's REMOTE_CONTROL).
constexpr int kPortRemoteControl = 16;

// The controller's last sample: what a frame with only a remote change
// carries for the controller (one Pad per app).
RawPadSample &last_sample()
{
    static RawPadSample sample = []
    {
        RawPadSample centred{};
        centred.left_x = centred.left_y = centred.right_x = centred.right_y = 128;
        centred.connected = 1;
        return centred;
    }();
    return sample;
}
// The classic two-motor rumble (the other mode is the DualSense's haptics).
constexpr int kVibrationModeCompatible = 2;

struct PadVibration
{
    std::uint8_t large_motor;
    std::uint8_t small_motor;
};

struct PadColor
{
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t reserved;
};

} // namespace

extern "C"
{
    int sceUserServiceInitialize(const void *params);
    int sceUserServiceTerminate(void);
    int sceUserServiceGetInitialUser(int *user);
    int scePadInit(void);
    int scePadOpen(int user, int type, int index, const void *parameters);
    int scePadRead(int handle, RawPadSample *samples, int count);
    int scePadClose(int handle);
    int scePadSetVibrationMode(int handle, int mode);
    int scePadSetVibration(int handle, const PadVibration *vibration);
    int scePadSetLightBar(int handle, const PadColor *color);
}

namespace hui::ps5
{

Pad::~Pad()
{
    close();
}

bool Pad::open()
{
    const int init = sceUserServiceInitialize(nullptr);
    owns_user_service_ = init == 0;
    if (init != 0 && init != kAlreadyInitialized)
        sys::log("[HUI] pad sceUserServiceInitialize=0x%08x", static_cast<unsigned>(init));
    if (sceUserServiceGetInitialUser(&user_) != 0)
    {
        sys::log("[HUI] pad no initial user");
        return false;
    }
    const int pad_init = scePadInit();
    if (pad_init != 0)
        sys::log("[HUI] pad scePadInit=0x%08x", static_cast<unsigned>(pad_init));
    // The pad service can lag behind title start; retry briefly.
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        handle_ = scePadOpen(user_, 0, 0, nullptr);
        if (handle_ >= 0)
        {
            const int mode = scePadSetVibrationMode(handle_, kVibrationModeCompatible);
            sys::log("[HUI] pad open user=0x%08x handle=%d attempts=%d vibration_mode=0x%x",
                     static_cast<unsigned>(user_), handle_, attempt + 1,
                     static_cast<unsigned>(mode));
            open_remote();
            return true;
        }
        sys::sleep_us(50000);
    }
    sys::log("[HUI] pad scePadOpen failed=0x%08x", static_cast<unsigned>(handle_));
    handle_ = -1;
    return false;
}

void Pad::open_remote()
{
    if (remote_ >= 0 || user_ < 0)
        return;
    // The remote as a pad of its own: the remote-control port of the player,
    // then of the system user (a remote paired to the console rather than to
    // a player), then the "special" port. The first that opens is read.
    constexpr int kSystemUser = 0xFF;
    constexpr int kPortSpecial = 2;
    const struct
    {
        int user;
        int port;
    } tries[] = {
        {user_, kPortRemoteControl}, {kSystemUser, kPortRemoteControl}, {user_, kPortSpecial}};
    for (const auto &attempt : tries)
    {
        remote_ = scePadOpen(attempt.user, attempt.port, 0, nullptr);
        // Logged on the first round only: a console without a remote says so once.
        if (remote_retry_ == 0)
        {
            sys::log("[HUI] pad media remote user=0x%x port=%d open=0x%08x",
                     static_cast<unsigned>(attempt.user), attempt.port,
                     static_cast<unsigned>(remote_));
            yt::log::line("remote: media remote user=0x%x port=%d -> 0x%08x",
                          static_cast<unsigned>(attempt.user), attempt.port,
                          static_cast<unsigned>(remote_));
        }
        if (remote_ >= 0)
            return;
    }
    remote_ = -1;
}

void Pad::set_light_bar(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    const std::uint32_t packed =
        (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | b;
    if (handle_ < 0 || packed == light_bar_)
        return;
    light_bar_ = packed;
    const PadColor color{r, g, b, 0};
    const int result = scePadSetLightBar(handle_, &color);
    if (result != 0)
        sys::log("[HUI] pad scePadSetLightBar=0x%08x", static_cast<unsigned>(result));
}

void Pad::rumble(float strength, float seconds)
{
    if (handle_ < 0 || strength <= 0.0f || seconds <= 0.0f)
        return;
    const float clamped = strength > 1.0f ? 1.0f : strength;
    // The small motor gives the crisp tick a UI wants; the large one adds
    // weight only to strong effects.
    const PadVibration vibration{
        static_cast<std::uint8_t>(clamped > 0.6f ? (clamped - 0.6f) * 2.5f * 255.0f : 0.0f),
        static_cast<std::uint8_t>(clamped * 255.0f)};
    scePadSetVibration(handle_, &vibration);
    rumble_left_ = seconds;
}

void Pad::tick(float dt)
{
    if (rumble_left_ <= 0.0f)
        return;
    rumble_left_ -= dt;
    if (rumble_left_ <= 0.0f && handle_ >= 0)
    {
        const PadVibration stop{0, 0};
        scePadSetVibration(handle_, &stop);
    }
}

void Pad::close()
{
    if (handle_ >= 0)
    {
        const PadVibration stop{0, 0};
        scePadSetVibration(handle_, &stop);
        scePadClose(handle_);
    }
    handle_ = -1;
    if (remote_ >= 0)
        scePadClose(remote_);
    remote_ = -1;
    if (owns_user_service_)
        sceUserServiceTerminate();
    owns_user_service_ = false;
}

std::size_t Pad::read(std::span<PadSample> out)
{
    if (out.empty())
        return 0;
    if (handle_ < 0)
    {
        out[0] = PadSample{};
        return 1;
    }
    RawPadSample raw[64];
    const int limit = static_cast<int>(out.size() < 64 ? out.size() : 64);
    int count = scePadRead(handle_, raw, limit);
    if (count < 0)
        count = 0;

    // The media remote, when one opened: its buttons are held state merged
    // into the controller's samples (one stream of samples, so a held button
    // never reads as released by the other device's sample in between).
    if (remote_ < 0 && ++remote_retry_ % 300 == 0)
        open_remote(); // a remote paired after the app started
    if (remote_ >= 0)
    {
        RawPadSample extra[16];
        const int got = scePadRead(remote_, extra, 16);
        for (int i = 0; i < got; ++i)
        {
            if (extra[i].connected == 0 && extra[i].buttons == 0)
                continue;
            remote_buttons_ = extra[i].buttons;
            const std::uint32_t fresh = remote_buttons_ & ~remote_seen_;
            if (fresh != 0)
            {
                remote_seen_ |= fresh;
                sys::log("[HUI] pad media remote buttons 0x%08x (new 0x%08x)",
                         static_cast<unsigned>(remote_buttons_), static_cast<unsigned>(fresh));
                yt::log::line("remote: media remote buttons 0x%08x (new 0x%08x)",
                              static_cast<unsigned>(remote_buttons_), static_cast<unsigned>(fresh));
            }
        }
        if (got > 0 && count == 0)
        {
            // The controller sent nothing this frame: the remote's change
            // still arrives, on the controller's last state.
            raw[0] = last_sample();
            count = 1;
        }
        for (int i = 0; i < count; ++i)
            raw[i].buttons |= remote_buttons_;
    }
    if (count > 0)
    {
        last_sample() = raw[count - 1];
        last_sample().buttons &= ~remote_buttons_;
    }
    if (count <= 0)
        return 0;
    for (int index = 0; index < count; ++index)
    {
        const RawPadSample &r = raw[index];
        PadSample &s = out[static_cast<std::size_t>(index)];
        s.buttons = r.buttons;
        s.left_x = r.left_x;
        s.left_y = r.left_y;
        s.right_x = r.right_x;
        s.right_y = r.right_y;
        s.l2 = r.l2;
        s.r2 = r.r2;
        s.connected = r.connected != 0;
        s.timestamp_us = r.timestamp_us;
    }
    return static_cast<std::size_t>(count);
}

} // namespace hui::ps5

// ps5-homebrew-ui - DualSense input through scePad.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/pad.hpp"

#include "platform/ps5/system.hpp"
#include "yt/log.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

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
    // A media remote's key arrives here and in no button bit. remote_keys[3]
    // is offset 0x6c, where PS5-SDL reads it (src/video/ps5/SDL_ps5remote.c).
    std::uint8_t remote_keys[15];
};
static_assert(sizeof(RawPadSample) == 120);
static_assert(offsetof(RawPadSample, connected) == 0x4c);
static_assert(offsetof(RawPadSample, timestamp_us) == 0x50);
static_assert(offsetof(RawPadSample, connected_count) == 0x68);
static_assert(offsetof(RawPadSample, remote_keys) + 3 == 0x6c);

// SDL's PS5_REMOTE_KEY_OFFSET.
constexpr std::size_t kRemoteKeyIndex = 3;

constexpr int kAlreadyInitialized = static_cast<int>(0x80960003);
// Something else holds the port; its handle can still be borrowed.
constexpr int kPadAlreadyOpened = static_cast<int>(0x80920004);
constexpr int kPortStandard = 0;
constexpr int kPortSpecial = 2; // where a Bluetooth media remote can land
constexpr int kPortRemoteControl = 16;
// A remote paired to the console rather than to a player.
constexpr int kSystemUser = 0xFF;
constexpr std::uint64_t kRetryFrames = 300;
constexpr std::uint64_t kRetrySlowFrames = 1800;
constexpr int kSlowAfterFailures = 12;
// A snapshot port that will not answer is no evidence a key is still down.
constexpr int kSnapshotDeadFrames = 30;

// sceSystemServiceGetStatus's answer in the PS4 layout; the rest is room.
struct SystemServiceStatus
{
    std::int32_t event_num;
    std::uint8_t system_ui_overlaid;
    std::uint8_t in_background;
    std::uint8_t cpu_mode7_normal;
    std::uint8_t live_streaming;
    std::uint8_t out_of_vr_area;
    std::uint8_t reserved[251];
};

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
    int scePadGetHandle(int user, int type, int index);
    int scePadRead(int handle, RawPadSample *samples, int count);
    int scePadReadState(int handle, RawPadSample *state);
    int scePadClose(int handle);
    int scePadSetVibrationMode(int handle, int mode);
    int scePadSetVibration(int handle, const PadVibration *vibration);
    int scePadSetLightBar(int handle, const PadColor *color);
    int sceSystemServiceGetStatus(SystemServiceStatus *status);
}

namespace
{

// A remote's sample as pad bits: its button word plus its key byte.
std::uint32_t remote_bits(const RawPadSample &sample, bool system_has_screen)
{
    if (!hui::remote_input_is_ours(sample.buttons, system_has_screen))
        return 0;
    return (sample.buttons & ~hui::pad_bits::kIntercepted) |
           hui::remote_key_bits(sample.remote_keys[kRemoteKeyIndex]);
}

// Fails open: on a console where the import is missing, the other answer would
// swallow every remote key.
bool system_has_screen()
{
    SystemServiceStatus status;
    std::memset(&status, 0, sizeof status);
    if (sceSystemServiceGetStatus(&status) != 0)
        return false;
    return status.system_ui_overlaid != 0 || status.in_background != 0;
}

void copy_sample(const RawPadSample &raw, hui::PadSample &out)
{
    out.buttons = raw.buttons;
    out.left_x = raw.left_x;
    out.left_y = raw.left_y;
    out.right_x = raw.right_x;
    out.right_y = raw.right_y;
    out.l2 = raw.l2;
    out.r2 = raw.r2;
    out.connected = raw.connected != 0;
    out.timestamp_us = raw.timestamp_us;
}

} // namespace

namespace hui::ps5
{

Pad::~Pad()
{
    close();
}

int Pad::open_port(int user, int port, bool *owned)
{
    int handle = scePadOpen(user, port, 0, nullptr);
    *owned = handle >= 0;
    if (handle == kPadAlreadyOpened)
    {
        // Borrow it rather than give up, as PS5-SDL does; closing it is then
        // not ours.
        handle = scePadGetHandle(user, port, 0);
        *owned = false;
    }
    return handle;
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
        handle_ = open_port(user_, kPortStandard, &owns_pad_);
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
    owns_pad_ = false;
    open_remote(); // a media remote can drive the app on its own
    return any_remote_open();
}

void Pad::open_remote()
{
    if (user_ < 0)
        return;
    const struct
    {
        int user;
        int port;
    } tries[] = {
        {user_, kPortRemoteControl}, {kSystemUser, kPortRemoteControl}, {user_, kPortSpecial}};
    static_assert(std::size(tries) == kRemoteSources, "one state per candidate port");
    for (std::size_t i = 0; i < std::size(tries); ++i)
    {
        RemoteSource &source = remotes_[i];
        if (source.handle >= 0 || frame_ < source.next_open_frame)
            continue;
        source.user = tries[i].user;
        source.port = tries[i].port;
        // The special port answers scePadReadState but queues no samples at
        // all, so it is read the other way round. See read_remote().
        source.snapshot_only = source.port == kPortSpecial;
        source.handle = open_port(source.user, source.port, &source.owned);
        // Logged when it changes, so a console without a remote says so once.
        if (!source.open_logged || source.handle != source.last_open_result)
        {
            source.open_logged = true;
            source.last_open_result = source.handle;
            sys::log("[HUI] pad media remote user=0x%x port=%d open=0x%08x",
                     static_cast<unsigned>(source.user), source.port,
                     static_cast<unsigned>(source.handle));
            yt::log::line("remote: media remote user=0x%x port=%d -> 0x%08x",
                          static_cast<unsigned>(source.user), source.port,
                          static_cast<unsigned>(source.handle));
        }
        if (source.handle >= 0)
        {
            source.open_failures = 0;
            arm_remote(source);
            continue;
        }
        // Stagger them so a console with no remote does not pay for all three
        // on one frame.
        ++source.open_failures;
        const std::uint64_t wait =
            source.open_failures >= kSlowAfterFailures ? kRetrySlowFrames : kRetryFrames;
        source.next_open_frame = frame_ + wait + static_cast<std::uint64_t>(i) * 100;
    }
}

void Pad::arm_remote(RemoteSource &source)
{
    // A key already down when the source opens is ignored until it is let go.
    // Decoded without the intercept filter, so a key held under the system's
    // menu is ignored too.
    source.buttons = 0;
    source.ignore = 0;
    source.connected = false;
    source.snapshot_failures = 0;
    // The snapshot below plus a batch read of one handle in one frame is the
    // mix RECIPES.md warns about, so this source sits out this frame.
    source.armed_frame = frame_;
    if (source.handle < 0)
        return;
    RawPadSample state;
    std::memset(&state, 0, sizeof state);
    if (scePadReadState(source.handle, &state) == 0)
        source.ignore = remote_bits(state, false);
}

bool Pad::any_remote_open() const
{
    for (const RemoteSource &source : remotes_)
    {
        if (source.handle >= 0)
            return true;
    }
    return false;
}

void Pad::read_remote(RemoteSource &source, bool system_screen, std::uint32_t *taps)
{
    // One read method per handle per frame (RECIPES.md): port 16 queues
    // samples, port 2 answers only the snapshot.
    RawPadSample samples[16];
    int count = 0;
    if (source.snapshot_only)
    {
        std::memset(&samples[0], 0, sizeof samples[0]);
        count = scePadReadState(source.handle, &samples[0]) == 0 ? 1 : 0;
        if (count != 0)
        {
            source.snapshot_failures = 0;
        }
        else if (++source.snapshot_failures >= kSnapshotDeadFrames)
        {
            source.buttons = 0;
            source.connected = false;
        }
    }
    else
    {
        // Nothing queued means nothing changed: a batch port sends its own
        // releases.
        count = scePadRead(source.handle, samples, 16);
        if (count < 0 || count > 16)
            count = 0;
    }
    for (int i = 0; i < count; ++i)
    {
        const std::uint32_t bits = remote_bits(samples[i], system_screen);
        source.ignore &= bits; // forgiven one key at a time, once it reads up
        const std::uint32_t value = bits & ~source.ignore;
        *taps |= value & ~source.buttons;
        source.buttons = value;
        // A source sending keys counts as connected even if it does not say so.
        source.connected = samples[i].connected != 0 || value != 0;
    }
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
        if (owns_pad_)
            scePadClose(handle_);
    }
    handle_ = -1;
    owns_pad_ = false;
    for (RemoteSource &source : remotes_)
    {
        // A borrowed handle belongs to whoever opened it.
        if (source.handle >= 0 && source.owned)
            scePadClose(source.handle);
        source = RemoteSource{};
    }
    last_remote_held_ = 0;
    frame_ = 0;
    if (owns_user_service_)
        sceUserServiceTerminate();
    owns_user_service_ = false;
}

std::size_t Pad::read(std::span<PadSample> out)
{
    if (out.empty())
        return 0;
    ++frame_;
    open_remote(); // opens nothing while every source is open or backing off
    if (handle_ < 0 && !any_remote_open())
    {
        out[0] = PadSample{};
        return 1;
    }

    // Two slots are kept free for a key pressed and let go inside this frame.
    const std::size_t room = out.size() > 2 ? out.size() - 2 : out.size();
    RawPadSample raw[64];
    const int limit = static_cast<int>(room < 64 ? room : 64);
    int count = handle_ >= 0 ? scePadRead(handle_, raw, limit) : 0;
    if (count < 0 || count > limit)
        count = 0;

    // Remote buttons merge in as held state, so the other device's sample in
    // between never reads as a release. Each source keeps its own level.
    const bool system_screen = system_has_screen();
    std::uint32_t remote_held = 0;
    std::uint32_t remote_taps = 0;
    bool remote_connected = false;
    for (RemoteSource &source : remotes_)
    {
        if (source.handle < 0 || source.armed_frame == frame_)
            continue;
        read_remote(source, system_screen, &remote_taps);
        remote_held |= source.buttons;
        remote_connected = remote_connected || source.connected;
    }
    remote_taps &= ~remote_held; // a tap only if nothing still holds the key

    if (count == 0 && (remote_held != last_remote_held_ || remote_taps != 0))
    {
        // The controller sent nothing this frame: the remote's change still
        // arrives, on the controller's last state.
        raw[0] = last_sample();
        count = 1;
    }
    for (int i = 0; i < count; ++i)
    {
        raw[i].buttons |= remote_held;
        // Without a controller the samples say "disconnected", and input from
        // a disconnected pad is ignored: the remote alone counts.
        if (remote_connected)
            raw[i].connected = 1;
    }
    if (count > 0)
    {
        last_sample() = raw[count - 1];
        last_sample().buttons &= ~remote_held;
    }
    last_remote_held_ = remote_held;
    // The controller may hold the same button; the release below must not
    // take it away.
    remote_taps &= ~last_sample().buttons;

    std::size_t written = 0;
    for (int index = 0; index < count; ++index)
        copy_sample(raw[index], out[written++]);
    if (remote_taps != 0 && written + 2 <= out.size())
    {
        // Pressed and let go between frames: both edges go out as samples.
        RawPadSample tap = last_sample();
        tap.buttons |= remote_held | remote_taps;
        if (remote_connected)
            tap.connected = 1;
        copy_sample(tap, out[written++]);
        tap.buttons &= ~remote_taps;
        copy_sample(tap, out[written++]);
    }
    return written;
}

} // namespace hui::ps5

// ps5-homebrew-ui - The console pad layer, driven against a fake scePad.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// src/platform/ps5/pad.cpp is console-only and is not in tests/unit/sources.txt,
// so it is included here and linked against the fakes below. What is under test
// is the part a console cannot easily be asked about: which remote ports are
// opened, how a key code becomes a button, and the edges the merge produces.

#include "platform/ps5/pad.cpp" // NOLINT(bugprone-suspicious-include)

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace
{

using hui::PadSample;
namespace bits = hui::pad_bits;

enum
{
    kUserId = 7,
    kStandardHandle = 10,
    kUser16Handle = 20,
    kSystem16Handle = 21,
    kSpecialHandle = 22,
};

// One fake pad port.
struct FakePort
{
    bool available = false;
    bool already_open = false; // scePadOpen refuses; the handle can be borrowed
    bool state_fails = false;
    RawPadSample state{};
    std::vector<RawPadSample> queue; // handed out by scePadRead, then emptied
    int opens = 0;
    int closes = 0;
    int borrows = 0;
};

FakePort g_standard;
FakePort g_user16;
FakePort g_system16;
FakePort g_special;
bool g_system_overlay = false;

FakePort *port_for_handle(int handle)
{
    switch (handle)
    {
    case kStandardHandle:
        return &g_standard;
    case kUser16Handle:
        return &g_user16;
    case kSystem16Handle:
        return &g_system16;
    case kSpecialHandle:
        return &g_special;
    default:
        return nullptr;
    }
}

struct Candidate
{
    FakePort *port;
    int handle;
};

Candidate candidate_for(int user, int type)
{
    if (type == kPortStandard)
        return {&g_standard, kStandardHandle};
    if (type == kPortRemoteControl && user == kSystemUser)
        return {&g_system16, kSystem16Handle};
    if (type == kPortRemoteControl)
        return {&g_user16, kUser16Handle};
    if (type == kPortSpecial)
        return {&g_special, kSpecialHandle};
    return {nullptr, -1};
}

RawPadSample make_sample(std::uint32_t buttons, unsigned key = 0, int connected = 1)
{
    RawPadSample sample{};
    sample.left_x = sample.left_y = sample.right_x = sample.right_y = 128;
    sample.buttons = buttons;
    sample.connected = connected;
    sample.remote_keys[kRemoteKeyIndex] = static_cast<std::uint8_t>(key);
    return sample;
}

void reset()
{
    g_standard = FakePort{};
    g_user16 = FakePort{};
    g_system16 = FakePort{};
    g_special = FakePort{};
    g_system_overlay = false;
    g_standard.state = make_sample(0);
    g_user16.state = make_sample(0);
    g_system16.state = make_sample(0);
    g_special.state = make_sample(0);
}

// The buttons of the last sample a read produced, 0 when it produced none.
std::uint32_t last_buttons(const std::vector<PadSample> &samples, std::size_t count)
{
    return count == 0 ? 0u : samples[count - 1].buttons;
}

// True when some sample in the read holds every bit of mask.
bool any_holds(const std::vector<PadSample> &samples, std::size_t count, std::uint32_t mask)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        if ((samples[i].buttons & mask) == mask)
            return true;
    }
    return false;
}

} // namespace

extern "C"
{
    int sceUserServiceInitialize(const void *)
    {
        return 0;
    }
    int sceUserServiceTerminate(void)
    {
        return 0;
    }
    int sceUserServiceGetInitialUser(int *user)
    {
        *user = kUserId;
        return 0;
    }
    int scePadInit(void)
    {
        return 0;
    }

    int scePadOpen(int user, int type, int index, const void *parameters)
    {
        EXPECT_EQ(index, 0);
        EXPECT_EQ(parameters, nullptr);
        const Candidate candidate = candidate_for(user, type);
        if (candidate.port == nullptr)
            return -1;
        ++candidate.port->opens;
        if (candidate.port->already_open)
            return kPadAlreadyOpened;
        return candidate.port->available ? candidate.handle : static_cast<int>(0x80920001);
    }

    int scePadGetHandle(int user, int type, int index)
    {
        EXPECT_EQ(index, 0);
        const Candidate candidate = candidate_for(user, type);
        if (candidate.port == nullptr)
            return -1;
        ++candidate.port->borrows;
        return candidate.port->available ? candidate.handle : -1;
    }

    int scePadRead(int handle, RawPadSample *samples, int count)
    {
        FakePort *port = port_for_handle(handle);
        if (port == nullptr || count <= 0)
            return -1;
        const int given = static_cast<int>(port->queue.size()) < count
                              ? static_cast<int>(port->queue.size())
                              : count;
        for (int i = 0; i < given; ++i)
            samples[i] = port->queue[static_cast<std::size_t>(i)];
        port->queue.clear();
        return given;
    }

    int scePadReadState(int handle, RawPadSample *state)
    {
        FakePort *port = port_for_handle(handle);
        if (port == nullptr || port->state_fails)
            return -1;
        *state = port->state;
        return 0;
    }

    int scePadClose(int handle)
    {
        FakePort *port = port_for_handle(handle);
        if (port != nullptr)
            ++port->closes;
        return 0;
    }

    int scePadSetVibrationMode(int, int)
    {
        return 0;
    }
    int scePadSetVibration(int, const PadVibration *)
    {
        return 0;
    }
    int scePadSetLightBar(int, const PadColor *)
    {
        return 0;
    }

    int sceSystemServiceGetStatus(SystemServiceStatus *status)
    {
        status->system_ui_overlaid = g_system_overlay ? 1 : 0;
        return 0;
    }
}

namespace
{

TEST(Pad, OpensEveryCandidateRemotePortNotJustTheFirst)
{
    reset();
    g_standard.available = true;
    g_user16.available = true; // the one that used to win and stop the search
    g_system16.available = true;
    g_special.available = true;
    hui::ps5::Pad pad;
    ASSERT_TRUE(pad.open());
    EXPECT_EQ(g_user16.opens, 1);
    EXPECT_EQ(g_system16.opens, 1);
    EXPECT_EQ(g_special.opens, 1);
    pad.close();
}

TEST(Pad, OkKeyArrivesAsCross)
{
    reset();
    g_standard.available = true;
    g_system16.available = true;
    hui::ps5::Pad pad;
    ASSERT_TRUE(pad.open());
    // The remote's OK is in no button bit: only in the key byte.
    g_system16.queue.push_back(make_sample(0, 13));
    std::vector<PadSample> out(64);
    const std::size_t count = pad.read(out);
    EXPECT_NE(last_buttons(out, count) & bits::kCross, 0u);
    pad.close();
}

TEST(Pad, PlayPauseAndTransportKeysArrive)
{
    const struct
    {
        unsigned key;
        std::uint32_t bit;
    } keys[] = {{45, bits::kPlayPause}, {22, bits::kPlayPause}, {26, bits::kPlayPause},
                {15, bits::kCircle},    {23, bits::kL1},        {24, bits::kR1}};
    for (const auto &entry : keys)
    {
        reset();
        g_standard.available = true;
        g_system16.available = true;
        hui::ps5::Pad pad;
        ASSERT_TRUE(pad.open());
        g_system16.queue.push_back(make_sample(0, entry.key));
        std::vector<PadSample> out(64);
        std::size_t count = pad.read(out);
        EXPECT_NE(last_buttons(out, count) & entry.bit, 0u) << "key " << entry.key;
        // Letting go releases it.
        g_system16.queue.push_back(make_sample(0, 0));
        count = pad.read(out);
        EXPECT_EQ(last_buttons(out, count) & entry.bit, 0u) << "key " << entry.key;
        pad.close();
    }
}

TEST(Pad, SpecialPortIsReadThroughItsSnapshot)
{
    // The special port queues nothing at all: without the snapshot it is mute,
    // which is what hid a Bluetooth remote behind a port that merely opened.
    reset();
    g_special.available = true; // no controller, no remote-control port
    hui::ps5::Pad pad;
    ASSERT_TRUE(pad.open());
    g_special.state = make_sample(0, 45);
    std::vector<PadSample> out(64);
    const std::size_t count = pad.read(out);
    ASSERT_GT(count, 0u);
    EXPECT_NE(last_buttons(out, count) & bits::kPlayPause, 0u);
    EXPECT_TRUE(out[count - 1].connected);
    pad.close();
}

TEST(Pad, KeyHeldWhenTheSourceOpensIsIgnoredUntilLetGo)
{
    reset();
    g_standard.available = true;
    g_system16.available = true;
    g_system16.state = make_sample(0, 13); // OK already down at open
    hui::ps5::Pad pad;
    ASSERT_TRUE(pad.open());
    std::vector<PadSample> out(64);
    g_system16.queue.push_back(make_sample(0, 13));
    std::size_t count = pad.read(out);
    EXPECT_EQ(last_buttons(out, count) & bits::kCross, 0u);
    // Let go, then press for real.
    g_system16.queue.push_back(make_sample(0, 0));
    pad.read(out);
    g_system16.queue.push_back(make_sample(0, 13));
    count = pad.read(out);
    EXPECT_NE(last_buttons(out, count) & bits::kCross, 0u);
    pad.close();
}

TEST(Pad, TapInsideOneFrameKeepsItsPressAndRelease)
{
    reset();
    g_standard.available = true;
    g_system16.available = true;
    hui::ps5::Pad pad;
    ASSERT_TRUE(pad.open());
    // Pressed and let go before the app looked: both edges must survive.
    g_system16.queue.push_back(make_sample(0, 13));
    g_system16.queue.push_back(make_sample(0, 0));
    std::vector<PadSample> out(64);
    const std::size_t count = pad.read(out);
    EXPECT_TRUE(any_holds(out, count, bits::kCross));
    EXPECT_EQ(last_buttons(out, count) & bits::kCross, 0u);
    pad.close();
}

TEST(Pad, AKeyTheControllerIsHoldingIsNotTakenAwayByARemoteTap)
{
    reset();
    g_standard.available = true;
    g_system16.available = true;
    hui::ps5::Pad pad;
    ASSERT_TRUE(pad.open());
    std::vector<PadSample> out(64);
    g_standard.queue.push_back(make_sample(bits::kCross)); // the hand on X
    g_system16.queue.push_back(make_sample(0, 13));        // and a remote tap
    g_system16.queue.push_back(make_sample(0, 0));
    const std::size_t count = pad.read(out);
    ASSERT_GT(count, 0u);
    EXPECT_NE(last_buttons(out, count) & bits::kCross, 0u);
    pad.close();
}

TEST(Pad, InterceptedKeysAreTheSystemsOnlyWhileItHasTheScreen)
{
    reset();
    g_standard.available = true;
    g_system16.available = true;
    hui::ps5::Pad pad;
    ASSERT_TRUE(pad.open());
    std::vector<PadSample> out(64);
    // With the system's menu up the key is the system's.
    g_system_overlay = true;
    g_system16.queue.push_back(make_sample(bits::kIntercepted, 13));
    std::size_t count = pad.read(out);
    EXPECT_EQ(last_buttons(out, count) & bits::kCross, 0u);
    // With the app in front the same sample is ours - this is the whole reason
    // OK and play/pause did nothing before.
    g_system_overlay = false;
    g_system16.queue.push_back(make_sample(bits::kIntercepted, 13));
    count = pad.read(out);
    EXPECT_NE(last_buttons(out, count) & bits::kCross, 0u);
    EXPECT_EQ(last_buttons(out, count) & bits::kIntercepted, 0u);
    pad.close();
}

TEST(Pad, ASourceThatSaysDisconnectedStillDeliversItsRelease)
{
    reset();
    g_standard.available = true;
    g_system16.available = true;
    hui::ps5::Pad pad;
    ASSERT_TRUE(pad.open());
    std::vector<PadSample> out(64);
    g_system16.queue.push_back(make_sample(0, 13));
    std::size_t count = pad.read(out);
    ASSERT_NE(last_buttons(out, count) & bits::kCross, 0u);
    // The release arrives on a sample that reports the source disconnected;
    // skipping it is what used to leave the button down for good.
    g_system16.queue.push_back(make_sample(0, 0, 0));
    count = pad.read(out);
    EXPECT_EQ(last_buttons(out, count) & bits::kCross, 0u);
    pad.close();
}

TEST(Pad, ASnapshotPortThatStopsAnsweringReleasesWhatItHeld)
{
    reset();
    g_special.available = true;
    hui::ps5::Pad pad;
    ASSERT_TRUE(pad.open());
    std::vector<PadSample> out(64);
    g_special.state = make_sample(0, 13);
    std::size_t count = pad.read(out);
    ASSERT_NE(last_buttons(out, count) & bits::kCross, 0u);
    g_special.state_fails = true;
    for (int i = 0; i < kSnapshotDeadFrames + 2; ++i)
        count = pad.read(out);
    EXPECT_EQ(last_buttons(out, count) & bits::kCross, 0u);
    pad.close();
}

TEST(Pad, ABorrowedHandleIsNotClosed)
{
    reset();
    g_standard.available = true;
    g_system16.available = true;
    g_system16.already_open = true; // something else holds it
    hui::ps5::Pad pad;
    ASSERT_TRUE(pad.open());
    EXPECT_EQ(g_system16.borrows, 1);
    // It still delivers keys...
    g_system16.queue.push_back(make_sample(0, 13));
    std::vector<PadSample> out(64);
    const std::size_t count = pad.read(out);
    EXPECT_NE(last_buttons(out, count) & bits::kCross, 0u);
    pad.close();
    // ...but closing it is not ours to do.
    EXPECT_EQ(g_system16.closes, 0);
    EXPECT_EQ(g_standard.closes, 1);
}

TEST(Pad, AConsoleWithNoRemoteKeepsWorkingAndStopsAsking)
{
    reset();
    g_standard.available = true;
    hui::ps5::Pad pad;
    ASSERT_TRUE(pad.open());
    const int opens_after_open = g_system16.opens;
    EXPECT_EQ(opens_after_open, 1);
    std::vector<PadSample> out(64);
    for (int i = 0; i < 100; ++i)
        pad.read(out);
    // Back off rather than ask every frame.
    EXPECT_EQ(g_system16.opens, opens_after_open);
    g_standard.queue.push_back(make_sample(bits::kCross));
    const std::size_t count = pad.read(out);
    EXPECT_NE(last_buttons(out, count) & bits::kCross, 0u);
    pad.close();
}
} // namespace

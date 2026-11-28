// ps5-homebrew-ui - Controller input model tests.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/input.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace
{

using hui::Action;
using hui::Direction;
using hui::InputTracker;
using hui::PadSample;
namespace bits = hui::pad_bits;

PadSample sample(std::uint32_t buttons, std::uint8_t lx = 128, std::uint8_t ly = 128)
{
    PadSample s;
    s.buttons = buttons;
    s.left_x = lx;
    s.left_y = ly;
    s.connected = true;
    return s;
}

TEST(Input, KeepsATapThatStartsAndEndsBetweenFrames)
{
    InputTracker tracker;
    const std::vector<PadSample> samples = {sample(0), sample(bits::kCross), sample(0)};
    const auto frame = tracker.update(samples, 0);
    EXPECT_TRUE(frame.is_pressed(Action::confirm));
    EXPECT_FALSE(frame.is_held(Action::confirm));
    EXPECT_NE(frame.released & hui::action_bit(Action::confirm), 0u);
}

TEST(Input, SwapsConfirmAndBack)
{
    InputTracker tracker;
    hui::InputSettings settings;
    settings.swap_confirm = true;
    tracker.set_settings(settings);
    const std::vector<PadSample> samples = {sample(bits::kCircle)};
    const auto frame = tracker.update(samples, 0);
    EXPECT_TRUE(frame.is_pressed(Action::confirm));
    EXPECT_FALSE(frame.is_pressed(Action::back));
}

TEST(Input, NavigationFiresThenRepeatsAfterDelay)
{
    InputTracker tracker;
    const std::vector<PadSample> down = {sample(bits::kDown)};
    auto frame = tracker.update(down, 0);
    EXPECT_EQ(frame.nav, Direction::down);
    EXPECT_FALSE(frame.nav_repeat);
    frame = tracker.update(down, 200000);
    EXPECT_EQ(frame.nav, Direction::none);
    frame = tracker.update(down, 350000);
    EXPECT_EQ(frame.nav, Direction::down);
    EXPECT_TRUE(frame.nav_repeat);
    frame = tracker.update(down, 400000);
    EXPECT_EQ(frame.nav, Direction::none);
    frame = tracker.update(down, 460000);
    EXPECT_EQ(frame.nav, Direction::down);
    const std::vector<PadSample> none = {sample(0)};
    frame = tracker.update(none, 470000);
    EXPECT_EQ(frame.nav, Direction::none);
}

TEST(Input, QuickDirectionTapNavigatesOnce)
{
    InputTracker tracker;
    const std::vector<PadSample> tap = {sample(bits::kRight), sample(0)};
    const auto frame = tracker.update(tap, 0);
    EXPECT_EQ(frame.nav, Direction::right);
}

TEST(Input, StickActsAsDpadBeyondThreshold)
{
    EXPECT_EQ(InputTracker::stick_direction(sample(0, 128, 128)), Direction::none);
    EXPECT_EQ(InputTracker::stick_direction(sample(0, 100, 150)), Direction::none);
    EXPECT_EQ(InputTracker::stick_direction(sample(0, 10, 100)), Direction::left);
    EXPECT_EQ(InputTracker::stick_direction(sample(0, 250, 128)), Direction::right);
    EXPECT_EQ(InputTracker::stick_direction(sample(0, 140, 5)), Direction::up);
    EXPECT_EQ(InputTracker::stick_direction(sample(0, 128, 255)), Direction::down);
}

TEST(Input, StickDeadzoneAndScaling)
{
    InputTracker tracker;
    std::vector<PadSample> centre = {sample(0, 135, 124)};
    auto frame = tracker.update(centre, 0);
    EXPECT_FLOAT_EQ(frame.stick_x, 0.0f);
    EXPECT_FLOAT_EQ(frame.stick_y, 0.0f);
    std::vector<PadSample> full = {sample(0, 255, 128)};
    frame = tracker.update(full, 0);
    EXPECT_NEAR(frame.stick_x, 1.0f, 0.01f);
    EXPECT_NEAR(frame.stick_y, 0.0f, 0.01f);
}

TEST(Input, InterceptOrDisconnectReleasesEverythingAndLosesFocus)
{
    InputTracker tracker;
    const std::vector<PadSample> hold = {sample(bits::kCross | bits::kL1)};
    tracker.update(hold, 0);
    PadSample intercepted = sample(bits::kIntercepted | bits::kCross);
    const std::vector<PadSample> overlay = {intercepted};
    auto frame = tracker.update(overlay, 1000);
    EXPECT_TRUE(frame.focus_lost);
    EXPECT_EQ(frame.held, 0u);
    EXPECT_TRUE(frame.released & hui::action_bit(Action::confirm));

    PadSample unplugged;
    unplugged.connected = false;
    const std::vector<PadSample> gone = {unplugged};
    frame = tracker.update(gone, 2000);
    EXPECT_FALSE(frame.connected);
}

TEST(Input, FocusLostFiresOnceWhileOverlayOwnsInput)
{
    InputTracker tracker;
    const std::vector<PadSample> idle = {sample(0)};
    tracker.update(idle, 0);
    const std::vector<PadSample> overlay = {sample(bits::kIntercepted)};
    EXPECT_TRUE(tracker.update(overlay, 1000).focus_lost);
    EXPECT_FALSE(tracker.update(overlay, 2000).focus_lost);
    tracker.update(idle, 3000);
    EXPECT_TRUE(tracker.update(overlay, 4000).focus_lost);
}

TEST(Input, AnalogTriggersMapToJumps)
{
    PadSample s = sample(0);
    s.r2 = 200;
    const auto actions = InputTracker::map_buttons(s, false);
    EXPECT_NE(actions & hui::action_bit(Action::jump_next), 0u);
    EXPECT_EQ(actions & hui::action_bit(Action::jump_prev), 0u);
}

// ---- The media remote ----
//
// Its keys are not in the button word: each is a key code in a byte of its own,
// which hui::remote_key_bits turns into pad bits before anything else sees it.

TEST(Input, RemoteKeyCodesMapToPadBits)
{
    EXPECT_EQ(hui::remote_key_bits(13), bits::kCross);  // OK
    EXPECT_EQ(hui::remote_key_bits(15), bits::kCircle); // back
    EXPECT_EQ(hui::remote_key_bits(22), bits::kPlayPause);
    EXPECT_EQ(hui::remote_key_bits(26), bits::kPlayPause);
    EXPECT_EQ(hui::remote_key_bits(45), bits::kPlayPause);
    EXPECT_EQ(hui::remote_key_bits(23), bits::kL1); // rewind
    EXPECT_EQ(hui::remote_key_bits(24), bits::kR1); // fast forward
}

TEST(Input, UnmappedRemoteKeysAreIgnored)
{
    // 0 is what the remote sends while no key is down.
    EXPECT_EQ(hui::remote_key_bits(0), 0u);
    const unsigned unmapped[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,  14, 16,
                                 17, 18, 19, 20, 21, 25, 27, 28, 29, 30, 31, 32,  33, 34,
                                 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 46, 200, 255};
    for (unsigned key : unmapped)
        EXPECT_EQ(hui::remote_key_bits(static_cast<std::uint8_t>(key)), 0u) << "key " << key;
}

TEST(Input, RemoteKeysNeverClaimToBeIntercepted)
{
    for (unsigned key = 0; key <= 255; ++key)
    {
        const auto decoded = hui::remote_key_bits(static_cast<std::uint8_t>(key));
        EXPECT_EQ(decoded & bits::kIntercepted, 0u) << "key " << key;
    }
    EXPECT_EQ(bits::kPlayPause & bits::kIntercepted, 0u);
    EXPECT_LT(static_cast<unsigned>(Action::count), 32u);
}

TEST(Input, RemoteOkFollowsSwapConfirm)
{
    const PadSample ok = sample(hui::remote_key_bits(13));
    EXPECT_NE(InputTracker::map_buttons(ok, false) & hui::action_bit(Action::confirm), 0u);
    EXPECT_NE(InputTracker::map_buttons(ok, true) & hui::action_bit(Action::back), 0u);
}

TEST(Input, PlayPauseBitBecomesItsOwnAction)
{
    const auto actions = InputTracker::map_buttons(sample(bits::kPlayPause), false);
    EXPECT_EQ(actions, hui::action_bit(Action::play_pause));
}

TEST(Input, RemoteRewindAndForwardSeek)
{
    EXPECT_EQ(InputTracker::map_buttons(sample(hui::remote_key_bits(23)), false),
              hui::action_bit(Action::page_prev));
    EXPECT_EQ(InputTracker::map_buttons(sample(hui::remote_key_bits(24)), false),
              hui::action_bit(Action::page_next));
}

TEST(Input, RemotePlayPausePressesAndReleases)
{
    InputTracker tracker;
    const std::vector<PadSample> held = {sample(bits::kPlayPause)};
    auto frame = tracker.update(held, 0);
    EXPECT_TRUE(frame.is_pressed(Action::play_pause));
    EXPECT_TRUE(frame.is_held(Action::play_pause));
    EXPECT_EQ(frame.nav, Direction::none);
    // A level that does not change is not a second press.
    frame = tracker.update(held, 1000);
    EXPECT_FALSE(frame.is_pressed(Action::play_pause));
    EXPECT_TRUE(frame.is_held(Action::play_pause));
    const std::vector<PadSample> up = {sample(0)};
    frame = tracker.update(up, 2000);
    EXPECT_NE(frame.released & hui::action_bit(Action::play_pause), 0u);
    EXPECT_FALSE(frame.is_held(Action::play_pause));
}

TEST(Input, RemotePlayPauseTapInsideOneFrame)
{
    // What Pad::read sends when a key was pressed and let go between frames.
    InputTracker tracker;
    const std::vector<PadSample> tap = {sample(0), sample(bits::kPlayPause), sample(0)};
    const auto frame = tracker.update(tap, 0);
    EXPECT_TRUE(frame.is_pressed(Action::play_pause));
    EXPECT_FALSE(frame.is_held(Action::play_pause));
    EXPECT_NE(frame.released & hui::action_bit(Action::play_pause), 0u);
}

TEST(Input, RemoteInputIsOursUnlessSystemHasScreen)
{
    EXPECT_TRUE(hui::remote_input_is_ours(bits::kCross, false));
    EXPECT_TRUE(hui::remote_input_is_ours(bits::kCross, true));
    // Intercepted is the system's only while the system has the screen.
    EXPECT_TRUE(hui::remote_input_is_ours(bits::kIntercepted | bits::kCross, false));
    EXPECT_FALSE(hui::remote_input_is_ours(bits::kIntercepted | bits::kCross, true));
}

} // namespace

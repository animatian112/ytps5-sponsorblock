// ps5-homebrew-ui - Audio output thread through sceAudioOut.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/audio_out.hpp"

#include "platform/ps5/system.hpp"

extern "C"
{
    int sceAudioOutInit(void);
    int sceAudioOutOpen(int user, int type, int index, unsigned int grain_frames,
                        unsigned int frequency, unsigned int format);
    int sceAudioOutOutput(int handle, const void *samples);
    int sceAudioOutClose(int handle);
}

namespace hui::ps5
{

namespace
{

constexpr int kAlreadyInitialized = static_cast<int>(0x8026000e);
constexpr int kSystemUser = 0xff;
constexpr int kPortMain = 0;
constexpr unsigned int kFormatS16Stereo = 1;

} // namespace

AudioOut::~AudioOut()
{
    stop();
}

bool AudioOut::start(audio::Mixer &mixer)
{
    const int init = sceAudioOutInit();
    if (init != 0 && init != kAlreadyInitialized)
    {
        sys::log("[HUI] audio sceAudioOutInit=0x%08x", static_cast<unsigned>(init));
        return false;
    }
    handle_ = sceAudioOutOpen(kSystemUser, kPortMain, 0, kGrainFrames, audio::kSampleRate,
                              kFormatS16Stereo);
    if (handle_ < 0)
    {
        sys::log("[HUI] audio sceAudioOutOpen=0x%08x", static_cast<unsigned>(handle_));
        handle_ = -1;
        return false;
    }
    mixer_ = &mixer;
    stop_.store(false);
    // Never name threads: pthread_setname_np hangs on this platform.
    if (pthread_create(&thread_, nullptr, &AudioOut::thread_main, this) != 0)
    {
        sys::log("[HUI] audio thread creation failed");
        sceAudioOutClose(handle_);
        handle_ = -1;
        return false;
    }
    running_ = true;
    sys::log("[HUI] audio open handle=%d grain=%d rate=%d", handle_, kGrainFrames,
             audio::kSampleRate);
    return true;
}

void *AudioOut::thread_main(void *self)
{
    static_cast<AudioOut *>(self)->run();
    return nullptr;
}

void AudioOut::run()
{
    alignas(64) std::int16_t grain[kGrainFrames * 2];
    bool logged_first = false;
    std::int64_t last_error_log = 0;
    while (!stop_.load(std::memory_order_relaxed))
    {
        mixer_->render(grain, kGrainFrames);
        const int result = sceAudioOutOutput(handle_, grain);
        if (result < 0)
        {
            const std::uint64_t errors = errors_.fetch_add(1, std::memory_order_relaxed) + 1;
            // The first failure at once, then at most one line per second:
            // without this a dead port only shows as a rising counter.
            const std::int64_t now = sys::monotonic_us();
            if (errors == 1 || now - last_error_log >= 1000000)
            {
                last_error_log = now;
                sys::log("[HUI] audio write failed=0x%08x errors=%llu grains=%llu",
                         static_cast<unsigned>(result), static_cast<unsigned long long>(errors),
                         static_cast<unsigned long long>(grains_.load(std::memory_order_relaxed)));
            }
            sys::sleep_us(5000);
            continue;
        }
        grains_.fetch_add(1, std::memory_order_relaxed);
        if (!logged_first)
        {
            // One line that proves the port is fed and accepts the mix, so a
            // later silence can be traced past the output stage.
            logged_first = true;
            sys::log("[HUI] audio write ok handle=%d grain=%d rate=%d", handle_, kGrainFrames,
                     audio::kSampleRate);
        }
    }
    sceAudioOutOutput(handle_, nullptr); // drain the queued grain
    sys::log("[HUI] audio thread stopped grains=%llu errors=%llu",
             static_cast<unsigned long long>(grains_.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(errors_.load(std::memory_order_relaxed)));
}

void AudioOut::stop()
{
    if (running_)
    {
        stop_.store(true);
        pthread_join(thread_, nullptr);
        running_ = false;
    }
    if (handle_ >= 0)
        sceAudioOutClose(handle_);
    handle_ = -1;
}

} // namespace hui::ps5

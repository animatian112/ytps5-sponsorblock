// ps5-homebrew-ui - Lock-free ring of stereo float frames (one producer, one consumer).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hui::audio
{

// The game thread writes decoded music; the audio thread reads it. Neither
// side blocks: a short read means the producer fell behind (silence fills in).
class StreamRing
{
  public:
    explicit StreamRing(std::size_t frames = 1u << 16) : samples_(frames * 2), capacity_(frames)
    {
    }

    std::size_t capacity() const
    {
        return capacity_;
    }
    // Frames buffered and not yet read (a pending drain already folded in).
    std::size_t available() const
    {
        std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t cut = drop_until_.load(std::memory_order_acquire);
        if (tail < cut)
        {
            tail = cut;
        }
        const std::size_t head = head_.load(std::memory_order_acquire);
        return head > tail ? head - tail : 0u;
    }
    std::size_t space() const
    {
        const std::size_t buffered = available();
        return buffered >= capacity_ ? 0u : capacity_ - buffered;
    }

    // Producer: copies up to frames stereo frames; returns how many fit.
    std::size_t write(const float *stereo, std::size_t frames)
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t cut = drop_until_.load(std::memory_order_acquire);
        if (tail < cut)
        {
            tail = cut;
        }
        // Failure scenario: while two threads stored tail_ (read() and the old
        // drain()), a read that finished after a drain moved tail_ back behind
        // the drop, head - tail then grew past the capacity, this subtraction
        // wrapped, and a whole batch was written over unread frames. The room
        // is clamped so a wrapped count can never happen; an inconsistent pair
        // can only make the write smaller.
        const std::size_t used = head > tail ? head - tail : 0u;
        const std::size_t room = used >= capacity_ ? 0u : capacity_ - used;
        const std::size_t count = std::min(frames, room);
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::size_t at = ((head + i) % capacity_) * 2;
            samples_[at] = stereo[i * 2];
            samples_[at + 1] = stereo[i * 2 + 1];
        }
        head_.store(head + count, std::memory_order_release);
        written_.fetch_add(count, std::memory_order_relaxed);
        return count;
    }

    // Consumer: copies up to frames stereo frames; returns how many were there.
    std::size_t read(float *stereo, std::size_t frames)
    {
        // Paused: nothing is consumed, so the frames already decoded wait for
        // the resume instead of playing on past the picture.
        if (paused_.load(std::memory_order_acquire))
            return 0;
        // The consumer is the only thread that stores tail_. A drain() posted
        // from the producer's thread is applied here as a cut instead of
        // moving tail_ there, so the two sides can never race on it: tail_
        // only ever moves forward, and frames written after the drain (at or
        // beyond the cut) survive it.
        std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t cut = drop_until_.load(std::memory_order_acquire);
        if (tail < cut)
        {
            tail = cut;
        }
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t count = std::min(frames, head > tail ? head - tail : 0u);
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::size_t at = ((tail + i) % capacity_) * 2;
            stereo[i * 2] = samples_[at];
            stereo[i * 2 + 1] = samples_[at + 1];
        }
        tail_.store(tail + count, std::memory_order_release);
        read_.fetch_add(count, std::memory_order_relaxed);
        return count;
    }

    // Drops everything written so far. Safe from the producer's thread (the
    // player calls it from the frame thread while the audio thread reads): it
    // only records the cut and read() applies it. Frames written after this
    // call survive, so a seek's fresh audio is never dropped with the old.
    void drain()
    {
        drop_until_.store(head_.load(std::memory_order_relaxed), std::memory_order_release);
    }

    // Holds the frames in place (read() hands out none) until unpaused.
    void set_paused(bool paused)
    {
        paused_.store(paused, std::memory_order_release);
    }
    bool paused() const
    {
        return paused_.load(std::memory_order_acquire);
    }

    // Lifetime frame counts for the log: written on the producer's side, read
    // on the consumer's, each carried by its own atomic.
    std::uint64_t total_written() const
    {
        return written_.load(std::memory_order_relaxed);
    }
    std::uint64_t total_read() const
    {
        return read_.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<bool> paused_{false};
    std::vector<float> samples_;
    std::size_t capacity_;
    std::atomic<std::size_t> head_{0};       // frames written, ever (producer only)
    std::atomic<std::size_t> tail_{0};       // frames read, ever (consumer only)
    std::atomic<std::size_t> drop_until_{0}; // frames to skip, set by drain()
    std::atomic<std::uint64_t> written_{0};
    std::atomic<std::uint64_t> read_{0};
};

} // namespace hui::audio

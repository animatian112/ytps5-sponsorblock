// youtube-tv - A live stream's chat, polled in the background.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/live_chat.hpp"

#include "yt/innertube.hpp"
#include "yt/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

#include <pthread.h>

namespace yt
{

struct LiveChat::State
{
    std::atomic<bool> stop{false};
    std::atomic<bool> found{false};
    std::atomic<bool> unavailable{false};
    mutable std::mutex mutex;
    std::deque<ChatMessage> messages;
};

namespace
{

void run(std::shared_ptr<LiveChat::State> state, std::string video_id)
{
    InnerTube client;
    client.set_timeout(10);
    std::string continuation;
    for (int attempt = 0; attempt < 3 && continuation.empty() && !state->stop; ++attempt)
    {
        if (client.live_chat_start(video_id, &continuation))
            break;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (continuation.empty())
    {
        log::line("chat: %s has no live chat", video_id.c_str());
        state->unavailable = true;
        return;
    }
    state->found = true;
    log::line("chat: %s chat found", video_id.c_str());
    int failures = 0;
    while (!state->stop)
    {
        std::vector<ChatMessage> fresh;
        std::string next;
        int wait_ms = 5000;
        if (client.live_chat(continuation, &fresh, &next, &wait_ms) && !next.empty())
        {
            failures = 0;
            continuation = next;
            std::lock_guard<std::mutex> lock(state->mutex);
            for (ChatMessage &message : fresh)
                state->messages.push_back(std::move(message));
            while (state->messages.size() > LiveChat::kKeep)
                state->messages.pop_front();
        }
        else if (++failures >= 5)
        {
            log::line("chat: %s stopped after repeated failures", video_id.c_str());
            state->unavailable = true;
            return;
        }
        // YouTube's pace, kept between 1.5 and 10 seconds, in small steps
        // so a stop is seen quickly.
        const int wait = std::clamp(wait_ms, 1500, 10000);
        for (int waited = 0; waited < wait && !state->stop; waited += 250)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

struct Start
{
    std::shared_ptr<LiveChat::State> state;
    std::string video_id;
};

void *entry(void *argument)
{
    std::unique_ptr<Start> start(static_cast<Start *>(argument));
    run(std::move(start->state), std::move(start->video_id));
    return nullptr;
}

} // namespace

LiveChat::~LiveChat()
{
    stop();
}

void LiveChat::start(const std::string &video_id)
{
    stop();
    video_id_ = video_id;
    if (video_id.empty())
        return;
    state_ = std::make_shared<State>();
    // Its own 1 MiB stack, as the service's workers (TLS needs it).
    auto *start = new Start{state_, video_id};
    pthread_attr_t attributes;
    pthread_t thread;
    bool created = false;
    if (pthread_attr_init(&attributes) == 0)
    {
        pthread_attr_setstacksize(&attributes, 1u << 20);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        created = pthread_create(&thread, &attributes, entry, start) == 0;
        pthread_attr_destroy(&attributes);
    }
    if (!created)
    {
        delete start;
        state_->unavailable = true;
    }
}

void LiveChat::stop()
{
    if (state_ != nullptr)
        state_->stop = true;
    state_.reset();
    video_id_.clear();
}

std::vector<ChatMessage> LiveChat::messages() const
{
    if (state_ == nullptr)
        return {};
    std::lock_guard<std::mutex> lock(state_->mutex);
    return {state_->messages.begin(), state_->messages.end()};
}

bool LiveChat::found() const
{
    return state_ != nullptr && state_->found;
}

bool LiveChat::unavailable() const
{
    return state_ != nullptr && state_->unavailable;
}

} // namespace yt

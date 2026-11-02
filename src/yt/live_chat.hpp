// youtube-tv - A live stream's chat, polled in the background.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// start() finds the chat of a live video (its WEB watch page) and keeps
// asking get_live_chat on a thread of its own, at the pace YouTube asks for;
// the player reads the newest messages each frame. stop() lets the thread
// go without waiting for a request in flight (the state is shared).

#pragma once

#include "yt/types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace yt
{

class LiveChat
{
  public:
    LiveChat() = default;
    ~LiveChat();
    LiveChat(const LiveChat &) = delete;
    LiveChat &operator=(const LiveChat &) = delete;

    void start(const std::string &video_id);
    void stop();
    const std::string &video_id() const
    {
        return video_id_;
    }

    // The newest messages, oldest first (at most kKeep), and whether the
    // chat was found at all (false while looking or when it has none).
    std::vector<ChatMessage> messages() const;
    bool found() const;
    bool unavailable() const;

    static constexpr std::size_t kKeep = 40;

    struct State; // shared with the polling thread

  private:
    std::shared_ptr<State> state_;
    std::string video_id_;
};

} // namespace yt

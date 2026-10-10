// youtube-tv - The PC build's stand-in for the console self-update platform.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console platform (src/platform/ps5/self_update_ps5.c) is sceNet* and sceKernel*
// against the payload loader and is not built for the PC. This file provides the one
// symbol the engine needs there, self_update_console(), declared in
// src/update/self_update.h, so the module links in the host snapshot and unit tests.
//
// Every entry point fails cleanly and touches nothing: the channel open reports
// "nothing listening on the payload loader" (the engine turns the 0 into its
// "no payload loader on port 9021" message), the download reports failure, and the
// clock is a monotonic millisecond value the engine can poll.
#include "update/self_update.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{

// The engine's platform download would run the HTTPS transfer; the PC build has no
// transport, so it fails cleanly (see host/update_http_stub.cpp).
int host_download(void *user, const char *url, std::uint64_t limit, self_update_sink sink,
                  void *sink_user)
{
    (void)user;
    (void)url;
    (void)limit;
    (void)sink;
    (void)sink_user;
    return -1; /* no update transport in the host build */
}

// There is no payload loader on loopback port 9021 here: 0 is the engine's "nothing
// answers", which it reports as "no payload loader on port 9021".
int host_open_helper(void *user, self_update_channel *channel)
{
    (void)user;
    if (channel != nullptr)
        std::memset(channel, 0, sizeof(*channel));
    return 0; /* no payload loader on port 9021 */
}

// Monotonic milliseconds from std::chrono::steady_clock, matching the console clock.
std::uint64_t host_now_ms(void *user)
{
    (void)user;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

} // namespace

// Declared in src/update/self_update.h inside extern "C"; the same linkage here so
// src/yt/update.cpp (host build) links.
extern "C" const self_update_platform *self_update_console(void)
{
    static const self_update_platform platform = {host_download, host_open_helper, host_now_ms,
                                                  nullptr};
    return &platform;
}

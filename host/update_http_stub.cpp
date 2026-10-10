// youtube-tv - The PC build's stand-in for the console update transport.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console transport (src/platform/ps5/update_http.c) is libcurl plus the console's
// certificate list and is not built for the PC. This file provides the two functions
// declared in src/update/update_check.h so the engine links in the host snapshot and unit
// tests: they touch nothing and report "no update transport in the host build", which keeps
// yt::update::check() harmless there.
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" int update_transport_get_json(const char *url, const char *user_agent,
                                         const char *accept, char **body, std::size_t *size,
                                         long *status)
{
    (void)url;
    (void)user_agent;
    (void)accept;
    if (body != nullptr)
        *body = nullptr;
    if (size != nullptr)
        *size = 0;
    if (status != nullptr)
        *status = 0;
    return -1; /* no update transport in the host build */
}

extern "C" int
update_transport_download(const char *url, std::uint64_t limit,
                          int (*sink)(void *user, const void *data, std::size_t size), void *user)
{
    (void)url;
    (void)limit;
    (void)sink;
    (void)user;
    return -1; /* no update transport in the host build */
}

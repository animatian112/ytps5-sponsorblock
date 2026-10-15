/* youtube-tv - A minimal DNS (A record) query builder and answer reader.
 * Copyright (C) 2026 SponsorBlock for PS5 contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Platform-neutral so it can be unit-tested; the console's lookup in
 * src/platform/ps5/console_curl.c sends the query over UDP. Answers that reach
 * the address through CNAME aliases (www.youtube.com -> youtube-ui.l.google.com)
 * are read correctly: every A record in the answer section is collected. */

#ifndef YOUTUBE_TV_DNS_H
#define YOUTUBE_TV_DNS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DNS_MAX_PACKET 1500

    /* Writes an A/IN query with recursion desired. Returns its length, or -1 for a
     * name that doesn't fit or isn't a hostname. */
    int dns_build_query(uint8_t *buffer, size_t capacity, uint16_t id, const char *name);

    /* Reads the answer to a query sent with `id`. Fills up to `max` IPv4 addresses
     * (network byte order) and returns how many, 0 when the name has none,
     * -1 for a malformed packet or another id, -2 - rcode when the server refused
     * (-5 = NXDOMAIN). */
    int dns_parse_a(const uint8_t *packet, size_t length, uint16_t id, uint32_t *addresses,
                    int max);

#ifdef __cplusplus
}
#endif

#endif

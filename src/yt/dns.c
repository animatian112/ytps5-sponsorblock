/* youtube-tv - A minimal DNS (A record) query builder and answer reader.
 * Copyright (C) 2026 SponsorBlock for PS5 contributors
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "yt/dns.h"

#include <string.h>

enum
{
    DNS_TYPE_A = 1,
    DNS_CLASS_IN = 1,
    DNS_HEADER = 12,
};

static void put16(uint8_t *at, uint16_t value)
{
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

static uint16_t get16(const uint8_t *at)
{
    return (uint16_t)((at[0] << 8) | at[1]);
}

int dns_build_query(uint8_t *buffer, size_t capacity, uint16_t id, const char *name)
{
    size_t used = DNS_HEADER;
    const char *label = name;
    if (buffer == NULL || name == NULL || capacity < DNS_HEADER + 6)
        return -1;
    memset(buffer, 0, DNS_HEADER);
    put16(buffer, id);
    buffer[2] = 0x01; /* RD */
    put16(buffer + 4, 1);
    while (*label != '\0')
    {
        const char *dot = strchr(label, '.');
        const size_t size = dot != NULL ? (size_t)(dot - label) : strlen(label);
        if (size == 0 || size > 63 || used + 1 + size + 5 > capacity)
            return -1;
        for (size_t i = 0; i < size; ++i)
        {
            const char c = label[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_'))
                return -1;
        }
        buffer[used++] = (uint8_t)size;
        memcpy(buffer + used, label, size);
        used += size;
        if (dot == NULL)
            break;
        label = dot + 1;
    }
    if (used == DNS_HEADER || used > 255 + DNS_HEADER)
        return -1;
    buffer[used++] = 0;
    put16(buffer + used, DNS_TYPE_A);
    put16(buffer + used + 2, DNS_CLASS_IN);
    return (int)(used + 4);
}

/* Skips a (possibly compressed) name; returns the offset after it or 0. */
static size_t skip_name(const uint8_t *packet, size_t length, size_t at)
{
    for (int labels = 0; labels < 128; ++labels)
    {
        if (at >= length)
            return 0;
        const uint8_t size = packet[at];
        if (size == 0)
            return at + 1;
        if ((size & 0xC0) == 0xC0)
            return at + 2 <= length ? at + 2 : 0; /* a pointer ends the name */
        if ((size & 0xC0) != 0)
            return 0;
        at += 1u + size;
    }
    return 0;
}

int dns_parse_a(const uint8_t *packet, size_t length, uint16_t id, uint32_t *addresses, int max)
{
    if (packet == NULL || length < DNS_HEADER || get16(packet) != id)
        return -1;
    if ((packet[2] & 0x80) == 0) /* not a response */
        return -1;
    const int rcode = packet[3] & 0x0F;
    if (rcode != 0)
        return -2 - rcode;
    const unsigned questions = get16(packet + 4);
    const unsigned answers = get16(packet + 6);
    size_t at = DNS_HEADER;
    for (unsigned i = 0; i < questions; ++i)
    {
        at = skip_name(packet, length, at);
        if (at == 0 || at + 4 > length)
            return -1;
        at += 4;
    }
    int found = 0;
    for (unsigned i = 0; i < answers; ++i)
    {
        at = skip_name(packet, length, at);
        if (at == 0 || at + 10 > length)
            return found > 0 ? found : -1;
        const uint16_t type = get16(packet + at);
        const uint16_t klass = get16(packet + at + 2);
        const uint16_t size = get16(packet + at + 8);
        at += 10;
        if (at + size > length)
            return found > 0 ? found : -1;
        if (type == DNS_TYPE_A && klass == DNS_CLASS_IN && size == 4 && found < max)
        {
            uint32_t address;
            memcpy(&address, packet + at, 4); /* already network order */
            if (address != 0)
                addresses[found++] = address;
        }
        at += size;
    }
    return found;
}

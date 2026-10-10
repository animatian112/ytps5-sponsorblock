// youtube-tv - The console side of the self-update engine.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from ps5-native-app-boilerplate (examples/self-update/self_update_ps5.c), with the
// catalog's fetch and signature checks removed; see self_update.h.
//
// self_update_console(): HTTPS through the update transport seam
// (src/platform/ps5/update_http.c), the helper through the payload loader on loopback port
// 9021, and the process clock. The helper is shipped in the app's folder as
// /app0/self-updater.elf.
#include "update/self_update.h"

#include "update/update_check.h"

#include "update/self_update_protocol.h"

#include <fcntl.h>
#include <string.h>
#include <sys/types.h>

int sceKernelOpen(const char *path, int flags, mode_t mode);
int sceKernelClose(int descriptor);
int64_t sceKernelRead(int descriptor, void *buffer, size_t length);
uint64_t sceKernelGetProcessTime(void);
int sceNetConnect(int socket, const void *address, uint32_t address_length);
int sceNetSend(int socket, const void *data, size_t length, int flags);
int sceNetRecv(int socket, void *data, size_t length, int flags);
int sceNetSetsockopt(int socket, int level, int option, const void *value, uint32_t size);
int sceNetSocket(const char *name, int domain, int type, int protocol);
int sceNetSocketClose(int socket);

#ifndef SELF_UPDATE_HELPER_PATH
#define SELF_UPDATE_HELPER_PATH "/app0/self-updater.elf"
#endif

/* ---- The release --------------------------------------------------------------------------- */

/* The engine's platform download; the console transport does the transfer. */
static int console_download(void *user, const char *url, uint64_t limit, self_update_sink sink,
                            void *sink_user)
{
    (void)user;
    return update_transport_download(url, limit, sink, sink_user);
}

/* ---- The helper ---------------------------------------------------------------------------- */

typedef struct net_address
{
    uint8_t length;
    uint8_t family;
    uint16_t port;
    uint32_t address;
    uint16_t virtual_port;
    uint8_t zero[6];
} net_address;

static long channel_send(void *user, const void *data, size_t size)
{
    return (long)sceNetSend((int)(intptr_t)user, data, size, 0);
}

static long channel_receive(void *user, void *data, size_t size)
{
    return (long)sceNetRecv((int)(intptr_t)user, data, size, 0);
}

static void channel_close(void *user)
{
    (void)sceNetSocketClose((int)(intptr_t)user);
}

/* The loader reads the program up to the end of its section table and gives the rest of the
 * connection to the program as its standard input and output. */
static int console_open_helper(void *user, self_update_channel *channel)
{
    static unsigned char buffer[65536];
    /* The helper reports five times a second while it works; this long a silence means it is
     * gone. */
    const int io_timeout_us = 60000000;
    const int connect_timeout_us = 5000000;
    const uint16_t port = SELF_UPDATE_LOADER_PORT;
    const net_address address = {sizeof(net_address), 2, (uint16_t)((port << 8) | (port >> 8)),
                                 0x0100007f,          0, {0}};
    int program;
    int socket;
    int sent = 0;
    (void)user;
    program = sceKernelOpen(SELF_UPDATE_HELPER_PATH, O_RDONLY, 0);
    if (program < 0)
        return -1; /* the helper program is missing */
    socket = sceNetSocket("self_update", 2, 1, 6);
    if (socket >= 0 &&
        sceNetSetsockopt(socket, 0xffff, 0x1105, &io_timeout_us, sizeof(io_timeout_us)) >= 0 &&
        sceNetSetsockopt(socket, 0xffff, 0x1106, &io_timeout_us, sizeof(io_timeout_us)) >= 0 &&
        sceNetSetsockopt(socket, 0xffff, 0x1109, &connect_timeout_us, sizeof(connect_timeout_us)) >=
            0 &&
        sceNetConnect(socket, &address, sizeof(address)) >= 0)
    {
        sent = 1;
        for (;;)
        {
            const int64_t count = sceKernelRead(program, buffer, sizeof(buffer));
            size_t done = 0;
            if (count == 0)
                break;
            if (count < 0)
            {
                sent = 0;
                break;
            }
            while (sent && done < (size_t)count)
            {
                const int moved = sceNetSend(socket, buffer + done, (size_t)count - done, 0);
                if (moved <= 0)
                    sent = 0;
                else
                    done += (size_t)moved;
            }
            if (!sent)
                break;
        }
    }
    (void)sceKernelClose(program);
    if (!sent)
    {
        if (socket >= 0)
            (void)sceNetSocketClose(socket);
        return 0;
    }
    channel->send = channel_send;
    channel->receive = channel_receive;
    channel->close = channel_close;
    channel->user = (void *)(intptr_t)socket;
    return 1;
}

/* ---- The clock ----------------------------------------------------------------------------- */

static uint64_t console_now_ms(void *user)
{
    (void)user;
    return sceKernelGetProcessTime() / 1000u;
}

const self_update_platform *self_update_console(void)
{
    static const self_update_platform platform = {console_download, console_open_helper,
                                                  console_now_ms, NULL};
    return &platform;
}

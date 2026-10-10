// youtube-tv - The self-update engine: start the helper, stream the release, stage it.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from ps5-native-app-boilerplate (examples/self-update/self_update.{h,c}); the
// job below (a thread, the connection to the helper and the archive stream) is the
// original, the catalog's signed manifest is gone: src/yt/update.cpp reads the newest
// GitHub release instead and fills the offer.
//
// Lets the app replace itself with the release it found:
//
//   1. the app asks its user;
//   2. self_update_start()  sends the helper to the console's payload loader, gives it
//                           the release it expects and streams it the archive while it
//                           is downloaded;
//   3. the helper checks and unpacks it beside the app and reports "staged";
//   4. self_update_apply()  gives the go-ahead; the app then closes itself, and the
//                           helper replaces the app's files and posts a notification.
//
// The app stays in its sandbox and does the network transfer. The helper
// (tooling/self-update-helper, built as self-updater.elf and shipped in the app's
// folder as /app0/self-updater.elf) runs outside the sandbox. With no loader
// listening on SELF_UPDATE_LOADER_PORT, self_update_start() fails cleanly and the app
// is untouched.
//
// C11; also compiles as C++. Needs update_check.{h,c}, self_update_sha256.{h,c},
// self_update_protocol.h, and, on the console, src/platform/ps5/self_update_ps5.c (whose
// HTTPS goes through src/platform/ps5/update_http.c).
#ifndef YOUTUBE_TV_SELF_UPDATE_H
#define YOUTUBE_TV_SELF_UPDATE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* GitHub's redirect to its release file host carries a signed query of about a thousand
 * characters. */
#define SELF_UPDATE_MAX_URL 4096u

    /* ---- What the console provides (and what tests replace) ------------------------------ */

    /* A connection to a helper that has just been started. send and receive return the bytes
     * moved, or 0 or less when the connection ended or timed out. */
    typedef struct self_update_channel
    {
        long (*send)(void *user, const void *data, size_t size);
        long (*receive)(void *user, void *data, size_t size);
        void (*close)(void *user);
        void *user;
    } self_update_channel;

    /* Receives a download in order. Returns 1 to go on, 0 to stop the transfer. */
    typedef int (*self_update_sink)(void *user, const void *data, size_t size);

    typedef struct self_update_platform
    {
        /* GET a large file into sink, refusing more than limit bytes. Redirects are followed
         * only where self_update_url_allowed() says so. 0: complete with status 200. */
        int (*download)(void *user, const char *url, uint64_t limit, self_update_sink sink,
                        void *sink_user);
        /* Sends the helper to the payload loader. 1: channel is open; 0: nothing answers on
         * SELF_UPDATE_LOADER_PORT; -1: the helper program couldn't be read. */
        int (*open_helper)(void *user, self_update_channel *channel);
        /* Milliseconds from any fixed start, never going back. */
        uint64_t (*now_ms)(void *user);
        void *user;
    } self_update_platform;

    /* The console's own: HTTPS through src/platform/ps5/update_http.c, the payload loader
     * on loopback port 9021, and the helper at /app0/self-updater.elf. Defined in
     * src/platform/ps5/self_update_ps5.c. */
    const self_update_platform *self_update_console(void);

    /* ---- The release that is installed ---------------------------------------------------- */

    /* What the helper is told about the release; the same fields
     * self_update_protocol.h carries. yt::update fills it from the GitHub release
     * (src/yt/update.cpp) and keeps the SHA-256 out of the app's interface. */
    typedef struct self_update_offer
    {
        char title[10];     /* the app's title ID */
        char installed[12]; /* the running app's content version */
        char available[12]; /* the release's content version */
        char version[40];   /* the release's tag, for display */
        char name[64];      /* the app's name, for the notification */
        char artifact[512]; /* the asset's https://github.com/... address */
        char sha256[65];    /* the asset's SHA-256, from the release's listing */
        uint64_t size;      /* the asset's size in bytes; 0 when it isn't known */
    } self_update_offer;

    /* 1 when a download may be made from url. redirected: 0 for the address the release
     * lists (github.com only), 1 for where GitHub sends it (its release file host as well). */
    int self_update_url_allowed(const char *url, int redirected);

    /* ---- The update itself --------------------------------------------------------------- */

    typedef enum self_update_phase
    {
        SELF_UPDATE_IDLE = 0,
        SELF_UPDATE_STARTING,    /* starting the helper */
        SELF_UPDATE_DOWNLOADING, /* done/total are bytes of the archive */
        SELF_UPDATE_UNPACKING,   /* done/total are bytes unpacked */
        SELF_UPDATE_READY,       /* staged: call self_update_apply() or self_update_cancel() */
        SELF_UPDATE_APPLYING,    /* the helper has the go-ahead: close the app now */
        SELF_UPDATE_CANCELLED,
        SELF_UPDATE_FAILED /* error says why; nothing was changed */
    } self_update_phase;

    typedef struct self_update_status
    {
        self_update_phase phase;
        uint64_t done;
        uint64_t total;     /* 0 while it isn't known */
        double rate;        /* bytes per second, smoothed; 0 until it is known */
        char time_left[32]; /* "about 20 s left"; empty until it is known */
        char error[160];
    } self_update_status;

    typedef struct self_update_job
    {
        /* Private. Zero it once (a static or `= {0}`) before self_update_start(). */
        const self_update_platform *platform;
        self_update_offer offer;
        self_update_channel channel;
        pthread_mutex_t lock;
        pthread_t thread;
        int started;
        int channel_open;
        int cancel;
        self_update_phase phase;
        uint64_t done;
        uint64_t total;
        char error[160];
        uint64_t rate_time;
        uint64_t rate_done;
        double rate;
        self_update_phase rate_phase;
    } self_update_job;

    /* Starts the download and staging on a thread of its own. 1: started. */
    int self_update_start(self_update_job *job, const self_update_platform *platform,
                          const self_update_offer *offer);

    /* Where the job is; cheap, for every frame. */
    void self_update_poll(self_update_job *job, self_update_status *status);

    /* Stops the job. Until self_update_apply() has returned 1, this leaves the app untouched. */
    void self_update_cancel(self_update_job *job);

    /* In phase READY: gives the helper the go-ahead. 1: the helper is waiting for the app to
     * close; end the app now (on the console: sceSystemServiceLoadExec("exit", NULL)). The
     * helper replaces the files once the app is gone and posts a notification. */
    int self_update_apply(self_update_job *job);

    /* Waits for the job's thread; call it after cancel or failure before reusing the job. */
    void self_update_finish(self_update_job *job);

    /* "about 20 s left" from bytes left and a speed; empty when it can't be said. */
    void self_update_time_left(uint64_t done, uint64_t total, double rate, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif

// youtube-tv - The self-update engine: start the helper, stream the release, stage it.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from ps5-native-app-boilerplate (examples/self-update/self_update.c), with the
// catalog's signed check removed; see self_update.h.
//
// This file is C11 and also compiles as C++. It holds no console calls: everything it needs
// from the console comes through self_update_platform, so the same code runs in the host
// tests. Trust: the app was given the asset's address, size and SHA-256 by the release feed,
// hashes the download on the way and requires the two to match; the helper checks the
// archive again before it touches the app.
#include "self_update.h"

#include "self_update_protocol.h"
#include "self_update_sha256.h"

#include <stdio.h>
#include <string.h>

/* ---- The addresses GitHub uses ------------------------------------------------------------- */

static int starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

int self_update_url_allowed(const char *url, int redirected)
{
    size_t i;
    size_t length;
    if (url == NULL)
        return 0;
    length = strlen(url);
    if (length < 20 ||
        length >= (redirected ? SELF_UPDATE_MAX_URL : sizeof(((self_update_offer *)0)->artifact)))
        return 0;
    for (i = 0; i < length; ++i)
        if ((unsigned char)url[i] <= 0x20 || (unsigned char)url[i] >= 0x7f || url[i] == '\\' ||
            url[i] == '@')
            return 0;
    if (starts_with(url, "https://github.com/"))
        return 1;
    return redirected && starts_with(url, "https://release-assets.githubusercontent.com/");
}

void self_update_time_left(uint64_t done, uint64_t total, double rate, char *out, size_t size)
{
    double seconds;
    if (size == 0)
        return;
    out[0] = '\0';
    if (rate < 1024.0 || total <= done)
        return;
    seconds = (double)(total - done) / rate;
    if (seconds < 8.0)
        (void)snprintf(out, size, "a few seconds left");
    else if (seconds < 55.0)
        (void)snprintf(out, size, "about %d s left", ((int)((seconds + 4.999) / 5.0)) * 5);
    else if (seconds < 3600.0 * 3.0)
        (void)snprintf(out, size, "about %d min left", (int)((seconds + 59.999) / 60.0));
}

/* ---- The job ------------------------------------------------------------------------------- */

static void set_phase(self_update_job *job, self_update_phase phase)
{
    pthread_mutex_lock(&job->lock);
    if (job->phase != SELF_UPDATE_CANCELLED && job->phase != SELF_UPDATE_FAILED)
        job->phase = phase;
    pthread_mutex_unlock(&job->lock);
}

static void set_progress(self_update_job *job, uint64_t done, uint64_t total)
{
    pthread_mutex_lock(&job->lock);
    job->done = done;
    job->total = total;
    pthread_mutex_unlock(&job->lock);
}

static int cancelled(self_update_job *job)
{
    int value;
    pthread_mutex_lock(&job->lock);
    value = job->cancel;
    pthread_mutex_unlock(&job->lock);
    return value;
}

/* Ends the job: cancelled if that was asked, failed with the reason otherwise. Closing the
 * connection before "apply" is what makes the helper undo its work. */
static void stop(self_update_job *job, const char *reason)
{
    pthread_mutex_lock(&job->lock);
    if (job->cancel)
        job->phase = SELF_UPDATE_CANCELLED;
    else
    {
        job->phase = SELF_UPDATE_FAILED;
        (void)snprintf(job->error, sizeof(job->error), "%s", reason);
    }
    if (job->channel_open)
    {
        job->channel_open = 0;
        pthread_mutex_unlock(&job->lock);
        job->channel.close(job->channel.user);
        return;
    }
    pthread_mutex_unlock(&job->lock);
}

static int send_all(self_update_job *job, const void *data, size_t size)
{
    const char *bytes = (const char *)data;
    while (size != 0)
    {
        const long count = job->channel.send(job->channel.user, bytes, size);
        if (count <= 0)
            return 0;
        bytes += count;
        size -= (size_t)count;
    }
    return 1;
}

/* One line from the helper, without its end. 0: the connection ended or the line is too long. */
static int read_line(self_update_job *job, char *line, size_t size)
{
    size_t used = 0;
    for (;;)
    {
        char byte = 0;
        if (job->channel.receive(job->channel.user, &byte, 1) <= 0)
            return 0;
        if (byte == '\n')
        {
            line[used] = '\0';
            return 1;
        }
        if (used + 1 >= size)
            return 0;
        line[used++] = byte;
    }
}

typedef struct download_state
{
    self_update_job *job;
    self_update_sha256 hash;
    uint64_t received;
    int helper_lost;
} download_state;

static int on_download(void *user, const void *data, size_t size)
{
    download_state *state = (download_state *)user;
    const unsigned char *bytes = (const unsigned char *)data;
    if (cancelled(state->job))
        return 0;
    self_update_sha256_add(&state->hash, data, size);
    while (size != 0)
    {
        const size_t piece = size < SELF_UPDATE_PIECE ? size : SELF_UPDATE_PIECE;
        const unsigned char header[4] = {(unsigned char)piece, (unsigned char)(piece >> 8),
                                         (unsigned char)(piece >> 16),
                                         (unsigned char)(piece >> 24)};
        if (!send_all(state->job, header, sizeof(header)) || !send_all(state->job, bytes, piece))
        {
            state->helper_lost = 1;
            return 0;
        }
        bytes += piece;
        size -= piece;
        state->received += piece;
    }
    set_progress(state->job, state->received, state->job->offer.size);
    return 1;
}

static void *run_job(void *argument)
{
    self_update_job *job = (self_update_job *)argument;
    const self_update_offer *offer = &job->offer;
    static const unsigned char end_mark[4] = {0, 0, 0, 0};
    char line[SELF_UPDATE_LINE];
    char request[1024];
    download_state state;
    unsigned char digest[32];
    int length;
    int opened;

    opened = job->platform->open_helper(job->platform->user, &job->channel);
    if (opened != 1)
    {
        if (opened < 0)
            stop(job, "the update helper is missing (/app0/self-updater.elf)");
        else
            stop(job, "no payload loader on port 9021");
        return NULL;
    }
    pthread_mutex_lock(&job->lock);
    job->channel_open = 1;
    pthread_mutex_unlock(&job->lock);

    length = snprintf(request, sizeof(request), "%s\nupdate\n%s\n%s\n%s\n%llu\n%s\n%s\n%s\n",
                      SELF_UPDATE_MAGIC, offer->title, offer->installed, offer->available,
                      (unsigned long long)offer->size, offer->sha256, offer->name, offer->version);
    if (length <= 0 || (size_t)length >= sizeof(request) ||
        !send_all(job, request, (size_t)length) || !read_line(job, line, sizeof(line)))
    {
        stop(job, "The update helper didn't answer");
        return NULL;
    }
    if (strcmp(line, "ready") != 0)
    {
        stop(job, starts_with(line, "fail ") ? line + 5 : "The update helper refused the update");
        return NULL;
    }

    set_phase(job, SELF_UPDATE_DOWNLOADING);
    set_progress(job, 0, offer->size);
    memset(&state, 0, sizeof(state));
    state.job = job;
    self_update_sha256_start(&state.hash);
    if (job->platform->download(job->platform->user, offer->artifact,
                                offer->size != 0 ? offer->size : SELF_UPDATE_MAX_ARCHIVE,
                                on_download, &state) != 0)
    {
        stop(job, state.helper_lost ? "The update helper stopped" : "The download failed");
        return NULL;
    }
    self_update_sha256_finish(&state.hash, digest);
    if ((offer->size != 0 && state.received != offer->size) ||
        !self_update_sha256_matches(digest, offer->sha256))
    {
        stop(job, "The download doesn't match the release's listing");
        return NULL;
    }
    if (cancelled(job) || !send_all(job, end_mark, sizeof(end_mark)))
    {
        stop(job, "The update helper stopped");
        return NULL;
    }

    set_phase(job, SELF_UPDATE_UNPACKING);
    set_progress(job, 0, 0);
    for (;;)
    {
        /* The helper reports five times a second, so a cancel is seen within that. */
        if (cancelled(job))
        {
            (void)send_all(job, "cancel\n", 7);
            stop(job, "");
            return NULL;
        }
        if (!read_line(job, line, sizeof(line)))
        {
            stop(job, "The update helper stopped");
            return NULL;
        }
        if (line[0] == 'p' && line[1] == ' ')
        {
            unsigned long long done = 0;
            unsigned long long total = 0;
            if (sscanf(line + 2, "%llu %llu", &done, &total) == 2)
                set_progress(job, done, total);
            continue;
        }
        if (strcmp(line, "staged") == 0)
            break;
        stop(job, starts_with(line, "fail ") ? line + 5 : "The update could not be unpacked");
        return NULL;
    }
    if (cancelled(job))
    {
        stop(job, "");
        return NULL;
    }
    set_phase(job, SELF_UPDATE_READY);
    return NULL;
}

int self_update_start(self_update_job *job, const self_update_platform *platform,
                      const self_update_offer *offer)
{
    if (job == NULL || platform == NULL || offer == NULL || job->started)
        return 0;
    memset(job, 0, sizeof(*job));
    job->platform = platform;
    job->offer = *offer;
    job->phase = SELF_UPDATE_STARTING;
    if (pthread_mutex_init(&job->lock, NULL) != 0)
        return 0;
    if (pthread_create(&job->thread, NULL, run_job, job) != 0)
    {
        (void)pthread_mutex_destroy(&job->lock);
        job->phase = SELF_UPDATE_IDLE;
        return 0;
    }
    job->started = 1;
    return 1;
}

void self_update_poll(self_update_job *job, self_update_status *status)
{
    uint64_t now;
    memset(status, 0, sizeof(*status));
    if (job == NULL || !job->started)
        return;
    now = job->platform->now_ms(job->platform->user);
    pthread_mutex_lock(&job->lock);
    /* The speed is measured over half-second steps and smoothed, so the time left settles
     * instead of jumping with every block that arrives. */
    if (job->phase != job->rate_phase || job->done < job->rate_done)
    {
        job->rate_phase = job->phase;
        job->rate = 0.0;
        job->rate_time = now;
        job->rate_done = job->done;
    }
    else if (now - job->rate_time >= 500u)
    {
        const double speed =
            (double)(job->done - job->rate_done) * 1000.0 / (double)(now - job->rate_time);
        job->rate = job->rate > 0.0 ? job->rate * 0.75 + speed * 0.25 : speed;
        job->rate_time = now;
        job->rate_done = job->done;
    }
    status->phase = job->phase;
    status->done = job->done;
    status->total = job->total;
    status->rate = job->rate;
    memcpy(status->error, job->error, sizeof(status->error));
    pthread_mutex_unlock(&job->lock);
    if (status->phase == SELF_UPDATE_DOWNLOADING || status->phase == SELF_UPDATE_UNPACKING)
        self_update_time_left(status->done, status->total, status->rate, status->time_left,
                              sizeof(status->time_left));
}

void self_update_cancel(self_update_job *job)
{
    int close_channel = 0;
    if (job == NULL || !job->started)
        return;
    pthread_mutex_lock(&job->lock);
    if (job->phase != SELF_UPDATE_APPLYING && job->phase != SELF_UPDATE_FAILED)
    {
        job->cancel = 1;
        /* Staged and waiting: nothing is running that would notice, so end it here. */
        if (job->phase == SELF_UPDATE_READY)
        {
            job->phase = SELF_UPDATE_CANCELLED;
            close_channel = job->channel_open;
            job->channel_open = 0;
        }
    }
    pthread_mutex_unlock(&job->lock);
    if (close_channel)
    {
        (void)job->channel.send(job->channel.user, "cancel\n", 7);
        job->channel.close(job->channel.user);
    }
}

int self_update_apply(self_update_job *job)
{
    char line[SELF_UPDATE_LINE];
    int ready;
    if (job == NULL || !job->started)
        return 0;
    pthread_mutex_lock(&job->lock);
    ready = job->phase == SELF_UPDATE_READY && job->channel_open;
    pthread_mutex_unlock(&job->lock);
    if (!ready)
        return 0;
    if (!send_all(job, "apply\n", 6) || !read_line(job, line, sizeof(line)) ||
        strcmp(line, "applying") != 0)
    {
        stop(job, "The update helper stopped");
        return 0;
    }
    /* The connection stays open: the app closing is what the helper waits for next. */
    set_phase(job, SELF_UPDATE_APPLYING);
    return 1;
}

void self_update_finish(self_update_job *job)
{
    if (job == NULL || !job->started)
        return;
    (void)pthread_join(job->thread, NULL);
    pthread_mutex_lock(&job->lock);
    if (job->channel_open && job->phase != SELF_UPDATE_APPLYING)
    {
        job->channel_open = 0;
        pthread_mutex_unlock(&job->lock);
        job->channel.close(job->channel.user);
    }
    else
        pthread_mutex_unlock(&job->lock);
    (void)pthread_mutex_destroy(&job->lock);
    job->started = 0;
}

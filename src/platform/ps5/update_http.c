// youtube-tv - The console side of the update transport (libcurl).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Moved out of src/update/update_check.c and src/update/self_update_ps5.c so the engine
// compiles in the PC builds, where neither console_curl.c nor PacBrew's curl headers exist.
// Implements the two functions declared in src/update/update_check.h: one HTTPS GET of the
// release feed, and the streaming download of the release archive.
//
// libcurl with OpenSSL, verifying against the console's certificate list. console_curl.c
// makes PacBrew's archives work in a native title; the options are the ones found necessary
// on the console.
#include "update/update_check.h"

#include "console_curl.h"
#include "update/self_update.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    update_check_timeout_ms = 5000,
    self_update_connect_ms = 10000, /* covers the name lookup too */
    self_update_redirects = 5
};

static const char self_update_agent[] = "ytps5";

static pthread_once_t update_http_curl_once = PTHREAD_ONCE_INIT;
static CURLcode update_http_curl_started = CURLE_FAILED_INIT;

static void update_http_curl_start(void)
{
    update_http_curl_started = curl_global_init(CURL_GLOBAL_DEFAULT);
}

/* A fresh handle per request: the check runs once per launch and the download once per
 * install. Returns NULL when libcurl could not be started or initialised. */
static CURL *update_http_easy(const char *url, const char *user_agent)
{
    CURL *easy;
    (void)pthread_once(&update_http_curl_once, update_http_curl_start);
    if (update_http_curl_started != CURLE_OK)
        return NULL;
    easy = curl_easy_init();
    if (easy == NULL)
        return NULL;
    console_curl_setup(easy); /* no signals, the console's CA list, non-blocking sockets */
    (void)curl_easy_setopt(easy, CURLOPT_URL, url);
    (void)curl_easy_setopt(easy, CURLOPT_USERAGENT, user_agent);
    (void)curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https");
    (void)curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    (void)curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    return easy;
}

static int update_http_curl_error(CURLcode code)
{
    return -(10000 + (int)code);
}

#ifdef UPDATE_CHECK_CURL_TRACE
/* Diagnostics: libcurl's own account of each step, in the kernel log with the time since the
 * process started. Headers and bodies are left out. */
#ifdef __cplusplus
extern "C"
{
#endif
    int sceKernelDebugOutText(int channel, const char *text);
    uint64_t sceKernelGetProcessTime(void);
#ifdef __cplusplus
}
#endif

static int update_http_on_trace(CURL *easy, curl_infotype type, char *data, size_t size, void *user)
{
    char line[256];
    (void)easy;
    (void)user;
    if (type != CURLINFO_TEXT)
        return 0;
    while (size > 0 && (data[size - 1] == '\n' || data[size - 1] == '\r'))
        --size;
    (void)snprintf(line, sizeof(line), "UPDATE-CHECK-CURL: %llu ms %.*s\n",
                   (unsigned long long)(sceKernelGetProcessTime() / 1000u),
                   (int)(size < 200 ? size : 200), data);
    (void)sceKernelDebugOutText(0, line);
    return 0;
}
#endif

/* ---- The release feed: one small JSON document --------------------------------------------- */

typedef struct update_check_sink
{
    char *body;
    size_t capacity;
    size_t used;
    int overflow;
} update_check_sink;

static size_t update_check_on_body(char *data, size_t size, size_t count, void *user)
{
    update_check_sink *sink = (update_check_sink *)user;
    const size_t bytes = size * count;
    if (bytes > sink->capacity - sink->used)
    {
        sink->overflow = 1;
        return 0; /* anything but `bytes` ends the transfer */
    }
    memcpy(sink->body + sink->used, data, bytes);
    sink->used += bytes;
    return bytes;
}

int update_transport_get_json(const char *url, const char *user_agent, const char *accept,
                              char **body, size_t *size, long *status)
{
    update_check_sink sink;
    struct curl_slist *headers = NULL;
    char accept_header[128];
    long http_status = 0;
    CURLcode code;
    CURL *easy;

    if (body == NULL || size == NULL || status == NULL)
        return UPDATE_CHECK_BAD_ARGUMENT;
    *body = NULL;
    *size = 0;
    *status = 0;
    (void)snprintf(accept_header, sizeof(accept_header), "Accept: %s",
                   accept != NULL ? accept : UPDATE_CHECK_ACCEPT);

    /* One byte past the cap so the buffer can always be NUL-terminated. */
    sink.body = (char *)malloc(UPDATE_CHECK_MAX_RESPONSE + 1u);
    if (sink.body == NULL)
        return update_http_curl_error(CURLE_OUT_OF_MEMORY);
    sink.capacity = UPDATE_CHECK_MAX_RESPONSE;
    sink.used = 0;
    sink.overflow = 0;

    easy = update_http_easy(url, user_agent);
    if (easy == NULL)
    {
        free(sink.body);
        return update_http_curl_error(
            update_http_curl_started != CURLE_OK ? update_http_curl_started : CURLE_FAILED_INIT);
    }

    /* The release feed answers JSON only when asked for it. */
    headers = curl_slist_append(NULL, accept_header);
    (void)curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    (void)curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, (long)update_check_timeout_ms);
    (void)curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 3L * update_check_timeout_ms);
    (void)curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, update_check_on_body);
    (void)curl_easy_setopt(easy, CURLOPT_WRITEDATA, &sink);
#ifdef UPDATE_CHECK_CURL_TRACE
    (void)curl_easy_setopt(easy, CURLOPT_DEBUGFUNCTION, update_http_on_trace);
    (void)curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);
#endif

    code = curl_easy_perform(easy);
    (void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(easy);
    *status = http_status;
    if (sink.overflow)
    {
        free(sink.body);
        return UPDATE_CHECK_FETCH_TOO_LARGE;
    }
    if (code != CURLE_OK)
    {
        free(sink.body);
        return update_http_curl_error(code);
    }
    sink.body[sink.used] = '\0';
    *body = sink.body;
    *size = sink.used;
    return 0;
}

/* ---- The release archive: streamed into the sink ------------------------------------------- */

typedef struct stream_sink
{
    self_update_sink sink;
    void *user;
    uint64_t limit;
    uint64_t received;
    int stopped;
} stream_sink;

static size_t on_stream(char *data, size_t size, size_t count, void *user)
{
    stream_sink *stream = (stream_sink *)user;
    const size_t bytes = size * count;
    if (bytes > stream->limit - stream->received || stream->sink(stream->user, data, bytes) != 1)
    {
        stream->stopped = 1;
        return 0;
    }
    stream->received += bytes;
    return bytes;
}

int update_transport_download(const char *url, uint64_t limit, self_update_sink sink, void *user)
{
    static char current[SELF_UPDATE_MAX_URL]; /* one download at a time */
    int hop;
    if (!self_update_url_allowed(url, 0))
        return -1;
    (void)snprintf(current, sizeof(current), "%s", url);
    for (hop = 0; hop <= self_update_redirects; ++hop)
    {
        stream_sink stream = {sink, user, limit, 0, 0};
        long code = 0;
        char *next = NULL;
        CURLcode result;
        CURL *easy = update_http_easy(current, self_update_agent);
        if (easy == NULL)
            return -(10000 + (int)CURLE_FAILED_INIT);
        (void)curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, (long)self_update_connect_ms);
        (void)curl_easy_setopt(easy, CURLOPT_BUFFERSIZE, 256L * 1024L);
        /* No overall limit for a large file: give up when nothing arrives for a while. */
        (void)curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
        (void)curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 30L);
        (void)curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_stream);
        (void)curl_easy_setopt(easy, CURLOPT_WRITEDATA, &stream);
        result = curl_easy_perform(easy);
        (void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
        if (result == CURLE_OK && code >= 301 && code <= 308 && code != 304 && code != 305 &&
            code != 306 && curl_easy_getinfo(easy, CURLINFO_REDIRECT_URL, &next) == CURLE_OK &&
            next != NULL && self_update_url_allowed(next, 1) && stream.received == 0)
        {
            (void)snprintf(current, sizeof(current), "%s", next);
            curl_easy_cleanup(easy);
            continue;
        }
        curl_easy_cleanup(easy);
        if (result != CURLE_OK || stream.stopped)
            return -(10000 + (int)(result != CURLE_OK ? result : CURLE_WRITE_ERROR));
        return code == 200 ? 0 : -(int)code;
    }
    return -1; /* too many redirects */
}

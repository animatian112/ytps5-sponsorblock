// youtube-tv - Update check against the project's newest GitHub release.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from ps5-native-app-boilerplate (examples/update-check/update_check.{h,c}): the
// JSON reader, the content-version helpers and the libcurl transport are the originals,
// the homebrew.page catalog is replaced by the repository's release feed.
//
// The newest release of https://github.com/animatian112/ytps5-sponsorblock (the SponsorBlock
// build; upstream is https://github.com/haasele/ytps5) is the source of truth: its
// tag is the content version, its body the release notes and its asset PPSA98260.zip
// the payload. What comes back is the network's, so nothing here trusts it: every
// length is checked, strings are copied only into buffers that hold them (the release
// notes are the one trimmed value), and nesting is bounded.
//
//     update_check_release release;
//     int status = 0;
//     int error = 0;
//     if (update_check_run_release(&release, &status, &error) == UPDATE_CHECK_OK)
//         ... (release.tag is the version to compare)
//
// Transport: the two functions at the bottom of this header are the seam between the
// engine and the network. On the console they are implemented by
// src/platform/ps5/update_http.c (libcurl with OpenSSL and the console's certificate
// list); the PC build links host/update_http_stub.cpp instead. The check is one HTTPS
// GET of the release feed, downloads nothing else, installs nothing, and needs no
// elevation. Define UPDATE_CHECK_NO_NETWORK to build only the parsing, for the helper
// and host tests. The version helpers and the JSON reader are also what the helper uses
// to check the param.json of an unpacked release.
#ifndef YOUTUBE_TV_UPDATE_CHECK_H
#define YOUTUBE_TV_UPDATE_CHECK_H

#include <stddef.h>
#include <stdint.h>

#include "self_update.h" /* self_update_sink */

#ifdef __cplusplus
extern "C"
{
#endif

/* The one release feed the app reads: the newest release of its own repository. */
#define UPDATE_CHECK_API \
    "https://api.github.com/repos/animatian112/ytps5-sponsorblock/releases/latest"
/* The feed answers JSON only for this type; GitHub asks every caller to name itself. */
#define UPDATE_CHECK_ACCEPT "application/vnd.github+json"
#define UPDATE_CHECK_USER_AGENT "ytps5-sponsorblock"
/* GitHub's own limit for a release body; more is cut, never kept. */
#define UPDATE_CHECK_NOTES_MAX 2000u
/* The asset every release of this title must carry. */
#define UPDATE_CHECK_ASSET_NAME "PPSA98260.zip"
#define UPDATE_CHECK_MAX_RESPONSE 65536u

    /* What the whole check can say; 0 is success, everything else is why there is none. */
    typedef enum update_check_reason
    {
        UPDATE_CHECK_OK = 0,
        UPDATE_CHECK_NETWORK = -1,      /* the request failed; see platform_error */
        UPDATE_CHECK_HTTP_STATUS = -2,  /* the feed answered something but not 200 */
        UPDATE_CHECK_TOO_LARGE = -3,    /* the answer is over UPDATE_CHECK_MAX_RESPONSE */
        UPDATE_CHECK_BAD_RESPONSE = -4, /* not the JSON the API documents */
        UPDATE_CHECK_BAD_ARGUMENT = -5  /* a buffer that isn't usable */
    } update_check_reason;

    typedef struct update_check_release
    {
        char tag[64];                           /* tag_name, as GitHub writes it */
        char notes[UPDATE_CHECK_NOTES_MAX + 1]; /* body, trimmed */
        char asset_url[512];                    /* browser_download_url of PPSA98260.zip */
        char asset_sha256[65];                  /* the asset's digest, lowercase hex */
        uint64_t asset_size;                    /* its size in bytes */
        int has_asset;                          /* 1 when the release carries it */
        int has_size;                           /* 1 when its size came with it */
    } update_check_release;

    /* The pieces, usable and testable without a console. */

    /* 1 when `text` is a content version (NN.NNN.NNN); `parts` then holds its three numbers. */
    int update_check_version_parse(const char *text, unsigned parts[3]);

    /* Compares two content versions: negative, zero or positive like strcmp.
     * Returns 0 and sets *comparable to 0 when either isn't a content version. */
    int update_check_version_compare(const char *left, const char *right, int *comparable);

    /* Copies the string value of a top-level `key` of a JSON object into `out`.
     * 1: copied. 0: no such key. 2: the value is null. -1: malformed, not a string, or too long. */
    int update_check_json_string(const char *json, size_t length, const char *key, char *out,
                                 size_t size);

    /* Reads one release answer. 1: `out` is filled and `tag` is set. Never fails on an
     * oversized body: the notes are cut at UPDATE_CHECK_NOTES_MAX. */
    int update_check_read_release(const char *json, size_t length, update_check_release *out);

    /* A short English word for a reason, for the log. */
    const char *update_check_reason_text(update_check_reason reason);

#define UPDATE_CHECK_FETCH_TOO_LARGE 1

    /* ---- The transport seam -------------------------------------------------------------- */

    /* One HTTPS GET of a small JSON document (release feed). On success *body is a
     * malloc'd NUL-terminated buffer the caller frees. Returns 0 on success,
     * UPDATE_CHECK_FETCH_TOO_LARGE when the answer is over UPDATE_CHECK_MAX_RESPONSE, or a
     * negative error. *status carries the HTTP status when there was one. */
    int update_transport_get_json(const char *url, const char *user_agent, const char *accept,
                                  char **body, size_t *size, long *status);

    /* Streams a large file through sink(); limit is the expected size ceiling. Returns 0 on
     * success, the same error codes the engine used before. */
    int update_transport_download(const char *url, uint64_t limit, self_update_sink sink,
                                  void *user);

#ifndef UPDATE_CHECK_NO_NETWORK
    /* The whole check over HTTPS, with certificate verification. Blocking for up to a few
     * seconds: call it from a worker thread, at most once per launch, and never make the app
     * wait. Returns an update_check_reason (UPDATE_CHECK_TOO_LARGE for an oversized answer).
     * A failed request fills *platform_error (negative, from libcurl). */
    int update_check_run_release(update_check_release *out, int *http_status, int *platform_error);
#endif

#ifdef __cplusplus
}
#endif

#endif

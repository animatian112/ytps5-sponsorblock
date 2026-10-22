// youtube-tv - Update check against the project's newest GitHub release.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from ps5-native-app-boilerplate (examples/update-check/update_check.c): the version
// helpers, the JSON reader and the libcurl transport are the originals, the homebrew.page
// catalog is replaced by the repository's release feed. See update_check.h.
//
// What it reads comes from the network, so nothing here trusts it: every length is checked,
// strings are copied only into buffers that hold them, and nesting is bounded. The release
// notes are the one value that is cut rather than refused.
#include "update_check.h"

#include <stdlib.h>
#include <string.h>

/* The release asset this install updates from (see update_check_set_asset_name). */
static char update_check_asset_name[64] = UPDATE_CHECK_ASSET_NAME;

void update_check_set_asset_name(const char *name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) >= sizeof(update_check_asset_name))
        return;
    memcpy(update_check_asset_name, name, strlen(name) + 1);
}

/* Content versions: NN.NNN.NNN, compared as three numbers. */

int update_check_version_parse(const char *text, unsigned parts[3])
{
    static const char shape[] = "dd.ddd.ddd";
    unsigned values[3] = {0, 0, 0};
    unsigned field = 0;
    size_t i;
    if (text == NULL)
        return 0;
    for (i = 0; shape[i] != '\0'; ++i)
    {
        const char c = text[i];
        if (shape[i] == '.')
        {
            if (c != '.')
                return 0;
            ++field;
        }
        else
        {
            if (c < '0' || c > '9')
                return 0;
            values[field] = values[field] * 10u + (unsigned)(c - '0');
        }
    }
    if (text[i] != '\0')
        return 0;
    if (parts != NULL)
    {
        parts[0] = values[0];
        parts[1] = values[1];
        parts[2] = values[2];
    }
    return 1;
}

int update_check_version_compare(const char *left, const char *right, int *comparable)
{
    unsigned a[3];
    unsigned b[3];
    int i;
    const int ok = update_check_version_parse(left, a) && update_check_version_parse(right, b);
    if (comparable != NULL)
        *comparable = ok;
    if (!ok)
        return 0;
    for (i = 0; i < 3; ++i)
    {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}
typedef struct json_cursor
{
    const char *at;
    const char *end;
} json_cursor;

static void json_skip_space(json_cursor *c)
{
    while (c->at < c->end && (*c->at == ' ' || *c->at == '\t' || *c->at == '\n' || *c->at == '\r'))
        ++c->at;
}

static int json_hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Reads a string at the cursor. With `out`, copies it (UTF-8) and fails when it doesn't fit;
 * with `trim`, it is cut there instead. Without `out`, only skips it. Returns 1 on success. */
static int json_string(json_cursor *c, char *out, size_t size, int trim)
{
    size_t used = 0;
    int full = 0;
    if (c->at >= c->end || *c->at != '"')
        return 0;
    ++c->at;
    while (c->at < c->end)
    {
        unsigned code = (unsigned char)*c->at++;
        char encoded[3] = {0, 0, 0};
        size_t count = 1;
        size_t i;
        int escaped_code_point = 0; /* bytes of the text itself are already UTF-8 */
        if (code == '"')
        {
            if (out != NULL)
                out[used] = '\0';
            return 1;
        }
        if (code < 0x20)
            return 0;
        if (code == '\\')
        {
            if (c->at >= c->end)
                return 0;
            code = (unsigned char)*c->at++;
            switch (code)
            {
            case '"':
            case '\\':
            case '/':
                break;
            case 'b':
                code = '\b';
                break;
            case 'f':
                code = '\f';
                break;
            case 'n':
                code = '\n';
                break;
            case 'r':
                code = '\r';
                break;
            case 't':
                code = '\t';
                break;
            case 'u':
                if (c->end - c->at < 4)
                    return 0;
                code = 0;
                for (i = 0; i < 4; ++i)
                {
                    const int digit = json_hex(c->at[i]);
                    if (digit < 0)
                        return 0;
                    code = code * 16u + (unsigned)digit;
                }
                c->at += 4;
                if (code >= 0xD800u && code <= 0xDFFFu)
                    code = '?'; /* surrogate halves: nothing this reader needs */
                escaped_code_point = 1;
                break;
            default:
                return 0;
            }
        }
        if (!escaped_code_point || code < 0x80u)
        {
            encoded[0] = (char)code;
        }
        else if (code < 0x800u)
        {
            encoded[0] = (char)(0xC0u | (code >> 6));
            encoded[1] = (char)(0x80u | (code & 0x3Fu));
            count = 2;
        }
        else
        {
            encoded[0] = (char)(0xE0u | (code >> 12));
            encoded[1] = (char)(0x80u | ((code >> 6) & 0x3Fu));
            encoded[2] = (char)(0x80u | (code & 0x3Fu));
            count = 3;
        }
        if (out != NULL)
        {
            if (!full)
            {
                if (used + count < size)
                {
                    for (i = 0; i < count; ++i)
                        out[used + i] = encoded[i];
                    used += count;
                }
                else if (trim)
                    full = 1; /* the notes are cut here, never refused */
                else
                    return 0;
            }
        }
        else
            used += count;
    }
    return 0;
}

/* Skips one value of any type. Returns 1 on success. */
static int json_skip_value(json_cursor *c)
{
    int depth = 0;
    if (c->at >= c->end)
        return 0;
    if (*c->at == '"')
        return json_string(c, NULL, 0, 0);
    if (*c->at != '{' && *c->at != '[')
    {
        const char *start = c->at;
        while (c->at < c->end && *c->at != ',' && *c->at != '}' && *c->at != ']' && *c->at != ' ' &&
               *c->at != '\t' && *c->at != '\n' && *c->at != '\r')
            ++c->at;
        return c->at > start;
    }
    while (c->at < c->end)
    {
        const char ch = *c->at;
        if (ch == '"')
        {
            if (!json_string(c, NULL, 0, 0))
                return 0;
            continue;
        }
        ++c->at;
        if (ch == '{' || ch == '[')
        {
            if (++depth > 64)
                return 0;
        }
        else if ((ch == '}' || ch == ']') && --depth == 0)
        {
            return 1;
        }
    }
    return 0;
}

int update_check_json_string(const char *json, size_t length, const char *key, char *out,
                             size_t size)
{
    json_cursor c;
    char name[64];
    if (json == NULL || key == NULL || out == NULL || size == 0)
        return -1;
    c.at = json;
    c.end = json + length;
    json_skip_space(&c);
    if (c.at >= c.end || *c.at != '{')
        return -1;
    ++c.at;
    for (;;)
    {
        int matches;
        json_skip_space(&c);
        if (c.at < c.end && *c.at == '}')
            return 0;
        /* A name too long for `name` can't be the one asked for; it is skipped whole. */
        {
            json_cursor probe = c;
            if (json_string(&probe, name, sizeof(name), 0))
            {
                c = probe;
                matches = strcmp(name, key) == 0;
            }
            else if (json_string(&c, NULL, 0, 0))
            {
                matches = 0;
            }
            else
            {
                return -1;
            }
        }
        json_skip_space(&c);
        if (c.at >= c.end || *c.at != ':')
            return -1;
        ++c.at;
        json_skip_space(&c);
        if (matches)
        {
            if (c.at < c.end && *c.at == '"')
                return json_string(&c, out, size, 0) ? 1 : -1;
            if (c.end - c.at >= 4 && memcmp(c.at, "null", 4) == 0)
                return 2;
            return -1;
        }
        if (!json_skip_value(&c))
            return -1;
        json_skip_space(&c);
        if (c.at < c.end && *c.at == ',')
        {
            ++c.at;
            continue;
        }
        if (c.at < c.end && *c.at == '}')
            return 0;
        return -1;
    }
}

/* ---- The release feed ---------------------------------------------------------------------- */

static int text_starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

/* A non-negative whole number at the cursor. 1: read. */
static int json_number(json_cursor *c, uint64_t *out)
{
    uint64_t value = 0;
    int digits = 0;
    while (c->at < c->end && *c->at >= '0' && *c->at <= '9')
    {
        if (value > (UINT64_MAX - 9u) / 10u)
            return 0;
        value = value * 10u + (uint64_t)(*c->at - '0');
        ++c->at;
        ++digits;
    }
    if (digits == 0 || (c->at < c->end && (*c->at == '.' || *c->at == 'e' || *c->at == 'E')))
        return 0;
    *out = value;
    return 1;
}

/* One asset object of the release. Reads it whole and fills `out` when it is the asset this
 * app installs. 1: read, whatever it was; 0: malformed. */
static int json_read_asset(json_cursor *c, update_check_release *out)
{
    char name[64] = {0};
    char url[sizeof(out->asset_url)] = {0};
    char digest[80] = {0};
    uint64_t size = 0;
    int have_size = 0;
    if (c->at >= c->end || *c->at != '{')
        return 0;
    ++c->at;
    for (;;)
    {
        char key[32];
        json_skip_space(c);
        if (c->at < c->end && *c->at == '}')
        {
            ++c->at;
            break;
        }
        if (!json_string(c, key, sizeof(key), 0))
            return 0;
        json_skip_space(c);
        if (c->at >= c->end || *c->at != ':')
            return 0;
        ++c->at;
        json_skip_space(c);
        if (strcmp(key, "name") == 0)
        {
            if (c->at >= c->end || *c->at != '"' || !json_string(c, name, sizeof(name), 0))
                return 0;
        }
        else if (strcmp(key, "browser_download_url") == 0)
        {
            if (c->at >= c->end || *c->at != '"' || !json_string(c, url, sizeof(url), 0))
                return 0;
        }
        else if (strcmp(key, "digest") == 0)
        {
            if (c->at < c->end && *c->at == '"')
            {
                if (!json_string(c, digest, sizeof(digest), 0))
                    return 0;
            }
            else if (!json_skip_value(c))
            {
                return 0; /* null: GitHub has no digest for this asset */
            }
        }
        else if (strcmp(key, "size") == 0)
        {
            if (!json_number(c, &size))
                return 0;
            have_size = 1;
        }
        else if (!json_skip_value(c))
        {
            return 0;
        }
        json_skip_space(c);
        if (c->at < c->end && *c->at == ',')
        {
            ++c->at;
            continue;
        }
        if (c->at < c->end && *c->at == '}')
        {
            ++c->at;
            break;
        }
        return 0;
    }
    if (strcmp(name, update_check_asset_name) == 0)
    {
        size_t i;
        int hex = 1;
        out->has_asset = 1;
        out->asset_size = size;
        out->has_size = have_size;
        memcpy(out->asset_url, url, sizeof(out->asset_url));
        /* "sha256:<64 hex>"; anything else counts as no digest at all. */
        out->asset_sha256[0] = '\0';
        if (strlen(digest) == 71 && text_starts_with(digest, "sha256:"))
        {
            for (i = 0; i < 64; ++i)
            {
                const char digit = digest[7 + i];
                if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') ||
                      (digit >= 'A' && digit <= 'F')))
                    hex = 0;
            }
            if (hex)
            {
                memcpy(out->asset_sha256, digest + 7, 64);
                out->asset_sha256[64] = '\0';
            }
        }
    }
    return 1;
}

/* The release's assets array; keeps the one asset this app installs. 1: read. */
static int json_read_assets(json_cursor *c, update_check_release *out)
{
    if (c->at >= c->end || *c->at != '[')
        return 0;
    ++c->at;
    for (;;)
    {
        json_skip_space(c);
        if (c->at < c->end && *c->at == ']')
        {
            ++c->at;
            return 1;
        }
        if (!json_read_asset(c, out))
            return 0;
        json_skip_space(c);
        if (c->at < c->end && *c->at == ',')
        {
            ++c->at;
            continue;
        }
        if (c->at < c->end && *c->at == ']')
        {
            ++c->at;
            return 1;
        }
        return 0;
    }
}

int update_check_read_release(const char *json, size_t length, update_check_release *out)
{
    json_cursor c;
    if (json == NULL || out == NULL)
        return 0;
    memset(out, 0, sizeof(*out));
    c.at = json;
    c.end = json + length;
    json_skip_space(&c);
    if (c.at >= c.end || *c.at != '{')
        return 0;
    ++c.at;
    for (;;)
    {
        char key[32];
        json_skip_space(&c);
        if (c.at < c.end && *c.at == '}')
        {
            ++c.at;
            break;
        }
        if (!json_string(&c, key, sizeof(key), 0))
            return 0;
        json_skip_space(&c);
        if (c.at >= c.end || *c.at != ':')
            return 0;
        ++c.at;
        json_skip_space(&c);
        if (strcmp(key, "tag_name") == 0)
        {
            if (c.at >= c.end || *c.at != '"' || !json_string(&c, out->tag, sizeof(out->tag), 0))
                return 0;
        }
        else if (strcmp(key, "body") == 0)
        {
            if (c.at < c.end && *c.at == '"')
            {
                if (!json_string(&c, out->notes, sizeof(out->notes), 1))
                    return 0;
            }
            else if (!json_skip_value(&c))
            {
                return 0; /* null: the release has no notes */
            }
        }
        else if (strcmp(key, "assets") == 0)
        {
            if (!json_read_assets(&c, out))
                return 0;
        }
        else if (!json_skip_value(&c))
        {
            return 0;
        }
        json_skip_space(&c);
        if (c.at < c.end && *c.at == ',')
        {
            ++c.at;
            continue;
        }
        if (c.at < c.end && *c.at == '}')
        {
            ++c.at;
            break;
        }
        return 0;
    }
    json_skip_space(&c);
    /* The whole object and nothing else: an answer cut short in transit can hold
     * complete-looking fields. */
    if (c.at != c.end || out->tag[0] == '\0')
        return 0;
    return 1;
}

const char *update_check_reason_text(update_check_reason reason)
{
    switch (reason)
    {
    case UPDATE_CHECK_OK:
        return "ok";
    case UPDATE_CHECK_NETWORK:
        return "network";
    case UPDATE_CHECK_HTTP_STATUS:
        return "http-status";
    case UPDATE_CHECK_TOO_LARGE:
        return "too-large";
    case UPDATE_CHECK_BAD_RESPONSE:
        return "bad-response";
    case UPDATE_CHECK_BAD_ARGUMENT:
        return "bad-argument";
    }
    return "unknown";
}

#ifndef UPDATE_CHECK_NO_NETWORK

/* The whole check: one HTTPS GET of the repository's newest release, through the transport
 * seam (src/platform/ps5/update_http.c on the console, host/update_http_stub.cpp on the PC). */
int update_check_run_release(update_check_release *out, int *http_status, int *platform_error)
{
    char *body = NULL;
    size_t length = 0;
    long status = 0;
    int code;
    if (http_status != NULL)
        *http_status = 0;
    if (platform_error != NULL)
        *platform_error = 0;
    if (out == NULL)
        return UPDATE_CHECK_BAD_ARGUMENT;
    memset(out, 0, sizeof(*out));
    code = update_transport_get_json(UPDATE_CHECK_API, UPDATE_CHECK_USER_AGENT, UPDATE_CHECK_ACCEPT,
                                     &body, &length, &status);
    if (http_status != NULL)
        *http_status = (int)status;
    if (code == UPDATE_CHECK_FETCH_TOO_LARGE)
        return UPDATE_CHECK_TOO_LARGE;
    if (code != 0)
    {
        if (platform_error != NULL)
            *platform_error = code;
        return UPDATE_CHECK_NETWORK;
    }
    code = UPDATE_CHECK_OK;
    if (status != 200)
        code = UPDATE_CHECK_HTTP_STATUS;
    else if (!update_check_read_release(body, length, out))
        code = UPDATE_CHECK_BAD_RESPONSE;
    free(body);
    return code;
}

#endif /* UPDATE_CHECK_NO_NETWORK */

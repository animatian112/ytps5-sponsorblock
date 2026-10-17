/* SponsorBlock for PS5 - native client library
 *
 * Segment fetching/parsing and skip logic ported from SmartTube
 * (SponsorBlockService + SponsorBlockController, MIT, (c) yuliskov).
 * Plain C99, no platform dependencies: HTTP is supplied by the caller,
 * so the same code runs in the PS5 payload and in host unit tests.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SPONSORBLOCK_H
#define SPONSORBLOCK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define SB_VERSION "1.0.4"

#define SB_SERVER_MAIN "https://sponsor.ajay.app"
#define SB_SERVER_ALT "https://api.sponsor.ajay.app"

    /* ---- categories (bit flags) ------------------------------------------ */
    enum
    {
        SB_CAT_SPONSOR = 1u << 0,
        SB_CAT_SELFPROMO = 1u << 1,
        SB_CAT_INTERACTION = 1u << 2,
        SB_CAT_INTRO = 1u << 3,
        SB_CAT_OUTRO = 1u << 4,
        SB_CAT_PREVIEW = 1u << 5,
        SB_CAT_MUSIC_OFFTOPIC = 1u << 6,
        SB_CAT_FILLER = 1u << 7,
        SB_CAT_POI_HIGHLIGHT = 1u << 8,
        SB_CAT_EXCLUSIVE_ACCESS = 1u << 9, /* a whole-video label ("full") */
        SB_CAT_COUNT = 10
    };
#define SB_CAT_ALL ((1u << SB_CAT_COUNT) - 1u)

    /* Category name <-> flag. Unknown names map to 0. */
    unsigned sb_category_from_name(const char *name);
    const char *sb_category_name(unsigned flag);
    const char *sb_category_label(unsigned flag); /* "Sponsor", "Intro"... */
    /* Parse "sponsor,intro,outro" (or "all") into a mask. */
    unsigned sb_category_mask_parse(const char *list);

    /* ---- per-category behaviour (same values as SmartTube) --------------- */
    typedef enum
    {
        SB_ACT_SKIP_ONLY = 0,       /* skip silently                   */
        SB_ACT_SKIP_WITH_TOAST = 1, /* skip and tell the user          */
        SB_ACT_SHOW_DIALOG = 2,     /* ask before skipping             */
        SB_ACT_DO_NOTHING = 3       /* fetch (for markers) but no skip */
    } sb_action;

    typedef struct
    {
        unsigned categories;            /* which categories to request   */
        sb_action action[SB_CAT_COUNT]; /* indexed by bit position       */
        long min_skip_ms;               /* ignore skips shorter than this */
        int skip_once;                  /* don't skip a segment again    */
        int use_alt_server;
        int hash_prefix; /* privacy: /skipSegments/<sha256[0:4]> */
    } sb_config;

    /* SmartTube defaults: every category on, skip with toast, filler = nothing. */
    void sb_config_defaults(sb_config *cfg);
    sb_action sb_config_action(const sb_config *cfg, unsigned category_flag);

    /* ---- segments -------------------------------------------------------- */
    typedef struct
    {
        long start_ms;
        long end_ms;
        unsigned category; /* SB_CAT_* flag, 0 if unknown              */
        char category_name[24];
        char action_type[12]; /* "skip", "mute", "full", "poi", "chapter"   */
        char uuid[72];
        long video_duration_ms;
        int votes;
        int locked;
    } sb_segment;

    typedef struct
    {
        sb_segment *items;
        size_t count;
    } sb_segment_list;

    /* Parse a /api/skipSegments response. Handles both the plain endpoint
     * (array of segments) and the hash-prefix endpoint (array of
     * {videoID, segments:[...]}); for the latter only entries whose videoID
     * equals `video_id` are kept. Segments are sorted by start time.
     * Returns 0 on success, -1 on malformed JSON / OOM. */
    int sb_parse_segments(const char *json, size_t len, const char *video_id, sb_segment_list *out);
    void sb_segment_list_free(sb_segment_list *list);

    /* Validate an 11-char YouTube video id ([A-Za-z0-9_-]{11}). */
    int sb_valid_video_id(const char *video_id);

    /* Build the request URL for `video_id`. Returns length or -1. */
    int sb_build_url(char *out, size_t cap, const sb_config *cfg, const char *server,
                     const char *video_id);

    /* Serialise a list back to upstream JSON (for the local proxy). Caller
     * frees. Returns NULL on OOM. */
    char *sb_segments_to_json(const sb_segment_list *list, size_t *len_out);

    /* ---- fetch with a caller-supplied transport -------------------------- */
    /* Must perform an HTTPS GET; on return *body is malloc'd (caller frees
     * via free()) and the HTTP status is returned, or <0 on network error. */
    typedef int (*sb_http_get_fn)(void *ctx, const char *url, char **body, size_t *body_len);

    /* Fetch + parse. Returns 0 with a (possibly empty) list, <0 on error.
     * A 404 from the server means "no segments" and returns 0, count 0. */
    int sb_fetch_segments(const sb_config *cfg, const char *video_id, sb_http_get_fn get, void *ctx,
                          sb_segment_list *out);

    /* ---- playback tracker (port of SponsorBlockController) --------------- */
    typedef struct
    {
        sb_segment_list segs;    /* owned copy                               */
        unsigned char *consumed; /* skip_once bookkeeping                    */
        long last_skip_ms;
        sb_config cfg;
    } sb_tracker;

    typedef struct
    {
        int kind;                  /* SB_DECIDE_*                              */
        sb_action action;          /* how to present it                        */
        long seek_to_ms;           /* target position (clamped to duration)    */
        const sb_segment *segment; /* last matched segment (owned by tracker)  */
        int grouped;               /* number of adjacent segments merged       */
    } sb_decision;

    enum
    {
        SB_DECIDE_NONE = 0,
        SB_DECIDE_SKIP = 1,
        SB_DECIDE_ASK = 2
    };

    int sb_tracker_init(sb_tracker *t, const sb_config *cfg, const sb_segment_list *segs);
    void sb_tracker_free(sb_tracker *t);

    /* Call every ~1s while playing. Mirrors skipSegment()/applyActions():
     * match window is [start, min(start + 2000*speed, end)], adjacent
     * skippable segments within 3s are chained, short skips are ignored,
     * the same target is never reported twice in a row. */
    int sb_tracker_poll(sb_tracker *t, long pos_ms, long duration_ms, double speed,
                        sb_decision *out);

    /* Manual "skip now" button: full-segment match, ignores actions. */
    int sb_tracker_manual(sb_tracker *t, long pos_ms, long duration_ms, sb_decision *out);

    /* ---- helpers --------------------------------------------------------- */
    void sb_sha256(const void *data, size_t len, uint8_t out[32]);
    void sb_sha256_hex(const void *data, size_t len, char out[65]);

#ifdef __cplusplus
}
#endif
#endif

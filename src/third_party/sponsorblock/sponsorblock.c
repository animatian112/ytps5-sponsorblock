/* SponsorBlock for PS5 - native client library.
 * SPDX-License-Identifier: MIT
 */
#include "third_party/sponsorblock/sponsorblock.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================== */
/* categories                                                            */
/* ===================================================================== */

static const char *const k_cat_names[SB_CAT_COUNT] = {
    "sponsor", "selfpromo",      "interaction", "intro",        "outro",
    "preview", "music_offtopic", "filler",      "poi_highlight", "exclusive_access"};

static const char *const k_cat_labels[SB_CAT_COUNT] = {
    "Sponsor",       "Self promotion",    "Interaction reminder", "Intro",    "Outro",
    "Preview/Recap", "Non-music section", "Filler tangent",       "Highlight", "Exclusive access"};

static int cat_index(unsigned flag)
{
    for (int i = 0; i < SB_CAT_COUNT; i++)
    {
        if (flag == (1u << i))
            return i;
    }
    return -1;
}

unsigned sb_category_from_name(const char *name)
{
    if (!name)
        return 0;
    for (int i = 0; i < SB_CAT_COUNT; i++)
    {
        if (!strcmp(name, k_cat_names[i]))
            return 1u << i;
    }
    return 0;
}

const char *sb_category_name(unsigned flag)
{
    int i = cat_index(flag);
    return i < 0 ? "unknown" : k_cat_names[i];
}

const char *sb_category_label(unsigned flag)
{
    int i = cat_index(flag);
    return i < 0 ? "Segment" : k_cat_labels[i];
}

unsigned sb_category_mask_parse(const char *list)
{
    unsigned mask = 0;
    char tok[32];
    size_t n = 0;

    if (!list)
        return 0;
    for (const char *p = list;; p++)
    {
        if (*p == ',' || *p == ' ' || *p == '\t' || *p == '\0' || *p == '\r' || *p == '\n')
        {
            if (n)
            {
                tok[n] = 0;
                if (!strcmp(tok, "all"))
                    mask |= SB_CAT_ALL;
                else
                    mask |= sb_category_from_name(tok);
                n = 0;
            }
            if (!*p)
                break;
        }
        else if (n < sizeof(tok) - 1)
        {
            tok[n++] = (char)tolower((unsigned char)*p);
        }
    }
    return mask;
}

void sb_config_defaults(sb_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    for (int i = 0; i < SB_CAT_COUNT; i++)
        cfg->action[i] = SB_ACT_SKIP_WITH_TOAST;
    cfg->action[cat_index(SB_CAT_FILLER)] = SB_ACT_DO_NOTHING;
    /* Highlights are a point to jump *to*, not a range to skip; they are
     * still fetched so a UI can show them. */
    cfg->action[cat_index(SB_CAT_POI_HIGHLIGHT)] = SB_ACT_DO_NOTHING;
    cfg->action[cat_index(SB_CAT_EXCLUSIVE_ACCESS)] = SB_ACT_DO_NOTHING;
    /* SmartTube doesn't request categories set to "do nothing" (filler). */
    cfg->categories = SB_CAT_ALL & ~SB_CAT_FILLER;
    cfg->min_skip_ms = 0;
    cfg->skip_once = 1;
    cfg->use_alt_server = 0;
    cfg->hash_prefix = 0;
}

sb_action sb_config_action(const sb_config *cfg, unsigned flag)
{
    int i = cat_index(flag);
    return i < 0 ? SB_ACT_DO_NOTHING : cfg->action[i];
}

int sb_valid_video_id(const char *id)
{
    if (!id || strlen(id) != 11)
        return 0;
    for (int i = 0; i < 11; i++)
    {
        char c = id[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

/* ===================================================================== */
/* minimal JSON DOM                                                      */
/* ===================================================================== */

typedef enum
{
    J_NULL,
    J_BOOL,
    J_NUM,
    J_STR,
    J_ARR,
    J_OBJ
} jtype;

typedef struct jval
{
    jtype type;
    double num;
    char *str; /* J_STR, or key when inside an object */
    char *key;
    struct jval *child; /* first element / member              */
    struct jval *next;
} jval;

typedef struct
{
    const char *p, *end;
    int depth;
} jparser;

static void jfree(jval *v)
{
    while (v)
    {
        jval *n = v->next;
        jfree(v->child);
        free(v->str);
        free(v->key);
        free(v);
        v = n;
    }
}

static void jws(jparser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static char *jstring(jparser *ps)
{
    /* ps->p at opening quote */
    size_t cap = 32, n = 0;
    char *s = malloc(cap);
    if (!s)
        return NULL;
    ps->p++;
    while (ps->p < ps->end && *ps->p != '"')
    {
        unsigned cp;
        char c = *ps->p++;
        if ((unsigned char)c < 0x20)
            goto fail;
        if (c == '\\')
        {
            if (ps->p >= ps->end)
                goto fail;
            c = *ps->p++;
            switch (c)
            {
            case '"':
            case '\\':
            case '/':
                cp = (unsigned char)c;
                break;
            case 'b':
                cp = '\b';
                break;
            case 'f':
                cp = '\f';
                break;
            case 'n':
                cp = '\n';
                break;
            case 'r':
                cp = '\r';
                break;
            case 't':
                cp = '\t';
                break;
            case 'u':
            {
                if (ps->end - ps->p < 4)
                    goto fail;
                cp = 0;
                for (int i = 0; i < 4; i++)
                {
                    int h = hexv(ps->p[i]);
                    if (h < 0)
                        goto fail;
                    cp = cp * 16 + (unsigned)h;
                }
                ps->p += 4;
                /* surrogate pair */
                if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 && ps->p[0] == '\\' &&
                    ps->p[1] == 'u')
                {
                    unsigned lo = 0;
                    int ok = 1;
                    for (int i = 0; i < 4; i++)
                    {
                        int h = hexv(ps->p[2 + i]);
                        if (h < 0)
                        {
                            ok = 0;
                            break;
                        }
                        lo = lo * 16 + (unsigned)h;
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF)
                    {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        ps->p += 6;
                    }
                }
                break;
            }
            default:
                goto fail;
            }
        }
        else
        {
            cp = (unsigned char)c; /* raw byte, UTF-8 passes through */
            if (n + 2 > cap)
            {
                char *t = realloc(s, cap *= 2);
                if (!t)
                    goto fail;
                s = t;
            }
            s[n++] = (char)cp;
            continue;
        }
        /* encode cp as UTF-8 */
        if (n + 5 > cap)
        {
            char *t = realloc(s, cap *= 2);
            if (!t)
                goto fail;
            s = t;
        }
        if (cp < 0x80)
            s[n++] = (char)cp;
        else if (cp < 0x800)
        {
            s[n++] = (char)(0xC0 | (cp >> 6));
            s[n++] = (char)(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            s[n++] = (char)(0xE0 | (cp >> 12));
            s[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s[n++] = (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            s[n++] = (char)(0xF0 | (cp >> 18));
            s[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            s[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    if (ps->p >= ps->end)
        goto fail;
    ps->p++; /* closing quote */
    s[n] = 0;
    return s;
fail:
    free(s);
    return NULL;
}

static jval *jvalue(jparser *ps);

static jval *jnew(jtype t)
{
    jval *v = calloc(1, sizeof(*v));
    if (v)
        v->type = t;
    return v;
}

static jval *jcontainer(jparser *ps, jtype t)
{
    char close = t == J_ARR ? ']' : '}';
    jval *v = jnew(t), **tail;
    if (!v)
        return NULL;
    if (++ps->depth > 32)
        goto fail;
    tail = &v->child;
    ps->p++;
    jws(ps);
    if (ps->p < ps->end && *ps->p == close)
    {
        ps->p++;
        ps->depth--;
        return v;
    }
    for (;;)
    {
        char *key = NULL;
        jval *item;
        jws(ps);
        if (t == J_OBJ)
        {
            if (ps->p >= ps->end || *ps->p != '"')
                goto fail;
            if (!(key = jstring(ps)))
                goto fail;
            jws(ps);
            if (ps->p >= ps->end || *ps->p != ':')
            {
                free(key);
                goto fail;
            }
            ps->p++;
        }
        if (!(item = jvalue(ps)))
        {
            free(key);
            goto fail;
        }
        item->key = key;
        *tail = item;
        tail = &item->next;
        jws(ps);
        if (ps->p >= ps->end)
            goto fail;
        if (*ps->p == ',')
        {
            ps->p++;
            continue;
        }
        if (*ps->p == close)
        {
            ps->p++;
            break;
        }
        goto fail;
    }
    ps->depth--;
    return v;
fail:
    jfree(v);
    return NULL;
}

static jval *jvalue(jparser *ps)
{
    jws(ps);
    if (ps->p >= ps->end)
        return NULL;
    switch (*ps->p)
    {
    case '{':
        return jcontainer(ps, J_OBJ);
    case '[':
        return jcontainer(ps, J_ARR);
    case '"':
    {
        jval *v = jnew(J_STR);
        if (!v)
            return NULL;
        if (!(v->str = jstring(ps)))
        {
            free(v);
            return NULL;
        }
        return v;
    }
    case 't':
        if (ps->end - ps->p >= 4 && !memcmp(ps->p, "true", 4))
        {
            jval *v = jnew(J_BOOL);
            if (v)
                v->num = 1;
            ps->p += 4;
            return v;
        }
        return NULL;
    case 'f':
        if (ps->end - ps->p >= 5 && !memcmp(ps->p, "false", 5))
        {
            jval *v = jnew(J_BOOL);
            ps->p += 5;
            return v;
        }
        return NULL;
    case 'n':
        if (ps->end - ps->p >= 4 && !memcmp(ps->p, "null", 4))
        {
            jval *v = jnew(J_NULL);
            ps->p += 4;
            return v;
        }
        return NULL;
    default:
    {
        char buf[64];
        size_t n = 0;
        char *endp;
        jval *v;
        while (ps->p + n < ps->end && n < sizeof(buf) - 1 && strchr("+-0123456789.eE", ps->p[n]))
            n++;
        if (!n)
            return NULL;
        memcpy(buf, ps->p, n);
        buf[n] = 0;
        v = jnew(J_NUM);
        if (!v)
            return NULL;
        v->num = strtod(buf, &endp);
        if (endp != buf + n)
        {
            free(v);
            return NULL;
        }
        ps->p += n;
        return v;
    }
    }
}

static jval *jparse(const char *s, size_t len)
{
    jparser ps = {s, s + len, 0};
    jval *v = jvalue(&ps);
    if (!v)
        return NULL;
    jws(&ps);
    if (ps.p != ps.end)
    {
        jfree(v);
        return NULL;
    }
    return v;
}

static const jval *jget(const jval *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ)
        return NULL;
    for (const jval *m = obj->child; m; m = m->next)
    {
        if (m->key && !strcmp(m->key, key))
            return m;
    }
    return NULL;
}

static void jcopy_str(char *dst, size_t cap, const jval *v)
{
    dst[0] = 0;
    if (v && v->type == J_STR)
    {
        strncpy(dst, v->str, cap - 1);
        dst[cap - 1] = 0;
    }
}

/* ===================================================================== */
/* segment parsing                                                       */
/* ===================================================================== */

static long sec_to_ms(double s)
{
    if (s < 0)
        s = 0;
    return (long)(s * 1000.0 + 0.5);
}

static int push_segment(sb_segment_list *out, size_t *cap, const jval *o)
{
    const jval *seg = jget(o, "segment"), *a, *b, *v;
    sb_segment *s;

    if (!seg || seg->type != J_ARR)
        return 0;
    a = seg->child;
    b = a ? a->next : NULL;
    if (!a || !b || a->type != J_NUM || b->type != J_NUM)
        return 0;

    if (out->count == *cap)
    {
        size_t nc = *cap ? *cap * 2 : 8;
        sb_segment *t = realloc(out->items, nc * sizeof(*t));
        if (!t)
            return -1;
        out->items = t;
        *cap = nc;
    }
    s = &out->items[out->count++];
    memset(s, 0, sizeof(*s));
    s->start_ms = sec_to_ms(a->num);
    s->end_ms = sec_to_ms(b->num);
    if (s->end_ms < s->start_ms)
        s->end_ms = s->start_ms;
    jcopy_str(s->category_name, sizeof(s->category_name), jget(o, "category"));
    s->category = sb_category_from_name(s->category_name);
    jcopy_str(s->action_type, sizeof(s->action_type), jget(o, "actionType"));
    if (!s->action_type[0])
        strcpy(s->action_type, "skip");
    jcopy_str(s->uuid, sizeof(s->uuid), jget(o, "UUID"));
    if ((v = jget(o, "videoDuration")) && v->type == J_NUM)
        s->video_duration_ms = sec_to_ms(v->num);
    if ((v = jget(o, "votes")) && v->type == J_NUM)
        s->votes = (int)v->num;
    if ((v = jget(o, "locked")) && (v->type == J_NUM || v->type == J_BOOL))
        s->locked = v->num != 0;
    return 0;
}

static int seg_cmp(const void *x, const void *y)
{
    const sb_segment *a = x, *b = y;
    if (a->start_ms != b->start_ms)
        return a->start_ms < b->start_ms ? -1 : 1;
    if (a->end_ms != b->end_ms)
        return a->end_ms < b->end_ms ? -1 : 1;
    return 0;
}

int sb_parse_segments(const char *json, size_t len, const char *video_id, sb_segment_list *out)
{
    jval *root;
    size_t cap = 0;

    out->items = NULL;
    out->count = 0;
    if (!json)
        return -1;
    if (!(root = jparse(json, len)))
        return -1;
    if (root->type != J_ARR)
    {
        jfree(root);
        return -1;
    }

    for (const jval *e = root->child; e; e = e->next)
    {
        const jval *inner;
        if (e->type != J_OBJ)
            continue;
        inner = jget(e, "segments");
        if (inner && inner->type == J_ARR)
        {
            /* hash-prefix response: filter by videoID */
            const jval *vid = jget(e, "videoID");
            if (video_id && (!vid || vid->type != J_STR || strcmp(vid->str, video_id)))
                continue;
            for (const jval *s = inner->child; s; s = s->next)
            {
                if (s->type == J_OBJ && push_segment(out, &cap, s) < 0)
                    goto oom;
            }
        }
        else if (push_segment(out, &cap, e) < 0)
        {
            goto oom;
        }
    }
    jfree(root);
    if (out->count > 1)
        qsort(out->items, out->count, sizeof(sb_segment), seg_cmp);
    return 0;
oom:
    jfree(root);
    sb_segment_list_free(out);
    return -1;
}

void sb_segment_list_free(sb_segment_list *list)
{
    if (!list)
        return;
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

/* ===================================================================== */
/* URLs / serialisation                                                  */
/* ===================================================================== */

typedef struct
{
    char *buf;
    size_t cap, len;
    int overflow;
} sbuf;

static void sb_put(sbuf *b, const char *s)
{
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap)
    {
        b->overflow = 1;
        return;
    }
    memcpy(b->buf + b->len, s, n + 1);
    b->len += n;
}

static void put_json_list_encoded(sbuf *b, const char *const *items, int n)
{
    /* ["a","b"] percent-encoded */
    sb_put(b, "%5B");
    for (int i = 0; i < n; i++)
    {
        if (i)
            sb_put(b, "%2C");
        sb_put(b, "%22");
        sb_put(b, items[i]);
        sb_put(b, "%22");
    }
    sb_put(b, "%5D");
}

int sb_build_url(char *out, size_t cap, const sb_config *cfg, const char *server,
                 const char *video_id)
{
    /* Ranges to skip, plus the highlight point and whole-video labels
     * (paid promotion, exclusive access) a player can show. */
    static const char *const actions[] = {"skip", "mute", "poi", "full"};
    const char *cats[SB_CAT_COUNT];
    int ncat = 0;
    sbuf b = {out, cap, 0, 0};

    if (!out || !cap || !sb_valid_video_id(video_id))
        return -1;
    out[0] = 0;
    if (!server)
        server = cfg->use_alt_server ? SB_SERVER_ALT : SB_SERVER_MAIN;

    for (int i = 0; i < SB_CAT_COUNT; i++)
    {
        if (cfg->categories & (1u << i))
            cats[ncat++] = k_cat_names[i];
    }
    if (!ncat)
        return -1;

    sb_put(&b, server);
    if (cfg->hash_prefix)
    {
        char hex[65];
        sb_sha256_hex(video_id, 11, hex);
        hex[4] = 0;
        sb_put(&b, "/api/skipSegments/");
        sb_put(&b, hex);
        sb_put(&b, "?categories=");
    }
    else
    {
        sb_put(&b, "/api/skipSegments?videoID=");
        sb_put(&b, video_id);
        sb_put(&b, "&categories=");
    }
    put_json_list_encoded(&b, cats, ncat);
    sb_put(&b, "&actionTypes=");
    put_json_list_encoded(&b, actions, 4);
    return b.overflow ? -1 : (int)b.len;
}

static void json_escape(char *dst, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
        {
            dst[n++] = '\\';
            dst[n++] = (char)c;
        }
        else if (c < 0x20)
            n += (size_t)snprintf(dst + n, cap - n, "\\u%04x", c);
        else
            dst[n++] = (char)c;
    }
    dst[n] = 0;
}

char *sb_segments_to_json(const sb_segment_list *list, size_t *len_out)
{
    size_t cap = 64 + list->count * 400, n = 0;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    n += (size_t)snprintf(out + n, cap - n, "[");
    for (size_t i = 0; i < list->count; i++)
    {
        const sb_segment *s = &list->items[i];
        char cat[64], act[32], uuid[160];
        json_escape(cat, sizeof(cat), s->category_name);
        json_escape(act, sizeof(act), s->action_type);
        json_escape(uuid, sizeof(uuid), s->uuid);
        n += (size_t)snprintf(out + n, cap - n,
                              "%s{\"category\":\"%s\",\"actionType\":\"%s\","
                              "\"segment\":[%ld.%03ld,%ld.%03ld],\"UUID\":\"%s\","
                              "\"videoDuration\":%ld.%03ld,\"locked\":%d,\"votes\":%d}",
                              i ? "," : "", cat, act, s->start_ms / 1000, s->start_ms % 1000,
                              s->end_ms / 1000, s->end_ms % 1000, uuid, s->video_duration_ms / 1000,
                              s->video_duration_ms % 1000, s->locked, s->votes);
    }
    n += (size_t)snprintf(out + n, cap - n, "]");
    if (len_out)
        *len_out = n;
    return out;
}

/* ===================================================================== */
/* fetch                                                                 */
/* ===================================================================== */

int sb_fetch_segments(const sb_config *cfg, const char *video_id, sb_http_get_fn get, void *ctx,
                      sb_segment_list *out)
{
    char url[1024];
    char *body = NULL;
    size_t body_len = 0;
    int status, rc;

    out->items = NULL;
    out->count = 0;
    if (sb_build_url(url, sizeof(url), cfg, NULL, video_id) < 0)
        return -2;
    status = get(ctx, url, &body, &body_len);
    if (status == 404)
    {
        free(body);
        return 0;
    } /* no segments */
    if (status != 200)
    {
        free(body);
        return status < 0 ? status : -status;
    }
    rc = sb_parse_segments(body, body_len, video_id, out);
    free(body);
    return rc;
}

/* ===================================================================== */
/* tracker                                                               */
/* ===================================================================== */

int sb_tracker_init(sb_tracker *t, const sb_config *cfg, const sb_segment_list *segs)
{
    memset(t, 0, sizeof(*t));
    t->cfg = *cfg;
    if (segs && segs->count)
    {
        t->segs.items = malloc(segs->count * sizeof(sb_segment));
        t->consumed = calloc(segs->count, 1);
        if (!t->segs.items || !t->consumed)
        {
            sb_tracker_free(t);
            return -1;
        }
        memcpy(t->segs.items, segs->items, segs->count * sizeof(sb_segment));
        t->segs.count = segs->count;
    }
    return 0;
}

void sb_tracker_free(sb_tracker *t)
{
    free(t->segs.items);
    free(t->consumed);
    memset(t, 0, sizeof(*t));
}

static int is_skip_action(sb_action a)
{
    return a == SB_ACT_SKIP_ONLY || a == SB_ACT_SKIP_WITH_TOAST;
}

static int inside(long pos, const sb_segment *s, int full, double speed)
{
    if (full)
        return pos >= s->start_ms && pos <= s->end_ms;
    long win = (long)(2000.0 * speed);
    long hi = s->start_ms + win < s->end_ms ? s->start_ms + win : s->end_ms;
    return pos >= s->start_ms && pos <= hi;
}

static int skippable_type(const sb_segment *s)
{
    /* "poi"/"full"/"chapter" aren't ranges to jump over. */
    return !strcmp(s->action_type, "skip") || !strcmp(s->action_type, "mute");
}

/* findMatchedSegments(): returns count, indices in idx[] */
static int find_matched(sb_tracker *t, long pos, int full, double speed, size_t *idx, int max)
{
    int n = 0;
    for (size_t i = 0; i < t->segs.count && n < max; i++)
    {
        const sb_segment *s = &t->segs.items[i];
        sb_action a;
        if (t->consumed[i] || !skippable_type(s))
            continue;
        if (!(t->cfg.categories & s->category))
            continue;
        a = sb_config_action(&t->cfg, s->category);
        if (!n)
        {
            if (inside(pos, s, full, speed))
            {
                idx[n++] = i;
                if (!is_skip_action(a))
                    break; /* no grouping for dialogs */
            }
        }
        else
        {
            const sb_segment *last = &t->segs.items[idx[n - 1]];
            if (is_skip_action(a) && inside(last->end_ms + 3000, s, full, speed))
                idx[n++] = i;
        }
    }
    return n;
}

int sb_tracker_poll(sb_tracker *t, long pos_ms, long duration_ms, double speed, sb_decision *out)
{
    size_t idx[16];
    int n;
    const sb_segment *last;
    sb_action type;
    long skip_pos, target, skip_dur;

    memset(out, 0, sizeof(*out));
    if (speed <= 0)
        speed = 1.0;
    n = find_matched(t, pos_ms, 0, speed, idx, 16);
    if (!n)
    {
        t->last_skip_ms = 0;
        return SB_DECIDE_NONE;
    }

    last = &t->segs.items[idx[n - 1]];
    type = sb_config_action(&t->cfg, last->category);
    skip_pos = last->end_ms;
    target = duration_ms > 0 && skip_pos > duration_ms ? duration_ms : skip_pos;
    skip_dur = target - pos_ms;

    if (skip_dur >= t->cfg.min_skip_ms && t->last_skip_ms != skip_pos)
    {
        if (is_skip_action(type))
            out->kind = SB_DECIDE_SKIP;
        else if (type == SB_ACT_SHOW_DIALOG)
            out->kind = SB_DECIDE_ASK;
    }
    if (out->kind)
    {
        out->action = type;
        out->seek_to_ms = target;
        out->segment = last;
        out->grouped = n;
    }
    t->last_skip_ms = skip_pos;

    if (t->cfg.skip_once)
    {
        for (int i = 0; i < n; i++)
            t->consumed[idx[i]] = 1;
    }
    return out->kind;
}

int sb_tracker_manual(sb_tracker *t, long pos_ms, long duration_ms, sb_decision *out)
{
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < t->segs.count; i++)
    {
        const sb_segment *s = &t->segs.items[i];
        if (!skippable_type(s))
            continue;
        if (pos_ms >= s->start_ms && pos_ms <= s->end_ms)
        {
            /* chain adjacent segments like the controller does */
            const sb_segment *last = s;
            for (size_t j = i + 1; j < t->segs.count; j++)
            {
                const sb_segment *n = &t->segs.items[j];
                if (skippable_type(n) && last->end_ms + 3000 >= n->start_ms &&
                    last->end_ms + 3000 <= n->end_ms)
                    last = n;
            }
            out->kind = SB_DECIDE_SKIP;
            out->action = SB_ACT_SKIP_ONLY;
            out->segment = last;
            out->seek_to_ms =
                duration_ms > 0 && last->end_ms > duration_ms ? duration_ms : last->end_ms;
            out->grouped = 1;
            return out->kind;
        }
    }
    return SB_DECIDE_NONE;
}

/* ===================================================================== */
/* SHA-256 (public domain style, compact)                                */
/* ===================================================================== */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, hh;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++)
    {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0];
    b = h[1];
    c = h[2];
    d = h[3];
    e = h[4];
    f = h[5];
    g = h[6];
    hh = h[7];
    for (int i = 0; i < 64; i++)
    {
        uint32_t t1 =
            hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void sb_sha256(const void *data, size_t len, uint8_t out[32])
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const uint8_t *p = data;
    uint8_t tail[128];
    size_t full = len / 64, rem = len % 64, tl;
    uint64_t bits = (uint64_t)len * 8;

    for (size_t i = 0; i < full; i++)
        sha256_block(h, p + 64 * i);
    memset(tail, 0, sizeof(tail));
    memcpy(tail, p + 64 * full, rem);
    tail[rem] = 0x80;
    tl = rem < 56 ? 64 : 128;
    for (int i = 0; i < 8; i++)
        tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha256_block(h, tail);
    if (tl == 128)
        sha256_block(h, tail + 64);
    for (int i = 0; i < 8; i++)
    {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

void sb_sha256_hex(const void *data, size_t len, char out[65])
{
    static const char hx[] = "0123456789abcdef";
    uint8_t d[32];
    sb_sha256(data, len, d);
    for (int i = 0; i < 32; i++)
    {
        out[2 * i] = hx[d[i] >> 4];
        out[2 * i + 1] = hx[d[i] & 15];
    }
    out[64] = 0;
}

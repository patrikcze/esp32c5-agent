/*
 * llm_conv.c — bounded conversation arena.
 *
 * Record layout (little-endian):
 *   u8 kind | u16 run_id | u16 total_len | body...
 * Records of one run are contiguous. When space is needed the oldest *whole*
 * run is evicted, so a tool_use is never separated from its tool_result.
 */
#include <string.h>

#include "llm_provider.h"

#define HDR 5

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

void llm_conv_init(llm_conv_t *c, uint8_t *buf, uint16_t cap)
{
    c->buf = buf;
    c->cap = cap;
    c->len = 0;
    c->evictions = 0;
}

void llm_conv_clear(llm_conv_t *c) { c->len = 0; }

static void remove_range(llm_conv_t *c, uint16_t from, uint16_t to)
{
    memmove(c->buf + from, c->buf + to, (size_t)(c->len - to));
    c->len = (uint16_t)(c->len - (to - from));
}

void llm_conv_drop_run(llm_conv_t *c, uint16_t run_id)
{
    uint16_t pos = 0;
    while (pos < c->len) {
        uint16_t rl = get16(c->buf + pos + 3);
        if (get16(c->buf + pos + 1) == run_id) {
            remove_range(c, pos, (uint16_t)(pos + rl));
        } else {
            pos = (uint16_t)(pos + rl);
        }
    }
}

bool llm_conv_evict_oldest(llm_conv_t *c, uint16_t keep_run)
{
    if (c->len == 0) return false;
    uint16_t victim = get16(c->buf + 1);
    if (victim == keep_run) return false;
    uint16_t end = 0;
    while (end < c->len && get16(c->buf + end + 1) == victim) end = (uint16_t)(end + get16(c->buf + end + 3));
    remove_range(c, 0, end);
    c->evictions++;
    return true;
}

/** Make room for @p need bytes by evicting the oldest runs != run_id. */
static bool reserve(llm_conv_t *c, uint16_t run_id, size_t need)
{
    if (need > c->cap) return false;
    while ((size_t)(c->cap - c->len) < need) {
        /* fails when the current run alone exceeds the budget */
        if (!llm_conv_evict_oldest(c, run_id)) return false;
    }
    return true;
}

typedef struct {
    uint8_t *p;
} wr_t;

static void w8(wr_t *w, uint8_t v) { *w->p++ = v; }
static void w16(wr_t *w, uint16_t v)
{
    put16(w->p, v);
    w->p += 2;
}
static void wbytes(wr_t *w, const void *d, size_t n)
{
    if (n) memcpy(w->p, d, n);
    w->p += n;
}

static uint8_t clamp8(size_t n) { return (uint8_t)(n > 255 ? 255 : n); }

bool llm_conv_add_user(llm_conv_t *c, uint16_t run_id, const char *text, size_t n)
{
    if (n > 0xFFFF - 16) return false;
    size_t need = HDR + 2 + n;
    if (!reserve(c, run_id, need)) return false;
    wr_t w = {c->buf + c->len};
    w8(&w, LLM_MSG_USER);
    w16(&w, run_id);
    w16(&w, (uint16_t)need);
    w16(&w, (uint16_t)n);
    wbytes(&w, text, n);
    c->len = (uint16_t)(c->len + need);
    return true;
}

bool llm_conv_add_assistant(llm_conv_t *c, uint16_t run_id, const char *text, size_t n, const llm_call_view_t *calls,
                            uint8_t ncalls)
{
    if (ncalls > LLM_MAX_CALLS) return false;
    size_t need = HDR + 2 + n + 1;
    for (uint8_t i = 0; i < ncalls; i++) need += 1 + clamp8(calls[i].id.n) + 1 + clamp8(calls[i].name.n) + 2 + calls[i].args.n;
    if (need > 0xFFFF) return false;
    if (!reserve(c, run_id, need)) return false;
    wr_t w = {c->buf + c->len};
    w8(&w, LLM_MSG_ASSISTANT);
    w16(&w, run_id);
    w16(&w, (uint16_t)need);
    w16(&w, (uint16_t)n);
    wbytes(&w, text, n);
    w8(&w, ncalls);
    for (uint8_t i = 0; i < ncalls; i++) {
        w8(&w, clamp8(calls[i].id.n));
        wbytes(&w, calls[i].id.p, clamp8(calls[i].id.n));
        w8(&w, clamp8(calls[i].name.n));
        wbytes(&w, calls[i].name.p, clamp8(calls[i].name.n));
        w16(&w, calls[i].args.n);
        wbytes(&w, calls[i].args.p, calls[i].args.n);
    }
    c->len = (uint16_t)(c->len + need);
    return true;
}

bool llm_conv_add_results(llm_conv_t *c, uint16_t run_id, const llm_result_view_t *res, uint8_t n)
{
    if (n > LLM_MAX_CALLS) return false;
    size_t need = HDR + 1;
    for (uint8_t i = 0; i < n; i++) need += 1 + clamp8(res[i].id.n) + 1 + clamp8(res[i].name.n) + 1 + 2 + res[i].content.n;
    if (need > 0xFFFF) return false;
    if (!reserve(c, run_id, need)) return false;
    wr_t w = {c->buf + c->len};
    w8(&w, LLM_MSG_TOOL_RESULTS);
    w16(&w, run_id);
    w16(&w, (uint16_t)need);
    w8(&w, n);
    for (uint8_t i = 0; i < n; i++) {
        w8(&w, clamp8(res[i].id.n));
        wbytes(&w, res[i].id.p, clamp8(res[i].id.n));
        w8(&w, clamp8(res[i].name.n));
        wbytes(&w, res[i].name.p, clamp8(res[i].name.n));
        w8(&w, res[i].is_error ? 1 : 0);
        w16(&w, res[i].content.n);
        wbytes(&w, res[i].content.p, res[i].content.n);
    }
    c->len = (uint16_t)(c->len + need);
    return true;
}

static lla_str_t rd8s(const uint8_t **p)
{
    lla_str_t s = {(const char *)(*p + 1), **p};
    *p += 1 + s.n;
    return s;
}

static lla_str_t rd16s(const uint8_t **p)
{
    lla_str_t s = {(const char *)(*p + 2), get16(*p)};
    *p += 2 + s.n;
    return s;
}

bool llm_conv_next(const llm_conv_t *c, uint16_t *pos, llm_msg_view_t *out)
{
    if (*pos >= c->len) return false;
    const uint8_t *r = c->buf + *pos;
    memset(out, 0, sizeof(*out));
    out->kind = (llm_msg_kind_t)r[0];
    out->run_id = get16(r + 1);
    uint16_t rl = get16(r + 3);
    const uint8_t *p = r + HDR;
    switch (out->kind) {
    case LLM_MSG_USER:
        out->text = rd16s(&p);
        break;
    case LLM_MSG_ASSISTANT:
        out->text = rd16s(&p);
        out->n = *p++;
        for (uint8_t i = 0; i < out->n; i++) {
            out->calls[i].id = rd8s(&p);
            out->calls[i].name = rd8s(&p);
            out->calls[i].args = rd16s(&p);
        }
        break;
    case LLM_MSG_TOOL_RESULTS:
        out->n = *p++;
        for (uint8_t i = 0; i < out->n; i++) {
            out->results[i].id = rd8s(&p);
            out->results[i].name = rd8s(&p);
            out->results[i].is_error = *p++ != 0;
            out->results[i].content = rd16s(&p);
        }
        break;
    }
    *pos = (uint16_t)(*pos + rl);
    return true;
}

uint16_t llm_conv_count(const llm_conv_t *c)
{
    uint16_t n = 0, pos = 0;
    while (pos < c->len) {
        pos = (uint16_t)(pos + get16(c->buf + pos + 3));
        n++;
    }
    return n;
}

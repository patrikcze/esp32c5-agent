/*
 * lla_json.c — strict zero-allocation JSON tokenizer and bounded writer.
 * Recursion depth is bounded by LLA_JSON_MAX_DEPTH.
 */
#include "lla_json.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *js;
    size_t len;
    size_t pos;
    lla_jtok_t *toks;
    uint16_t max;
    uint16_t n;
} jp_t;

static void skip_ws(jp_t *p)
{
    while (p->pos < p->len) {
        char c = p->js[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        p->pos++;
    }
}

static int alloc_tok(jp_t *p, lla_jtype_t type, size_t start)
{
    if (p->n >= p->max) {
        return LLA_JSON_ERR_NOMEM;
    }
    lla_jtok_t *t = &p->toks[p->n];
    t->type = (uint8_t)type;
    t->start = (uint16_t)start;
    t->end = (uint16_t)start;
    t->size = 0;
    t->skip = 1;
    return p->n++;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_string(jp_t *p)
{
    /* p->pos at opening quote */
    size_t start = ++p->pos;
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->js[p->pos];
        if (c == '"') {
            int i = alloc_tok(p, LLA_JT_STRING, start);
            if (i < 0) return i;
            p->toks[i].end = (uint16_t)p->pos;
            p->pos++;
            return i;
        }
        if (c < 0x20) {
            return LLA_JSON_ERR_INVAL;
        }
        if (c == '\\') {
            p->pos++;
            if (p->pos >= p->len) return LLA_JSON_ERR_PART;
            char e = p->js[p->pos];
            switch (e) {
            case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
                break;
            case 'u':
                for (int k = 0; k < 4; k++) {
                    p->pos++;
                    if (p->pos >= p->len) return LLA_JSON_ERR_PART;
                    if (hexval(p->js[p->pos]) < 0) return LLA_JSON_ERR_INVAL;
                }
                break;
            default:
                return LLA_JSON_ERR_INVAL;
            }
        }
        p->pos++;
    }
    return LLA_JSON_ERR_PART;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static int parse_number(jp_t *p)
{
    size_t start = p->pos;
    const char *s = p->js;
    size_t n = p->len;
    if (p->pos < n && s[p->pos] == '-') p->pos++;
    if (p->pos >= n) return LLA_JSON_ERR_PART;
    if (s[p->pos] == '0') {
        p->pos++;
    } else if (is_digit(s[p->pos])) {
        while (p->pos < n && is_digit(s[p->pos])) p->pos++;
    } else {
        return LLA_JSON_ERR_INVAL;
    }
    if (p->pos < n && s[p->pos] == '.') {
        p->pos++;
        if (p->pos >= n) return LLA_JSON_ERR_PART;
        if (!is_digit(s[p->pos])) return LLA_JSON_ERR_INVAL;
        while (p->pos < n && is_digit(s[p->pos])) p->pos++;
    }
    if (p->pos < n && (s[p->pos] == 'e' || s[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < n && (s[p->pos] == '+' || s[p->pos] == '-')) p->pos++;
        if (p->pos >= n) return LLA_JSON_ERR_PART;
        if (!is_digit(s[p->pos])) return LLA_JSON_ERR_INVAL;
        while (p->pos < n && is_digit(s[p->pos])) p->pos++;
    }
    int i = alloc_tok(p, LLA_JT_NUMBER, start);
    if (i < 0) return i;
    p->toks[i].end = (uint16_t)p->pos;
    return i;
}

static int parse_literal(jp_t *p, const char *lit, lla_jtype_t type)
{
    size_t l = strlen(lit);
    size_t avail = p->len - p->pos;
    size_t cmp = avail < l ? avail : l;
    if (memcmp(p->js + p->pos, lit, cmp) != 0) return LLA_JSON_ERR_INVAL;
    if (avail < l) return LLA_JSON_ERR_PART;
    int i = alloc_tok(p, type, p->pos);
    if (i < 0) return i;
    p->pos += l;
    p->toks[i].end = (uint16_t)p->pos;
    return i;
}

static int parse_value(jp_t *p, int depth);

static int parse_container(jp_t *p, int depth, bool is_obj)
{
    if (depth >= LLA_JSON_MAX_DEPTH) return LLA_JSON_ERR_DEPTH;
    int idx = alloc_tok(p, is_obj ? LLA_JT_OBJECT : LLA_JT_ARRAY, p->pos);
    if (idx < 0) return idx;
    char close = is_obj ? '}' : ']';
    p->pos++;
    skip_ws(p);
    if (p->pos >= p->len) return LLA_JSON_ERR_PART;
    if (p->js[p->pos] == close) {
        p->pos++;
        p->toks[idx].end = (uint16_t)p->pos;
        return idx;
    }
    for (;;) {
        skip_ws(p);
        if (p->pos >= p->len) return LLA_JSON_ERR_PART;
        if (is_obj) {
            if (p->js[p->pos] != '"') return LLA_JSON_ERR_INVAL;
            int k = parse_string(p);
            if (k < 0) return k;
            skip_ws(p);
            if (p->pos >= p->len) return LLA_JSON_ERR_PART;
            if (p->js[p->pos] != ':') return LLA_JSON_ERR_INVAL;
            p->pos++;
        }
        int v = parse_value(p, depth + 1);
        if (v < 0) return v;
        p->toks[idx].size++;
        skip_ws(p);
        if (p->pos >= p->len) return LLA_JSON_ERR_PART;
        char c = p->js[p->pos];
        if (c == ',') {
            p->pos++;
            continue;
        }
        if (c == close) {
            p->pos++;
            p->toks[idx].end = (uint16_t)p->pos;
            p->toks[idx].skip = (uint16_t)(p->n - idx);
            return idx;
        }
        return LLA_JSON_ERR_INVAL;
    }
}

static int parse_value(jp_t *p, int depth)
{
    skip_ws(p);
    if (p->pos >= p->len) return LLA_JSON_ERR_PART;
    switch (p->js[p->pos]) {
    case '{': return parse_container(p, depth, true);
    case '[': return parse_container(p, depth, false);
    case '"': return parse_string(p);
    case 't': return parse_literal(p, "true", LLA_JT_TRUE);
    case 'f': return parse_literal(p, "false", LLA_JT_FALSE);
    case 'n': return parse_literal(p, "null", LLA_JT_NULL);
    default:
        if (p->js[p->pos] == '-' || is_digit(p->js[p->pos])) return parse_number(p);
        return LLA_JSON_ERR_INVAL;
    }
}

int lla_json_parse(const char *js, size_t len, lla_jtok_t *toks, uint16_t max_toks)
{
    if (len > 0xFFFF) return LLA_JSON_ERR_TOOBIG;
    jp_t p = {.js = js, .len = len, .pos = 0, .toks = toks, .max = max_toks, .n = 0};
    int r = parse_value(&p, 0);
    if (r < 0) return r;
    skip_ws(&p);
    if (p.pos != p.len) return LLA_JSON_ERR_INVAL; /* trailing garbage */
    return p.n;
}

bool lla_json_streq(const char *js, const lla_jtok_t *t, const char *s)
{
    if (t->type != LLA_JT_STRING) return false;
    size_t l = strlen(s);
    return lla_json_len(t) == l && memcmp(js + t->start, s, l) == 0;
}

int lla_json_obj_get(const char *js, const lla_jtok_t *toks, int obj, const char *key)
{
    if (obj < 0 || toks[obj].type != LLA_JT_OBJECT) return -1;
    int i = obj + 1;
    for (uint16_t k = 0; k < toks[obj].size; k++) {
        int v = i + 1;
        if (lla_json_streq(js, &toks[i], key)) return v;
        i = lla_json_next(toks, v);
    }
    return -1;
}

int lla_json_arr_get(const lla_jtok_t *toks, int arr, int n)
{
    if (arr < 0 || toks[arr].type != LLA_JT_ARRAY || n < 0 || n >= toks[arr].size) return -1;
    int i = arr + 1;
    for (int k = 0; k < n; k++) i = lla_json_next(toks, i);
    return i;
}

static int put_utf8(char *out, size_t cap, size_t *o, uint32_t cp)
{
    char b[4];
    int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    if (*o + (size_t)n >= cap) return -1;
    memcpy(out + *o, b, (size_t)n);
    *o += (size_t)n;
    return 0;
}

static uint32_t hex4(const char *s)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 4) | (uint32_t)hexval(s[i]);
    return v;
}

int lla_json_str(const char *js, const lla_jtok_t *t, char *out, size_t cap)
{
    if (cap == 0) return -1;
    out[0] = '\0';
    if (t->type != LLA_JT_STRING) return -1;
    size_t o = 0;
    for (size_t i = t->start; i < t->end; i++) {
        char c = js[i];
        if (c != '\\') {
            if (o + 1 >= cap) return -1;
            out[o++] = c;
            continue;
        }
        char e = js[++i];
        char r;
        switch (e) {
        case 'b': r = '\b'; break;
        case 'f': r = '\f'; break;
        case 'n': r = '\n'; break;
        case 'r': r = '\r'; break;
        case 't': r = '\t'; break;
        case 'u': {
            uint32_t cp = hex4(js + i + 1);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < t->end && js[i + 1] == '\\' && js[i + 2] == 'u') {
                uint32_t lo = hex4(js + i + 3);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; /* lone surrogate */
            if (cp == 0) cp = 0xFFFD; /* never emit embedded NUL */
            if (put_utf8(out, cap, &o, cp) < 0) return -1;
            continue;
        }
        default: r = e; break; /* " \ / */
        }
        if (o + 1 >= cap) return -1;
        out[o++] = r;
    }
    out[o] = '\0';
    return (int)o;
}

bool lla_json_int(const char *js, const lla_jtok_t *t, int64_t *out)
{
    if (t->type != LLA_JT_NUMBER) return false;
    size_t n = lla_json_len(t);
    char tmp[24];
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, js + t->start, n);
    tmp[n] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (tmp[i] == '.' || tmp[i] == 'e' || tmp[i] == 'E') {
            /* Accept integral values written as 5.0 / 5e0 */
            double d;
            if (!lla_json_num(js, t, &d) || d != floor(d) || fabs(d) > 9.0e15) return false;
            *out = (int64_t)d;
            return true;
        }
    }
    char *end;
    long long v = strtoll(tmp, &end, 10);
    if (*end != '\0') return false;
    if ((v == LLONG_MAX || v == LLONG_MIN) && n > 18) return false;
    *out = (int64_t)v;
    return true;
}

bool lla_json_num(const char *js, const lla_jtok_t *t, double *out)
{
    if (t->type != LLA_JT_NUMBER) return false;
    size_t n = lla_json_len(t);
    char tmp[40];
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, js + t->start, n);
    tmp[n] = '\0';
    char *end;
    double d = strtod(tmp, &end);
    if (*end != '\0' || !isfinite(d)) return false;
    *out = d;
    return true;
}

/* ------------------------------------------------------------------ writer */

void lla_jw_init(lla_jw_t *w, char *buf, size_t cap)
{
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    w->first[0] = true;
    if (cap) buf[0] = '\0';
}

void lla_jw_put(lla_jw_t *w, const char *s, size_t n)
{
    if (w->overflow) return;
    if (w->len + n + 1 > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void sep(lla_jw_t *w)
{
    if (w->after_key) {
        w->after_key = false;
        return;
    }
    if (!w->first[w->depth]) lla_jw_put(w, ",", 1);
    w->first[w->depth] = false;
}

static void open(lla_jw_t *w, char c)
{
    sep(w);
    lla_jw_put(w, &c, 1);
    if (w->depth + 1 >= LLA_JSON_MAX_DEPTH) {
        w->overflow = true;
        return;
    }
    w->depth++;
    w->first[w->depth] = true;
}

static void close_(lla_jw_t *w, char c)
{
    if (w->depth == 0) {
        w->overflow = true;
        return;
    }
    w->depth--;
    lla_jw_put(w, &c, 1);
}

void lla_jw_obj_begin(lla_jw_t *w) { open(w, '{'); }
void lla_jw_obj_end(lla_jw_t *w) { close_(w, '}'); }
void lla_jw_arr_begin(lla_jw_t *w) { open(w, '['); }
void lla_jw_arr_end(lla_jw_t *w) { close_(w, ']'); }

void lla_jw_escaped(lla_jw_t *w, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char u[6];
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            if (c < 0x20) {
                u[0] = '\\'; u[1] = 'u'; u[2] = '0'; u[3] = '0';
                u[4] = hex[c >> 4]; u[5] = hex[c & 15];
            }
            break;
        }
        if (esc || c < 0x20) {
            lla_jw_put(w, s + i - run, run);
            run = 0;
            if (esc) lla_jw_put(w, esc, strlen(esc));
            else lla_jw_put(w, u, 6);
        } else {
            run++;
        }
    }
    lla_jw_put(w, s + n - run, run);
}

void lla_jw_key(lla_jw_t *w, const char *k)
{
    sep(w);
    lla_jw_put(w, "\"", 1);
    lla_jw_escaped(w, k, strlen(k));
    lla_jw_put(w, "\":", 2);
    w->after_key = true;
}

void lla_jw_strn(lla_jw_t *w, const char *s, size_t n)
{
    sep(w);
    lla_jw_put(w, "\"", 1);
    lla_jw_escaped(w, s, n);
    lla_jw_put(w, "\"", 1);
}

void lla_jw_str(lla_jw_t *w, const char *s) { lla_jw_strn(w, s ? s : "", s ? strlen(s) : 0); }

void lla_jw_int(lla_jw_t *w, int64_t v)
{
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    sep(w);
    lla_jw_put(w, tmp, (size_t)n);
}

void lla_jw_num(lla_jw_t *w, double v)
{
    if (!isfinite(v)) {
        lla_jw_null(w);
        return;
    }
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%.6g", v);
    sep(w);
    lla_jw_put(w, tmp, (size_t)n);
}

void lla_jw_bool(lla_jw_t *w, bool v)
{
    sep(w);
    lla_jw_put(w, v ? "true" : "false", v ? 4 : 5);
}

void lla_jw_null(lla_jw_t *w)
{
    sep(w);
    lla_jw_put(w, "null", 4);
}

void lla_jw_raw(lla_jw_t *w, const char *json, size_t n)
{
    sep(w);
    lla_jw_put(w, json, n);
}

uint32_t lla_fnv1a(uint32_t h, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

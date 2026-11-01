/*
 * lla_json.h — zero-allocation JSON tokenizer and bounded JSON writer.
 *
 * Tokenizer: strict RFC 8259 parser that fills a caller-provided token array
 * (jsmn-style). No heap use. Documents are limited to 65535 bytes because
 * token offsets are 16-bit; larger inputs are rejected, never truncated.
 *
 * Writer: appends to a caller-provided buffer. Overflow is sticky: once the
 * buffer is full every further call is a no-op and lla_jw_ok() returns false,
 * so callers check once at the end.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LLA_JT_INVALID = 0,
    LLA_JT_OBJECT,
    LLA_JT_ARRAY,
    LLA_JT_STRING, /* start/end exclude the quotes, escapes are NOT decoded */
    LLA_JT_NUMBER,
    LLA_JT_TRUE,
    LLA_JT_FALSE,
    LLA_JT_NULL,
} lla_jtype_t;

typedef struct {
    uint8_t type;    /* lla_jtype_t */
    uint16_t start;  /* byte offset of first char */
    uint16_t end;    /* byte offset one past last char */
    uint16_t size;   /* object: number of keys; array: number of elements */
    uint16_t skip;   /* number of tokens in this subtree including itself */
} lla_jtok_t;

typedef enum {
    LLA_JSON_OK = 0,
    LLA_JSON_ERR_NOMEM = -1,   /* token array too small */
    LLA_JSON_ERR_INVAL = -2,   /* syntax error */
    LLA_JSON_ERR_PART = -3,    /* input ended inside a value (truncated) */
    LLA_JSON_ERR_TOOBIG = -4,  /* input longer than 65535 bytes */
    LLA_JSON_ERR_DEPTH = -5,   /* nesting deeper than LLA_JSON_MAX_DEPTH */
} lla_json_err_t;

#define LLA_JSON_MAX_DEPTH 24

/**
 * Parse exactly one JSON value (surrounding whitespace allowed).
 * @return number of tokens used (>0) or a negative lla_json_err_t.
 */
int lla_json_parse(const char *js, size_t len, lla_jtok_t *toks, uint16_t max_toks);

/** Index of the token following the subtree rooted at @p i. */
static inline int lla_json_next(const lla_jtok_t *t, int i) { return i + t[i].skip; }

/** True if token is a string exactly equal to @p s (compared on raw bytes). */
bool lla_json_streq(const char *js, const lla_jtok_t *t, const char *s);

/** Find the value token of key @p key in object @p obj. Returns -1 if absent. */
int lla_json_obj_get(const char *js, const lla_jtok_t *toks, int obj, const char *key);

/** Index of the @p n-th element of array @p arr, or -1. */
int lla_json_arr_get(const lla_jtok_t *toks, int arr, int n);

/**
 * Decode a string token (handles all escapes incl. \uXXXX surrogate pairs →
 * UTF-8) into @p out (always NUL-terminated if cap > 0).
 * @return decoded length, or -1 if it did not fit / token is not a string.
 */
int lla_json_str(const char *js, const lla_jtok_t *t, char *out, size_t cap);

/** Parse a number token as int64 (rejects fractions/exponents/overflow). */
bool lla_json_int(const char *js, const lla_jtok_t *t, int64_t *out);

/** Parse a number token as double. */
bool lla_json_num(const char *js, const lla_jtok_t *t, double *out);

/** Raw bytes of a token as a slice; for strings this excludes quotes. */
static inline size_t lla_json_len(const lla_jtok_t *t) { return (size_t)(t->end - t->start); }

/* ------------------------------------------------------------------ writer */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
    uint8_t depth;
    bool first[LLA_JSON_MAX_DEPTH]; /* no element written yet at this depth */
    bool after_key;
} lla_jw_t;

void lla_jw_init(lla_jw_t *w, char *buf, size_t cap);
static inline bool lla_jw_ok(const lla_jw_t *w) { return !w->overflow && w->depth == 0; }
void lla_jw_obj_begin(lla_jw_t *w);
void lla_jw_obj_end(lla_jw_t *w);
void lla_jw_arr_begin(lla_jw_t *w);
void lla_jw_arr_end(lla_jw_t *w);
void lla_jw_key(lla_jw_t *w, const char *k);
void lla_jw_str(lla_jw_t *w, const char *s);
void lla_jw_strn(lla_jw_t *w, const char *s, size_t n);
void lla_jw_int(lla_jw_t *w, int64_t v);
void lla_jw_num(lla_jw_t *w, double v); /* NaN/Inf are written as null */
void lla_jw_bool(lla_jw_t *w, bool v);
void lla_jw_null(lla_jw_t *w);
/** Insert pre-validated JSON verbatim as a value. Caller guarantees validity. */
void lla_jw_raw(lla_jw_t *w, const char *json, size_t n);
/** Write @p s as the *content* of a JSON string, escaped, without quotes. */
void lla_jw_escaped(lla_jw_t *w, const char *s, size_t n);
/** Low level: append bytes unchanged (no separators). */
void lla_jw_put(lla_jw_t *w, const char *s, size_t n);

/* Convenience key/value helpers. */
static inline void lla_jw_kv_str(lla_jw_t *w, const char *k, const char *v) { lla_jw_key(w, k); lla_jw_str(w, v); }
static inline void lla_jw_kv_int(lla_jw_t *w, const char *k, int64_t v) { lla_jw_key(w, k); lla_jw_int(w, v); }
static inline void lla_jw_kv_num(lla_jw_t *w, const char *k, double v) { lla_jw_key(w, k); lla_jw_num(w, v); }
static inline void lla_jw_kv_bool(lla_jw_t *w, const char *k, bool v) { lla_jw_key(w, k); lla_jw_bool(w, v); }

/** FNV-1a 32-bit hash, used for loop detection and dedup keys. */
uint32_t lla_fnv1a(uint32_t h, const void *data, size_t n);
#define LLA_FNV_INIT 2166136261u

#ifdef __cplusplus
}
#endif

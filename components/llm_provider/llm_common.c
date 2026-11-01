/*
 * llm_common.c — provider registry, bounded parser driver (non-stream body
 * accumulation and SSE line assembly), response validation and helpers.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "llm_provider.h"
#include "lla_port.h"

static const char *TAG = "llm";

static const llm_provider_t *const s_providers[LLM_KIND_COUNT] = {
    [LLM_KIND_OPENAI] = &llm_provider_openai,
    [LLM_KIND_ANTHROPIC] = &llm_provider_anthropic,
};

const llm_provider_t *llm_provider_get(llm_kind_t kind)
{
    return (unsigned)kind < LLM_KIND_COUNT ? s_providers[kind] : NULL;
}

const llm_provider_t *llm_provider_by_name(const char *name)
{
    for (int i = 0; i < LLM_KIND_COUNT; i++) {
        if (s_providers[i] && strcmp(s_providers[i]->name, name) == 0) return s_providers[i];
    }
    return NULL;
}

void llm_cfg_default(llm_cfg_t *c, llm_kind_t kind)
{
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->max_tokens = 512;
    c->temperature = 0.2f;
    c->stream = true;
    c->timeout_ms = 60000;
    if (kind == LLM_KIND_ANTHROPIC) {
        strcpy(c->base_url, "https://api.anthropic.com");
        strcpy(c->model, "claude-haiku-5-5");
        c->prompt_cache = true;
        c->temperature = -1; /* current Claude models reject `temperature`; omit it */
    } else {
        strcpy(c->base_url, "http://192.168.1.100:8080/v1");
        strcpy(c->model, "local-model");
    }
}

const char *llm_perr_str(int e)
{
    switch (e) {
    case LLM_OK: return "ok";
    case LLM_ERR_TRUNCATED: return "truncated";
    case LLM_ERR_MALFORMED: return "malformed";
    case LLM_ERR_TOO_LARGE: return "too_large";
    case LLM_ERR_PROTOCOL: return "protocol";
    case LLM_ERR_API: return "api_error";
    default: return "?";
    }
}

const char *llm_stop_str(llm_stop_t s)
{
    static const char *const n[] = {"none", "end_turn", "tool_use", "max_tokens", "filtered", "other"};
    return (unsigned)s < 6 ? n[s] : "?";
}

const char *llm_tr_str(llm_tr_err_t e)
{
    static const char *const n[] = {"ok", "config", "connect", "tls", "timeout", "io", "canceled", "aborted", "no_network"};
    return (unsigned)e < 9 ? n[e] : "?";
}

bool llm_join_url(char *out, size_t cap, const char *base, const char *path)
{
    size_t bl = strlen(base);
    while (bl && base[bl - 1] == '/') bl--;
    int n = snprintf(out, cap, "%.*s%s", (int)bl, base, path);
    return n > 0 && (size_t)n < cap;
}

void llm_resp_reset(llm_response_t *r) { memset(r, 0, sizeof(*r)); }

void llm_set_err(llm_parser_t *p, int err)
{
    if (p->err == 0) p->err = err;
}

void llm_copy_tok(const char *js, const lla_jtok_t *t, char *dst, size_t cap, bool *overflow)
{
    if (lla_json_str(js, t, dst, cap) < 0) {
        dst[0] = '\0';
        *overflow = true;
    }
}

bool llm_append_tok(const char *js, const lla_jtok_t *t, char *dst, size_t cap, uint16_t *len)
{
    if (*len >= cap) return false;
    int n = lla_json_str(js, t, dst + *len, cap - *len);
    if (n < 0) {
        dst[*len] = '\0';
        return false;
    }
    *len = (uint16_t)(*len + n);
    return true;
}

void llm_append_text(llm_parser_t *p, const char *js, const lla_jtok_t *t)
{
    if (t->type != LLA_JT_STRING) return;
    llm_response_t *r = p->resp;
    if (!llm_append_tok(js, t, r->text, sizeof(r->text), &r->text_len)) r->diag |= LLM_DIAG_TEXT_TRUNC;
}

void llm_parse_error_body(llm_parser_t *p, const char *js, size_t n)
{
    llm_response_t *r = p->resp;
    int nt = lla_json_parse(js, n, p->toks, p->max_toks);
    if (nt < 0 || p->toks[0].type != LLA_JT_OBJECT) {
        /* Not JSON (e.g. proxy HTML page): keep a printable prefix. */
        size_t k = 0;
        for (size_t i = 0; i < n && k + 1 < sizeof(r->err_msg); i++) {
            char c = js[i];
            r->err_msg[k++] = (c >= 0x20 && c < 0x7f) ? c : ' ';
        }
        r->err_msg[k] = '\0';
        strcpy(r->err_type, "http_error");
        return;
    }
    bool ov = false;
    int e = lla_json_obj_get(js, p->toks, 0, "error");
    if (e >= 0 && p->toks[e].type == LLA_JT_STRING) {
        llm_copy_tok(js, &p->toks[e], r->err_msg, sizeof(r->err_msg), &ov);
        strcpy(r->err_type, "error");
        return;
    }
    if (e >= 0 && p->toks[e].type == LLA_JT_OBJECT) {
        int m = lla_json_obj_get(js, p->toks, e, "message");
        int t = lla_json_obj_get(js, p->toks, e, "type");
        if (m >= 0) llm_copy_tok(js, &p->toks[m], r->err_msg, sizeof(r->err_msg), &ov);
        if (t >= 0) llm_copy_tok(js, &p->toks[t], r->err_type, sizeof(r->err_type), &ov);
    }
    if (!r->err_type[0]) strcpy(r->err_type, "error");
}

int llm_parse_models(const char *js, size_t n, lla_jtok_t *toks, uint16_t max_toks,
                     void (*cb)(void *u, const char *id), void *u)
{
    int nt = lla_json_parse(js, n, toks, max_toks);
    if (nt < 0) return nt;
    int data = lla_json_obj_get(js, toks, 0, "data");
    if (data < 0 || toks[data].type != LLA_JT_ARRAY) data = lla_json_obj_get(js, toks, 0, "models"); /* ollama native */
    if (data < 0 || toks[data].type != LLA_JT_ARRAY) return LLA_JSON_ERR_INVAL;
    int count = 0;
    for (int i = 0; i < toks[data].size; i++) {
        int it = lla_json_arr_get(toks, data, i);
        int id = lla_json_obj_get(js, toks, it, "id");
        if (id < 0) id = lla_json_obj_get(js, toks, it, "name");
        char buf[192]; /* llama-server reports the full GGUF path as the id */
        if (id < 0) continue;
        if (lla_json_str(js, &toks[id], buf, sizeof(buf)) < 0) snprintf(buf, sizeof(buf), "(id longer than %u bytes)", (unsigned)sizeof(buf) - 1);
        cb(u, buf);
        count++;
    }
    return count;
}

/* ------------------------------------------------------------- driver */

void llm_parser_init(llm_parser_t *p, const llm_provider_t *prov, const llm_cfg_t *cfg, llm_response_t *resp,
                     char *buf, size_t cap, lla_jtok_t *toks, uint16_t max_toks)
{
    memset(p, 0, sizeof(*p));
    p->prov = prov;
    p->cfg = cfg;
    p->resp = resp;
    p->stream = cfg->stream;
    p->buf = buf;
    p->cap = cap;
    p->toks = toks;
    p->max_toks = max_toks;
    memset(p->block_call, -1, sizeof(p->block_call));
    memset(p->idx_call, -1, sizeof(p->idx_call));
    llm_resp_reset(resp);
}

void llm_parser_status(llm_parser_t *p, int status) { p->http_status = status; }

void llm_parser_header(llm_parser_t *p, const char *k, const char *v)
{
    if (strcasecmp(k, "retry-after") == 0) {
        long s = strtol(v, NULL, 10);
        if (s < 0) s = 0;
        if (s > 60) s = 60;
        p->retry_after_ms = (uint32_t)s * 1000u;
    }
}

static void sse_line(llm_parser_t *p, char *line, size_t n)
{
    if (n && line[n - 1] == '\r') n--;
    line[n] = '\0';
    if (n == 0) {
        p->event[0] = '\0'; /* end of event block */
        return;
    }
    if (line[0] == ':') return; /* comment / keep-alive */
    if (n >= 6 && memcmp(line, "event:", 6) == 0) {
        const char *v = line + 6;
        while (*v == ' ') v++;
        snprintf(p->event, sizeof(p->event), "%s", v);
        return;
    }
    if (n >= 5 && memcmp(line, "data:", 5) == 0) {
        const char *v = line + 5;
        if (*v == ' ') v++;
        size_t vl = n - (size_t)(v - line);
        p->any_data = true;
        if (vl == 6 && memcmp(v, "[DONE]", 6) == 0) {
            p->done_marker = true;
            return;
        }
        if (p->done_marker) {
            llm_set_err(p, LLM_ERR_PROTOCOL); /* data after [DONE] */
            return;
        }
        p->prov->on_sse(p, v, vl);
        return;
    }
    /* "id:" / "retry:" and unknown fields are ignored per the SSE spec. */
}

/* stream mode only becomes SSE once we see something other than '{' */
enum { MODE_UNKNOWN = 0, MODE_SSE, MODE_BODY };

int llm_parser_feed(llm_parser_t *p, const char *d, size_t n)
{
    if (p->err) return p->err;
    bool body_mode = !p->stream || p->http_status != 200;
    if (p->stream && p->http_status == 200 && p->len == 0 && !p->any_data && p->event[0] == '\0') {
        /* Servers that ignore stream:true reply with plain JSON. Detect it. */
        size_t i = 0;
        while (i < n && isspace((unsigned char)d[i])) i++;
        if (i < n && d[i] == '{') {
            p->stream = false;
            body_mode = true;
        }
    }
    if (body_mode) {
        if (p->len + n >= p->cap) {
            if (p->http_status != 200) { /* error bodies may be truncated */
                n = p->cap - 1 - p->len;
            } else {
                llm_set_err(p, LLM_ERR_TOO_LARGE);
                return p->err;
            }
        }
        memcpy(p->buf + p->len, d, n);
        p->len += n;
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char c = d[i];
        if (c == '\n') {
            sse_line(p, p->buf, p->len);
            p->len = 0;
            if (p->err) return p->err;
            continue;
        }
        if (p->len + 1 >= p->cap) {
            llm_set_err(p, LLM_ERR_TOO_LARGE);
            return p->err;
        }
        p->buf[p->len++] = c;
    }
    return 0;
}

static bool looks_like_tool_call(const char *t)
{
    static const char *const pat[] = {"<tool_call>", "[TOOL_CALLS]", "<|python_tag|>", "<function=", "\"arguments\"",
                                      "<|tool_call", NULL};
    for (int i = 0; pat[i]; i++) {
        if (strstr(t, pat[i])) return true;
    }
    return false;
}

static int validate(llm_parser_t *p)
{
    llm_response_t *r = p->resp;
    if (r->stop == LLM_STOP_NONE) return LLM_ERR_PROTOCOL;
    for (uint8_t i = 0; i < r->n_calls; i++) {
        llm_tool_call_t *c = &r->calls[i];
        if (!c->complete) return LLM_ERR_TRUNCATED;
        if (!c->name[0]) return LLM_ERR_PROTOCOL;
        if (!c->id[0]) {
            if (!p->cfg->allow_missing_ids) return LLM_ERR_PROTOCOL;
            snprintf(c->id, sizeof(c->id), "call_lla_%u", (unsigned)i);
            c->synth_id = true;
            r->diag |= LLM_DIAG_SYNTH_ID;
        }
        for (uint8_t j = 0; j < i; j++) {
            if (strcmp(r->calls[j].id, c->id) == 0) return LLM_ERR_PROTOCOL; /* duplicate ids */
        }
        if (c->overflow) {
            c->invalid = "arguments_too_large";
            continue;
        }
        if (c->args_len == 0) {
            strcpy(c->args, "{}");
            c->args_len = 2;
        }
        lla_jtok_t t[2];
        int nt = lla_json_parse(c->args, c->args_len, t, 2);
        /* only need to know the root is a complete object; NOMEM is fine */
        if (nt == LLA_JSON_ERR_NOMEM) {
            /* re-check syntax with the big token array */
            nt = lla_json_parse(c->args, c->args_len, p->toks, p->max_toks);
            if (nt > 0 && p->toks[0].type != LLA_JT_OBJECT) nt = LLA_JSON_ERR_INVAL;
        } else if (nt > 0 && t[0].type != LLA_JT_OBJECT) {
            nt = LLA_JSON_ERR_INVAL;
        }
        if (nt < 0) c->invalid = "arguments_not_a_json_object";
    }
    if (r->n_calls > 0 && r->stop == LLM_STOP_END) {
        r->diag |= LLM_DIAG_FINISH_MISMATCH;
        r->stop = LLM_STOP_TOOL_USE;
    }
    if (r->n_calls == 0 && r->stop == LLM_STOP_TOOL_USE) return LLM_ERR_PROTOCOL;
    if (r->text_len && looks_like_tool_call(r->text)) r->diag |= LLM_DIAG_TOOLCALL_IN_TEXT;
    return LLM_OK;
}

int llm_parser_finish(llm_parser_t *p)
{
    if (p->http_status != 200) {
        p->buf[p->len < p->cap ? p->len : p->cap - 1] = '\0';
        llm_parse_error_body(p, p->buf, p->len);
        return LLM_ERR_API;
    }
    if (p->err) return p->err;
    if (!p->stream) {
        if (p->len == 0) return LLM_ERR_TRUNCATED;
        p->prov->on_body(p, p->buf, p->len);
    } else if (p->len) {
        sse_line(p, p->buf, p->len); /* final line without newline */
        p->len = 0;
    }
    if (p->err) return p->err;
    if (p->resp->err_type[0]) return LLM_ERR_API; /* in-stream error event */
    if (!p->finished) return LLM_ERR_TRUNCATED;
    if (p->stream && p->prov->kind == LLM_KIND_OPENAI && !p->done_marker) p->resp->diag |= LLM_DIAG_NO_DONE;
    int v = validate(p);
    if (v != LLM_OK) LLA_LOGW(TAG, "response validation failed: %s", llm_perr_str(v));
    return v;
}

static void sink_status(void *u, int s) { llm_parser_status((llm_parser_t *)u, s); }
static void sink_header(void *u, const char *k, const char *v) { llm_parser_header((llm_parser_t *)u, k, v); }
static int sink_data(void *u, const char *d, size_t n) { return llm_parser_feed((llm_parser_t *)u, d, n); }

void llm_parser_sink(llm_parser_t *p, llm_http_sink_t *sink)
{
    sink->on_status = sink_status;
    sink->on_header = sink_header;
    sink->on_data = sink_data;
    sink->u = p;
}

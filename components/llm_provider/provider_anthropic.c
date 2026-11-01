/*
 * provider_anthropic.c — native Anthropic Messages API (tool_use/tool_result).
 *
 * Streaming events handled: message_start, content_block_start/delta/stop,
 * message_delta, message_stop, ping, error. A response is only accepted after
 * message_stop; every tool_use block must have received content_block_stop.
 */
#include <stdio.h>
#include <string.h>

#include "llm_provider.h"

#define ANTHROPIC_VERSION "2023-06-01"

static bool an_build(const llm_cfg_t *cfg, const llm_req_ctx_t *ctx, lla_jw_t *w)
{
    lla_jw_obj_begin(w);
    lla_jw_kv_str(w, "model", cfg->model);
    lla_jw_kv_int(w, "max_tokens", cfg->max_tokens);
    if (cfg->temperature >= 0) lla_jw_kv_num(w, "temperature", cfg->temperature);
    if (cfg->stream) lla_jw_kv_bool(w, "stream", true);
    if (ctx->system_prompt && ctx->system_prompt[0]) {
        lla_jw_key(w, "system");
        if (cfg->prompt_cache) {
            /* Cache breakpoint on the system block caches tools + system
             * (tools precede system in the cache prefix order). */
            lla_jw_arr_begin(w);
            lla_jw_obj_begin(w);
            lla_jw_kv_str(w, "type", "text");
            lla_jw_kv_str(w, "text", ctx->system_prompt);
            lla_jw_key(w, "cache_control");
            lla_jw_obj_begin(w);
            lla_jw_kv_str(w, "type", "ephemeral");
            lla_jw_obj_end(w);
            lla_jw_obj_end(w);
            lla_jw_arr_end(w);
        } else {
            lla_jw_str(w, ctx->system_prompt);
        }
    }
    lla_jw_key(w, "messages");
    lla_jw_arr_begin(w);
    uint16_t pos = 0;
    llm_msg_view_t m;
    while (llm_conv_next(ctx->conv, &pos, &m)) {
        lla_jw_obj_begin(w);
        switch (m.kind) {
        case LLM_MSG_USER:
            lla_jw_kv_str(w, "role", "user");
            lla_jw_key(w, "content");
            lla_jw_strn(w, m.text.p, m.text.n);
            break;
        case LLM_MSG_ASSISTANT:
            lla_jw_kv_str(w, "role", "assistant");
            lla_jw_key(w, "content");
            lla_jw_arr_begin(w);
            if (m.text.n || m.n == 0) {
                lla_jw_obj_begin(w);
                lla_jw_kv_str(w, "type", "text");
                lla_jw_key(w, "text");
                if (m.text.n) lla_jw_strn(w, m.text.p, m.text.n);
                else lla_jw_str(w, "(no text)");
                lla_jw_obj_end(w);
            }
            for (uint8_t i = 0; i < m.n; i++) {
                lla_jw_obj_begin(w);
                lla_jw_kv_str(w, "type", "tool_use");
                lla_jw_key(w, "id");
                lla_jw_strn(w, m.calls[i].id.p, m.calls[i].id.n);
                lla_jw_key(w, "name");
                lla_jw_strn(w, m.calls[i].name.p, m.calls[i].name.n);
                lla_jw_key(w, "input");
                /* args stored in the conversation are always a validated object */
                lla_jw_raw(w, m.calls[i].args.p, m.calls[i].args.n);
                lla_jw_obj_end(w);
            }
            lla_jw_arr_end(w);
            break;
        case LLM_MSG_TOOL_RESULTS:
            lla_jw_kv_str(w, "role", "user");
            lla_jw_key(w, "content");
            lla_jw_arr_begin(w);
            for (uint8_t i = 0; i < m.n; i++) {
                lla_jw_obj_begin(w);
                lla_jw_kv_str(w, "type", "tool_result");
                lla_jw_key(w, "tool_use_id");
                lla_jw_strn(w, m.results[i].id.p, m.results[i].id.n);
                lla_jw_key(w, "content");
                lla_jw_strn(w, m.results[i].content.p, m.results[i].content.n);
                if (m.results[i].is_error) lla_jw_kv_bool(w, "is_error", true);
                lla_jw_obj_end(w);
            }
            lla_jw_arr_end(w);
            break;
        }
        lla_jw_obj_end(w);
    }
    lla_jw_arr_end(w);
    if (ctx->tools) {
        size_t mark = w->len;
        lla_jw_key(w, "tools");
        size_t arr_start = w->len;
        tool_registry_write_schemas(ctx->tools, ctx->src, TOOL_FMT_ANTHROPIC, w);
        if (!w->overflow && w->len - arr_start == 2) {
            w->len = mark;
            w->buf[w->len] = '\0';
        }
    }
    lla_jw_obj_end(w);
    return lla_jw_ok(w);
}

static uint8_t an_headers(const llm_cfg_t *cfg, llm_header_t *h, uint8_t max, char *scratch, size_t scratch_cap)
{
    (void)max;
    uint8_t n = 0;
    h[n++] = (llm_header_t){"Content-Type", "application/json"};
    h[n++] = (llm_header_t){"anthropic-version", ANTHROPIC_VERSION};
    if (cfg->api_key[0]) {
        snprintf(scratch, scratch_cap, "%s", cfg->api_key);
        h[n++] = (llm_header_t){"x-api-key", scratch};
    }
    if (cfg->stream) h[n++] = (llm_header_t){"Accept", "text/event-stream"};
    return n;
}

static llm_stop_t map_stop(const char *s)
{
    if (strcmp(s, "end_turn") == 0 || strcmp(s, "stop_sequence") == 0) return LLM_STOP_END;
    if (strcmp(s, "tool_use") == 0) return LLM_STOP_TOOL_USE;
    if (strcmp(s, "max_tokens") == 0) return LLM_STOP_MAX_TOKENS;
    if (strcmp(s, "refusal") == 0) return LLM_STOP_FILTER;
    return LLM_STOP_OTHER; /* pause_turn, model_context_window_exceeded, ... */
}

static void set_stop(llm_parser_t *p, const char *js, const lla_jtok_t *t)
{
    if (t->type != LLA_JT_STRING) return;
    bool ov = false;
    llm_copy_tok(js, t, p->resp->stop_raw, sizeof(p->resp->stop_raw), &ov);
    p->resp->stop = map_stop(p->resp->stop_raw);
}

static void usage(llm_parser_t *p, const char *js, const lla_jtok_t *tk, int obj)
{
    int u = lla_json_obj_get(js, tk, obj, "usage");
    if (u < 0) return;
    int64_t v;
    int a = lla_json_obj_get(js, tk, u, "input_tokens");
    int b = lla_json_obj_get(js, tk, u, "output_tokens");
    if (a >= 0 && lla_json_int(js, &tk[a], &v)) p->resp->in_tokens = (uint32_t)v;
    if (b >= 0 && lla_json_int(js, &tk[b], &v)) p->resp->out_tokens = (uint32_t)v;
}

/** Start a tool_use call from a content block object. Returns slot or -1. */
static int start_tool(llm_parser_t *p, const char *js, const lla_jtok_t *tk, int blk)
{
    llm_response_t *r = p->resp;
    if (r->n_calls >= LLM_MAX_CALLS) {
        r->diag |= LLM_DIAG_EXTRA_CALLS;
        return -1;
    }
    llm_tool_call_t *c = &r->calls[r->n_calls];
    bool ov = false;
    int id = lla_json_obj_get(js, tk, blk, "id");
    int nm = lla_json_obj_get(js, tk, blk, "name");
    if (id >= 0) llm_copy_tok(js, &tk[id], c->id, sizeof(c->id), &ov);
    if (nm >= 0) llm_copy_tok(js, &tk[nm], c->name, sizeof(c->name), &ov);
    if (ov) {
        llm_set_err(p, LLM_ERR_PROTOCOL);
        return -1;
    }
    c->started = true;
    return r->n_calls++;
}

static void an_on_sse(llm_parser_t *p, const char *js, size_t n)
{
    int nt = lla_json_parse(js, n, p->toks, p->max_toks);
    if (nt < 0) {
        llm_set_err(p, nt == LLA_JSON_ERR_NOMEM ? LLM_ERR_TOO_LARGE : LLM_ERR_MALFORMED);
        return;
    }
    const lla_jtok_t *tk = p->toks;
    int ty = lla_json_obj_get(js, tk, 0, "type");
    if (ty < 0) {
        llm_set_err(p, LLM_ERR_PROTOCOL);
        return;
    }
    llm_response_t *r = p->resp;
    if (lla_json_streq(js, &tk[ty], "message_start")) {
        int msg = lla_json_obj_get(js, tk, 0, "message");
        if (msg >= 0) usage(p, js, tk, msg);
    } else if (lla_json_streq(js, &tk[ty], "content_block_start")) {
        int ix = lla_json_obj_get(js, tk, 0, "index");
        int blk = lla_json_obj_get(js, tk, 0, "content_block");
        int64_t index;
        if (ix < 0 || blk < 0 || !lla_json_int(js, &tk[ix], &index)) {
            llm_set_err(p, LLM_ERR_PROTOCOL);
            return;
        }
        int bt = lla_json_obj_get(js, tk, blk, "type");
        if (bt >= 0 && lla_json_streq(js, &tk[bt], "tool_use")) {
            int s = start_tool(p, js, tk, blk);
            if (s >= 0) {
                if (index < 0 || index >= (int64_t)sizeof(p->block_call)) {
                    llm_set_err(p, LLM_ERR_PROTOCOL);
                    return;
                }
                p->block_call[index] = (int8_t)s;
            }
        } else if (bt >= 0 && lla_json_streq(js, &tk[bt], "text")) {
            int tx = lla_json_obj_get(js, tk, blk, "text");
            if (tx >= 0) llm_append_text(p, js, &tk[tx]);
        }
    } else if (lla_json_streq(js, &tk[ty], "content_block_delta")) {
        int ix = lla_json_obj_get(js, tk, 0, "index");
        int d = lla_json_obj_get(js, tk, 0, "delta");
        int64_t index;
        if (ix < 0 || d < 0 || !lla_json_int(js, &tk[ix], &index)) {
            llm_set_err(p, LLM_ERR_PROTOCOL);
            return;
        }
        int dt = lla_json_obj_get(js, tk, d, "type");
        if (dt >= 0 && lla_json_streq(js, &tk[dt], "text_delta")) {
            int tx = lla_json_obj_get(js, tk, d, "text");
            if (tx >= 0) llm_append_text(p, js, &tk[tx]);
        } else if (dt >= 0 && lla_json_streq(js, &tk[dt], "input_json_delta")) {
            if (index < 0 || index >= (int64_t)sizeof(p->block_call)) {
                llm_set_err(p, LLM_ERR_PROTOCOL);
                return;
            }
            int s = p->block_call[index];
            if (s < 0) return; /* extra call beyond LLM_MAX_CALLS, dropped */
            llm_tool_call_t *c = &r->calls[s];
            if (c->complete) {
                llm_set_err(p, LLM_ERR_PROTOCOL);
                return;
            }
            int pj = lla_json_obj_get(js, tk, d, "partial_json");
            if (pj >= 0 && !c->overflow &&
                !llm_append_tok(js, &tk[pj], c->args, sizeof(c->args), &c->args_len)) {
                c->overflow = true;
            }
        }
        /* thinking_delta / signature_delta: not requested, ignored */
    } else if (lla_json_streq(js, &tk[ty], "content_block_stop")) {
        int ix = lla_json_obj_get(js, tk, 0, "index");
        int64_t index;
        if (ix >= 0 && lla_json_int(js, &tk[ix], &index) && index >= 0 && index < (int64_t)sizeof(p->block_call) &&
            p->block_call[index] >= 0) {
            r->calls[p->block_call[index]].complete = true;
        }
    } else if (lla_json_streq(js, &tk[ty], "message_delta")) {
        int d = lla_json_obj_get(js, tk, 0, "delta");
        if (d >= 0) {
            int sr = lla_json_obj_get(js, tk, d, "stop_reason");
            if (sr >= 0) set_stop(p, js, &tk[sr]);
        }
        usage(p, js, tk, 0);
    } else if (lla_json_streq(js, &tk[ty], "message_stop")) {
        p->finished = true;
        p->done_marker = true;
    } else if (lla_json_streq(js, &tk[ty], "ping")) {
        /* keep-alive */
    } else if (lla_json_streq(js, &tk[ty], "error")) {
        llm_parse_error_body(p, js, n);
    } else {
        r->diag |= LLM_DIAG_UNKNOWN_EVENT;
    }
}

static void an_on_body(llm_parser_t *p, const char *js, size_t n)
{
    int nt = lla_json_parse(js, n, p->toks, p->max_toks);
    if (nt < 0) {
        llm_set_err(p, nt == LLA_JSON_ERR_PART ? LLM_ERR_TRUNCATED : nt == LLA_JSON_ERR_NOMEM ? LLM_ERR_TOO_LARGE
                                                                                               : LLM_ERR_MALFORMED);
        return;
    }
    const lla_jtok_t *tk = p->toks;
    int ty = lla_json_obj_get(js, tk, 0, "type");
    if (ty >= 0 && lla_json_streq(js, &tk[ty], "error")) {
        llm_parse_error_body(p, js, n);
        return;
    }
    int content = lla_json_obj_get(js, tk, 0, "content");
    int sr = lla_json_obj_get(js, tk, 0, "stop_reason");
    if (content < 0 || tk[content].type != LLA_JT_ARRAY || sr < 0) {
        llm_set_err(p, LLM_ERR_PROTOCOL);
        return;
    }
    for (int i = 0; i < tk[content].size; i++) {
        int blk = lla_json_arr_get(tk, content, i);
        int bt = lla_json_obj_get(js, tk, blk, "type");
        if (bt < 0) continue;
        if (lla_json_streq(js, &tk[bt], "text")) {
            int tx = lla_json_obj_get(js, tk, blk, "text");
            if (tx >= 0) llm_append_text(p, js, &tk[tx]);
        } else if (lla_json_streq(js, &tk[bt], "tool_use")) {
            int s = start_tool(p, js, tk, blk);
            if (s < 0) continue;
            llm_tool_call_t *c = &p->resp->calls[s];
            int in = lla_json_obj_get(js, tk, blk, "input");
            if (in >= 0 && tk[in].type == LLA_JT_OBJECT) {
                size_t l = lla_json_len(&tk[in]);
                if (l >= sizeof(c->args)) {
                    c->overflow = true;
                } else {
                    memcpy(c->args, js + tk[in].start, l);
                    c->args[l] = '\0';
                    c->args_len = (uint16_t)l;
                }
            }
            c->complete = true;
        }
    }
    set_stop(p, js, &tk[sr]);
    usage(p, js, tk, 0);
    p->finished = true;
}

const llm_provider_t llm_provider_anthropic = {
    .name = "anthropic",
    .kind = LLM_KIND_ANTHROPIC,
    .caps = {.tools = true, .streaming = true, .parallel_tool_calls = true, .requires_key = true,
             .native_tool_ids = true, .max_context_tokens = 200000},
    .chat_path = "/v1/messages",
    .models_path = "/v1/models?limit=100",
    .build = an_build,
    .headers = an_headers,
    .on_sse = an_on_sse,
    .on_body = an_on_body,
};

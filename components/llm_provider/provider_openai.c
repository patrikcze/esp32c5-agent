/*
 * provider_openai.c — OpenAI Chat Completions protocol (function calling).
 *
 * Used for OpenAI and OpenAI-compatible servers: llama.cpp llama-server,
 * Ollama (/v1), LM Studio, vLLM. Server quirks handled (all documented in
 * docs/PROVIDERS.md):
 *   - stream:true ignored → plain JSON body (detected in the driver)
 *   - finish_reason "stop" although tool_calls present → accepted with diag
 *   - tool_call "index" missing → positional index
 *   - function.arguments as JSON object instead of string → raw copy
 *   - missing tool call id → only with allow_missing_ids (diag)
 */
#include <stdio.h>
#include <string.h>

#include "llm_provider.h"

static void write_text_or_empty(lla_jw_t *w, lla_str_t s) { lla_jw_strn(w, s.p ? s.p : "", s.n); }

static bool oa_build(const llm_cfg_t *cfg, const llm_req_ctx_t *ctx, lla_jw_t *w)
{
    lla_jw_obj_begin(w);
    lla_jw_kv_str(w, "model", cfg->model);
    lla_jw_kv_int(w, "max_tokens", cfg->max_tokens);
    if (cfg->temperature >= 0) lla_jw_kv_num(w, "temperature", cfg->temperature);
    lla_jw_kv_bool(w, "stream", cfg->stream);
    lla_jw_key(w, "messages");
    lla_jw_arr_begin(w);
    if (ctx->system_prompt && ctx->system_prompt[0]) {
        lla_jw_obj_begin(w);
        lla_jw_kv_str(w, "role", "system");
        lla_jw_kv_str(w, "content", ctx->system_prompt);
        lla_jw_obj_end(w);
    }
    uint16_t pos = 0;
    llm_msg_view_t m;
    while (llm_conv_next(ctx->conv, &pos, &m)) {
        switch (m.kind) {
        case LLM_MSG_USER:
            lla_jw_obj_begin(w);
            lla_jw_kv_str(w, "role", "user");
            lla_jw_key(w, "content");
            write_text_or_empty(w, m.text);
            lla_jw_obj_end(w);
            break;
        case LLM_MSG_ASSISTANT:
            lla_jw_obj_begin(w);
            lla_jw_kv_str(w, "role", "assistant");
            lla_jw_key(w, "content");
            write_text_or_empty(w, m.text);
            if (m.n) {
                lla_jw_key(w, "tool_calls");
                lla_jw_arr_begin(w);
                for (uint8_t i = 0; i < m.n; i++) {
                    lla_jw_obj_begin(w);
                    lla_jw_key(w, "id");
                    lla_jw_strn(w, m.calls[i].id.p, m.calls[i].id.n);
                    lla_jw_kv_str(w, "type", "function");
                    lla_jw_key(w, "function");
                    lla_jw_obj_begin(w);
                    lla_jw_key(w, "name");
                    lla_jw_strn(w, m.calls[i].name.p, m.calls[i].name.n);
                    lla_jw_key(w, "arguments");
                    lla_jw_strn(w, m.calls[i].args.p, m.calls[i].args.n);
                    lla_jw_obj_end(w);
                    lla_jw_obj_end(w);
                }
                lla_jw_arr_end(w);
            }
            lla_jw_obj_end(w);
            break;
        case LLM_MSG_TOOL_RESULTS:
            for (uint8_t i = 0; i < m.n; i++) {
                lla_jw_obj_begin(w);
                lla_jw_kv_str(w, "role", "tool");
                lla_jw_key(w, "tool_call_id");
                lla_jw_strn(w, m.results[i].id.p, m.results[i].id.n);
                lla_jw_key(w, "content");
                lla_jw_strn(w, m.results[i].content.p, m.results[i].content.n);
                lla_jw_obj_end(w);
            }
            break;
        }
    }
    lla_jw_arr_end(w);
    if (ctx->tools) {
        /* write schemas into the request; omit the key entirely if none permitted */
        size_t mark = w->len;
        lla_jw_key(w, "tools");
        size_t arr_start = w->len;
        tool_registry_write_schemas(ctx->tools, ctx->src, TOOL_FMT_OPENAI, w);
        if (!w->overflow && w->len - arr_start == 2) { /* "[]" */
            w->len = mark;
            w->buf[w->len] = '\0';
        } else {
            lla_jw_kv_str(w, "tool_choice", "auto");
        }
    }
    lla_jw_obj_end(w);
    return lla_jw_ok(w);
}

static uint8_t oa_headers(const llm_cfg_t *cfg, llm_header_t *h, uint8_t max, char *scratch, size_t scratch_cap)
{
    (void)max;
    uint8_t n = 0;
    h[n++] = (llm_header_t){"Content-Type", "application/json"};
    if (cfg->api_key[0]) {
        snprintf(scratch, scratch_cap, "Bearer %s", cfg->api_key);
        h[n++] = (llm_header_t){"Authorization", scratch};
    }
    if (cfg->stream) h[n++] = (llm_header_t){"Accept", "text/event-stream"};
    return n;
}

static llm_stop_t map_finish(const char *js, const lla_jtok_t *t, char *raw, size_t cap)
{
    bool ov = false;
    llm_copy_tok(js, t, raw, cap, &ov);
    if (strcmp(raw, "stop") == 0 || strcmp(raw, "eos") == 0) return LLM_STOP_END;
    if (strcmp(raw, "tool_calls") == 0 || strcmp(raw, "function_call") == 0) return LLM_STOP_TOOL_USE;
    if (strcmp(raw, "length") == 0) return LLM_STOP_MAX_TOKENS;
    if (strcmp(raw, "content_filter") == 0) return LLM_STOP_FILTER;
    return LLM_STOP_OTHER;
}

static int slot_for(llm_parser_t *p, int index)
{
    llm_response_t *r = p->resp;
    if (index < 0 || index >= (int)sizeof(p->idx_call)) {
        r->diag |= LLM_DIAG_EXTRA_CALLS;
        return -1;
    }
    if (p->idx_call[index] >= 0) return p->idx_call[index];
    if (r->n_calls >= LLM_MAX_CALLS) {
        r->diag |= LLM_DIAG_EXTRA_CALLS;
        return -1;
    }
    p->idx_call[index] = (int8_t)r->n_calls;
    r->calls[r->n_calls].started = true;
    return r->n_calls++;
}

/** Merge one tool_calls[] element (stream delta or full message entry). */
static void merge_call(llm_parser_t *p, const char *js, const lla_jtok_t *tk, int el, int pos, bool stream)
{
    int ix = lla_json_obj_get(js, tk, el, "index");
    int64_t index = pos;
    if (ix >= 0 && !lla_json_int(js, &tk[ix], &index)) {
        llm_set_err(p, LLM_ERR_PROTOCOL);
        return;
    }
    int s = slot_for(p, (int)index);
    if (s < 0) return;
    llm_tool_call_t *c = &p->resp->calls[s];
    bool ov = false;

    int id = lla_json_obj_get(js, tk, el, "id");
    if (id >= 0 && tk[id].type == LLA_JT_STRING && lla_json_len(&tk[id]) > 0) {
        char tmp[LLM_ID_MAX];
        llm_copy_tok(js, &tk[id], tmp, sizeof(tmp), &ov);
        if (ov) {
            llm_set_err(p, LLM_ERR_PROTOCOL);
            return;
        }
        if (c->id[0] && strcmp(c->id, tmp) != 0) {
            llm_set_err(p, LLM_ERR_PROTOCOL); /* id changed mid-stream */
            return;
        }
        strcpy(c->id, tmp);
    }
    int fn = lla_json_obj_get(js, tk, el, "function");
    if (fn < 0) return;
    int nm = lla_json_obj_get(js, tk, fn, "name");
    if (nm >= 0 && tk[nm].type == LLA_JT_STRING) {
        char tmp[TOOL_NAME_MAX];
        llm_copy_tok(js, &tk[nm], tmp, sizeof(tmp), &ov);
        if (ov) {
            llm_set_err(p, LLM_ERR_PROTOCOL);
            return;
        }
        if (strcmp(c->name, tmp) != 0) {
            /* fragments are concatenated; a repeat of the full name is ignored */
            size_t l = strlen(c->name);
            if (l + strlen(tmp) >= sizeof(c->name)) {
                llm_set_err(p, LLM_ERR_PROTOCOL);
                return;
            }
            strcpy(c->name + l, tmp);
        }
    }
    int ar = lla_json_obj_get(js, tk, fn, "arguments");
    if (ar >= 0 && !c->overflow) {
        if (tk[ar].type == LLA_JT_STRING) {
            if (!llm_append_tok(js, &tk[ar], c->args, sizeof(c->args), &c->args_len)) c->overflow = true;
        } else if (tk[ar].type == LLA_JT_OBJECT && c->args_len == 0) {
            size_t l = lla_json_len(&tk[ar]);
            if (l >= sizeof(c->args)) {
                c->overflow = true;
            } else {
                memcpy(c->args, js + tk[ar].start, l);
                c->args[l] = '\0';
                c->args_len = (uint16_t)l;
            }
        }
    }
    if (!stream) c->complete = true;
}

static void parse_usage(llm_parser_t *p, const char *js, const lla_jtok_t *tk)
{
    int u = lla_json_obj_get(js, tk, 0, "usage");
    if (u < 0 || tk[u].type != LLA_JT_OBJECT) return;
    int64_t v;
    int a = lla_json_obj_get(js, tk, u, "prompt_tokens");
    int b = lla_json_obj_get(js, tk, u, "completion_tokens");
    if (a >= 0 && lla_json_int(js, &tk[a], &v)) p->resp->in_tokens = (uint32_t)v;
    if (b >= 0 && lla_json_int(js, &tk[b], &v)) p->resp->out_tokens = (uint32_t)v;
}

static void handle_choice(llm_parser_t *p, const char *js, const lla_jtok_t *tk, int ch, bool stream)
{
    int msg = lla_json_obj_get(js, tk, ch, stream ? "delta" : "message");
    if (msg >= 0 && tk[msg].type == LLA_JT_OBJECT) {
        int content = lla_json_obj_get(js, tk, msg, "content");
        if (content >= 0) llm_append_text(p, js, &tk[content]);
        int tc = lla_json_obj_get(js, tk, msg, "tool_calls");
        if (tc >= 0 && tk[tc].type == LLA_JT_ARRAY) {
            for (int i = 0; i < tk[tc].size && !p->err; i++) merge_call(p, js, tk, lla_json_arr_get(tk, tc, i), i, stream);
        }
    }
    int fr = lla_json_obj_get(js, tk, ch, "finish_reason");
    if (fr >= 0 && tk[fr].type == LLA_JT_STRING) {
        p->resp->stop = map_finish(js, &tk[fr], p->resp->stop_raw, sizeof(p->resp->stop_raw));
        p->finished = true;
        for (uint8_t i = 0; i < p->resp->n_calls; i++) p->resp->calls[i].complete = true;
    }
}

static void oa_handle(llm_parser_t *p, const char *js, size_t n, bool stream)
{
    int nt = lla_json_parse(js, n, p->toks, p->max_toks);
    if (nt < 0) {
        llm_set_err(p, nt == LLA_JSON_ERR_PART ? LLM_ERR_TRUNCATED : nt == LLA_JSON_ERR_NOMEM ? LLM_ERR_TOO_LARGE
                                                                                               : LLM_ERR_MALFORMED);
        return;
    }
    const lla_jtok_t *tk = p->toks;
    if (tk[0].type != LLA_JT_OBJECT) {
        llm_set_err(p, LLM_ERR_MALFORMED);
        return;
    }
    if (lla_json_obj_get(js, tk, 0, "error") >= 0) {
        llm_parse_error_body(p, js, n); /* re-tokenizes; fine, we are done with tk */
        return;
    }
    parse_usage(p, js, tk);
    int choices = lla_json_obj_get(js, tk, 0, "choices");
    if (choices < 0 || tk[choices].type != LLA_JT_ARRAY) {
        if (!stream) llm_set_err(p, LLM_ERR_PROTOCOL);
        return; /* stream: usage-only chunk */
    }
    if (tk[choices].size == 0) return;
    int ch = lla_json_arr_get(tk, choices, 0); /* we always request n=1 */
    handle_choice(p, js, tk, ch, stream);
}

static void oa_on_sse(llm_parser_t *p, const char *d, size_t n) { oa_handle(p, d, n, true); }

static void oa_on_body(llm_parser_t *p, const char *d, size_t n)
{
    oa_handle(p, d, n, false);
    if (!p->err && !p->resp->err_type[0] && !p->finished) {
        /* Complete JSON without finish_reason: some servers omit it for
         * plain answers. Accept text-only replies; tool calls need it. */
        if (p->resp->n_calls == 0) {
            p->resp->stop = LLM_STOP_END;
            p->finished = true;
        } else {
            llm_set_err(p, LLM_ERR_PROTOCOL);
        }
    }
}

const llm_provider_t llm_provider_openai = {
    .name = "openai",
    .kind = LLM_KIND_OPENAI,
    .caps = {.tools = true, .streaming = true, .parallel_tool_calls = true, .requires_key = false,
             .native_tool_ids = true, .max_context_tokens = 0},
    .chat_path = "/chat/completions",
    .models_path = "/models",
    .build = oa_build,
    .headers = oa_headers,
    .on_sse = oa_on_sse,
    .on_body = oa_on_body,
};

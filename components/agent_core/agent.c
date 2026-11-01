/*
 * agent.c — bounded agent loop with retry, idempotency ledger and budgets.
 *
 * Safety invariants (tested in tests/host/test_agent.c):
 *  I1  No tool executes unless the model response was complete and validated.
 *  I2  A tool whose previous execution in this run had an UNKNOWN outcome is
 *      never re-executed automatically (any arguments, if non-idempotent).
 *  I3  A NON_IDEMPOTENT tool is never executed twice with identical arguments
 *      in one run.
 *  I4  Retrying a model request never re-executes tools: tool results are
 *      already recorded in the context that is re-sent.
 *  I5  Every run terminates: turns, tool calls, retries and wall time are
 *      bounded, and repeated identical calls abort the run.
 */
#include "agent.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "agent";

const char *agent_state_str(agent_state_t s)
{
    static const char *const n[] = {"IDLE",          "CONNECTING", "REQUESTING_MODEL", "PARSING_RESPONSE",
                                    "VALIDATING_TOOL", "EXECUTING_TOOL", "VERIFYING",     "SUBMITTING_RESULT",
                                    "RECOVERING",    "COMPLETED",  "ERROR",            "CANCELED"};
    return (unsigned)s <= AG_CANCELED ? n[s] : "?";
}

void agent_budget_default(agent_budget_t *b)
{
    b->max_model_turns = 8;
    b->max_tool_calls = 16;
    b->max_identical_calls = 2;
    b->max_retries = 3;
    b->max_loop_strikes = 2;
    b->deadline_ms = 120000;
    b->backoff_base_ms = 500;
    b->backoff_max_ms = 8000;
    b->net_wait_ms = 15000;
}

void agent_init(agent_t *a, const agent_mem_t *mem, llm_transport_t *tr, tool_registry_t *reg)
{
    memset(a, 0, sizeof(*a));
    a->mem = *mem;
    a->tr = tr;
    a->reg = reg;
    agent_budget_default(&a->budget);
    llm_cfg_default(&a->cfg, LLM_KIND_OPENAI);
    a->prov = llm_provider_get(a->cfg.kind);
    llm_conv_init(&a->conv, mem->conv_buf, mem->conv_cap);
    snprintf(a->system_prompt, sizeof(a->system_prompt),
             "You control an ESP32-C5 device through tools. Only act on devices reported by the tools. "
             "Never guess sensor values; read them. Keep answers short.");
}

bool agent_set_provider(agent_t *a, const llm_cfg_t *cfg)
{
    const llm_provider_t *p = llm_provider_get(cfg->kind);
    if (!p) return false;
    a->cfg = *cfg;
    a->prov = p;
    return true;
}

void agent_set_system_prompt(agent_t *a, const char *s) { snprintf(a->system_prompt, sizeof(a->system_prompt), "%s", s); }
void agent_set_hooks(agent_t *a, const agent_hooks_t *h) { a->hooks = *h; }
void agent_cancel(agent_t *a) { a->cancel = true; }
void agent_clear_context(agent_t *a) { llm_conv_clear(&a->conv); }
const char *agent_last_text(const agent_t *a) { return a->resp.text; }

static void set_state(agent_t *a, agent_state_t s, const char *detail)
{
    a->state = s;
    if (a->hooks.on_state) a->hooks.on_state(a->hooks.u, s, detail);
}

uint32_t agent_call_hash(const char *name, const char *args, size_t n)
{
    uint32_t h = lla_fnv1a(LLA_FNV_INIT, name, strlen(name));
    h = lla_fnv1a(h, "\0", 1);
    bool in_str = false, esc = false;
    for (size_t i = 0; i < n; i++) {
        char c = args[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
        } else {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
            if (c == '"') in_str = true;
        }
        h = lla_fnv1a(h, &c, 1);
    }
    return h;
}

static agent_ledger_t *ledger_find(agent_t *a, uint32_t h)
{
    for (uint8_t i = 0; i < a->n_ledger; i++) {
        if (a->ledger[i].hash == h) return &a->ledger[i];
    }
    return NULL;
}

static bool tool_has_unknown(agent_t *a, int tool)
{
    for (uint8_t i = 0; i < a->n_ledger; i++) {
        if (a->ledger[i].tool == tool && a->ledger[i].status == TOOL_ST_UNKNOWN) return true;
    }
    return false;
}

static uint32_t remaining(uint64_t deadline)
{
    uint64_t now = lla_now_ms();
    return now >= deadline ? 0 : (uint32_t)(deadline - now);
}

/** Sleep in slices so cancellation stays responsive. */
static bool sleep_cancelable(agent_t *a, uint32_t ms)
{
    while (ms) {
        if (a->cancel) return false;
        uint32_t s = ms > 100 ? 100 : ms;
        lla_sleep_ms(s);
        ms -= s;
    }
    return !a->cancel;
}

typedef enum { MR_OK, MR_FATAL, MR_CANCELED } mr_t;

static bool http_retryable(int status)
{
    return status == 408 || status == 409 || status == 429 || status == 500 || status == 502 || status == 503 ||
           status == 504 || status == 529;
}

/** Build the request, evicting older runs if the body does not fit. */
static bool build_request(agent_t *a, lla_jw_t *w)
{
    llm_req_ctx_t ctx = {.system_prompt = a->system_prompt, .conv = &a->conv, .tools = a->reg, .src = a->src};
    for (;;) {
        lla_jw_init(w, a->mem.req_buf, a->mem.req_cap);
        if (a->prov->build(&a->cfg, &ctx, w)) return true;
        if (!llm_conv_evict_oldest(&a->conv, a->run_id)) return false;
    }
}

static mr_t model_request(agent_t *a, uint64_t deadline, agent_run_result_t *out)
{
    lla_jw_t w;
    if (!build_request(a, &w)) {
        snprintf(out->error, sizeof(out->error), "request exceeds %u-byte buffer", (unsigned)a->mem.req_cap);
        return MR_FATAL;
    }
    if (w.len > out->req_bytes_max) out->req_bytes_max = (uint32_t)w.len;

    char url[192];
    if (!llm_join_url(url, sizeof(url), a->cfg.base_url, a->prov->chat_path)) {
        snprintf(out->error, sizeof(out->error), "provider URL too long");
        return MR_FATAL;
    }

    for (uint8_t attempt = 0;; attempt++) {
        if (a->cancel) return MR_CANCELED;
        uint32_t rem = remaining(deadline);
        if (rem == 0) {
            snprintf(out->error, sizeof(out->error), "deadline exceeded");
            return MR_FATAL;
        }
        set_state(a, AG_REQUESTING_MODEL, a->prov->name);
        llm_http_req_t req = {.method = "POST", .url = url, .body = a->mem.req_buf, .body_len = w.len,
                              .timeout_ms = a->cfg.timeout_ms < rem ? a->cfg.timeout_ms : rem, .cancel = &a->cancel};
        req.n_headers = a->prov->headers(&a->cfg, req.headers, LLM_MAX_HEADERS, a->hdr_scratch, sizeof(a->hdr_scratch));
        llm_parser_init(&a->parser, a->prov, &a->cfg, &a->resp, a->mem.resp_buf, a->mem.resp_cap, a->mem.toks,
                        a->mem.max_toks);
        llm_http_sink_t sink;
        llm_parser_sink(&a->parser, &sink);

        uint64_t t0 = lla_now_ms();
        llm_tr_err_t tr = a->tr->perform(a->tr, &req, &sink);
        memset(a->hdr_scratch, 0, sizeof(a->hdr_scratch)); /* wipe secret header copy */
        out->model_ms += (uint32_t)(lla_now_ms() - t0);

        bool retry = false;
        uint32_t wait = 0;
        if (tr == LLM_TR_CANCELED || a->cancel) return MR_CANCELED;
        if (tr != LLM_TR_OK && tr != LLM_TR_ABORTED) {
            snprintf(out->error, sizeof(out->error), "transport: %s", llm_tr_str(tr));
            retry = tr == LLM_TR_CONNECT || tr == LLM_TR_TIMEOUT || tr == LLM_TR_IO || tr == LLM_TR_NO_NETWORK;
        } else {
            set_state(a, AG_PARSING_RESPONSE, NULL);
            int prc = llm_parser_finish(&a->parser);
            out->in_tokens += a->resp.in_tokens;
            out->out_tokens += a->resp.out_tokens;
            out->diag |= a->resp.diag;
            if (prc == LLM_OK) return MR_OK;
            if (prc == LLM_ERR_API) {
                snprintf(out->error, sizeof(out->error), "HTTP %d %s: %.70s", a->parser.http_status, a->resp.err_type,
                         a->resp.err_msg);
                /* in-stream errors (HTTP 200) e.g. overloaded_error are retryable */
                retry = a->parser.http_status == 200 || http_retryable(a->parser.http_status);
                wait = a->parser.retry_after_ms;
            } else {
                /* Malformed / truncated / protocol violation: nothing was
                 * executed (I1). Re-asking is side-effect free. */
                snprintf(out->error, sizeof(out->error), "model response %s", llm_perr_str(prc));
                retry = true;
            }
        }
        LLA_LOGW(TAG, "model request failed (attempt %u): %s", attempt + 1, out->error);
        if (!retry || attempt >= a->budget.max_retries) return MR_FATAL;

        uint32_t backoff = a->budget.backoff_base_ms << attempt;
        if (backoff > a->budget.backoff_max_ms) backoff = a->budget.backoff_max_ms;
        backoff += lla_rand32() % (backoff / 4 + 1);
        if (wait > backoff) backoff = wait;
        if (backoff >= remaining(deadline)) {
            snprintf(out->error + strlen(out->error), sizeof(out->error) - strlen(out->error), " (no time to retry)");
            return MR_FATAL;
        }
        out->retries++;
        set_state(a, AG_RECOVERING, out->error);
        if (!sleep_cancelable(a, backoff)) return MR_CANCELED;
        if (a->hooks.net_ready && !a->hooks.net_ready(a->hooks.u, remaining(deadline) < a->budget.net_wait_ms
                                                                    ? remaining(deadline)
                                                                    : a->budget.net_wait_ms)) {
            /* keep trying within the budget; the next attempt reports no_network */
        }
        out->error[0] = '\0';
    }
}

static void before_verify(void *u) { set_state((agent_t *)u, AG_VERIFYING, NULL); }

/** Validate + execute one call into a->results[i]. Returns false to abort the run. */
static bool run_call(agent_t *a, uint8_t i, agent_run_result_t *out)
{
    llm_tool_call_t *c = &a->resp.calls[i];
    tool_result_t *res = &a->results[i];
    set_state(a, AG_VALIDATING_TOOL, c->name);

    if (out->tool_calls >= a->budget.max_tool_calls) {
        tool_result_fail(res, TOOL_ST_REJECTED, "tool_call_budget_exhausted", "finish with the information you have");
        tool_result_end(res);
        return true;
    }
    if (c->invalid) {
        tool_result_fail(res, TOOL_ST_REJECTED, "invalid_tool_call", c->invalid);
        tool_result_end(res);
        return true;
    }
    int idx = tool_registry_find(a->reg, c->name, strlen(c->name));
    if (idx < 0) {
        tool_result_fail(res, TOOL_ST_REJECTED, "unknown_tool", "use only the tools provided");
        tool_result_end(res);
        return true;
    }
    const tool_def_t *def = a->reg->tools[idx];
    uint32_t h = agent_call_hash(c->name, c->args, c->args_len);
    agent_ledger_t *prior = ledger_find(a, h);

    if (def->effect != TOOL_EFFECT_NONE && (prior ? prior->status == TOOL_ST_UNKNOWN
                                                  : def->effect == TOOL_EFFECT_NON_IDEMPOTENT && tool_has_unknown(a, idx))) {
        tool_result_fail(res, TOOL_ST_REJECTED, "previous_outcome_unknown",
                         "not repeated automatically; ask the user to check the device");
        tool_result_end(res);
        return true;
    }
    if (prior && def->effect == TOOL_EFFECT_NON_IDEMPOTENT && prior->status == TOOL_ST_OK) {
        tool_result_fail(res, TOOL_ST_REJECTED, "duplicate_suppressed", "identical call already executed in this run");
        tool_result_end(res);
        return true;
    }
    if (prior && prior->count >= a->budget.max_identical_calls) {
        tool_result_fail(res, TOOL_ST_REJECTED, "repeated_call", "identical call repeated; use the earlier result");
        tool_result_end(res);
        if (++a->loop_strikes >= a->budget.max_loop_strikes) {
            snprintf(out->error, sizeof(out->error), "non-progressing loop: %s repeated", c->name);
            return false;
        }
        return true;
    }

    set_state(a, AG_EXECUTING_TOOL, c->name);
    uint64_t t0 = lla_now_ms();
    tool_registry_invoke(a->reg, idx, a->src, c->args, c->args_len, t0, res, before_verify, a);
    out->tool_ms += (uint32_t)(lla_now_ms() - t0);
    if (res->status != TOOL_ST_REJECTED) {
        out->tool_calls++;
        if (!prior && a->n_ledger < AGENT_LEDGER_MAX) {
            prior = &a->ledger[a->n_ledger++];
            prior->hash = h;
            prior->tool = (uint8_t)idx;
            prior->count = 0;
        }
        if (prior) {
            prior->status = (uint8_t)res->status;
            prior->count++;
        }
        if (res->status == TOOL_ST_UNKNOWN) out->unknown_outcomes++;
    }
    return true;
}

agent_wd_action_t agent_watchdog_check(const agent_t *a, uint64_t now, uint32_t grace_ms)
{
    uint64_t start = a->run_started_ms;
    if (!start) return AGENT_WD_OK;
    uint64_t limit = start + a->budget.deadline_ms + grace_ms;
    if (now < limit) return AGENT_WD_OK;
    return now < limit + grace_ms ? AGENT_WD_CANCEL : AGENT_WD_RESTART;
}

static agent_state_t finish(agent_t *a, agent_state_t st, agent_run_result_t *out, uint64_t t_start)
{
    a->run_started_ms = 0;
    out->final_state = st;
    out->elapsed_ms = (uint32_t)(lla_now_ms() - t_start);
    if (st != AG_COMPLETED) {
        /* Roll back the incomplete run so later runs see a consistent history. */
        llm_conv_drop_run(&a->conv, a->run_id);
        if (st == AG_CANCELED && !out->error[0]) snprintf(out->error, sizeof(out->error), "canceled");
    }
    a->last = *out;
    set_state(a, st, out->error[0] ? out->error : NULL);
    a->state = AG_IDLE;
    a->cancel = false;
    return st;
}

agent_state_t agent_run(agent_t *a, tool_source_t src, const char *request, agent_run_result_t *out_opt)
{
    agent_run_result_t local;
    agent_run_result_t *out = out_opt ? out_opt : &local;
    memset(out, 0, sizeof(*out));
    uint64_t t_start = lla_now_ms();
    uint64_t deadline = t_start + a->budget.deadline_ms;
    a->run_id++;
    a->run_started_ms = t_start ? t_start : 1;
    a->src = src;
    a->n_ledger = 0;
    a->loop_strikes = 0;
    llm_resp_reset(&a->resp);

    if (!a->prov || !a->tr) {
        snprintf(out->error, sizeof(out->error), "no provider configured");
        return finish(a, AG_ERROR, out, t_start);
    }
    set_state(a, AG_CONNECTING, NULL);
    if (a->hooks.net_ready && !a->hooks.net_ready(a->hooks.u, a->budget.net_wait_ms)) {
        snprintf(out->error, sizeof(out->error), "network unavailable");
        return finish(a, AG_ERROR, out, t_start);
    }
    if (a->cancel) return finish(a, AG_CANCELED, out, t_start);
    if (!llm_conv_add_user(&a->conv, a->run_id, request, strlen(request))) {
        snprintf(out->error, sizeof(out->error), "request exceeds context budget");
        return finish(a, AG_ERROR, out, t_start);
    }

    for (;;) {
        if (a->cancel) return finish(a, AG_CANCELED, out, t_start);
        if (out->model_turns >= a->budget.max_model_turns) {
            snprintf(out->error, sizeof(out->error), "model turn budget (%u) exhausted", a->budget.max_model_turns);
            return finish(a, AG_ERROR, out, t_start);
        }
        out->model_turns++;
        mr_t mr = model_request(a, deadline, out);
        if (mr == MR_CANCELED) return finish(a, AG_CANCELED, out, t_start);
        if (mr == MR_FATAL) return finish(a, AG_ERROR, out, t_start);

        llm_response_t *r = &a->resp;
        if (r->diag & LLM_DIAG_TOOLCALL_IN_TEXT) {
            LLA_LOGW(TAG, "model text resembles a tool call but no structured call was received; "
                          "NOT executed (check server tool-call parsing)");
        }
        if (r->stop == LLM_STOP_MAX_TOKENS && r->n_calls) {
            snprintf(out->error, sizeof(out->error), "model output hit max_tokens during a tool call; not executed");
            return finish(a, AG_ERROR, out, t_start);
        }
        if (r->stop == LLM_STOP_FILTER) {
            snprintf(out->error, sizeof(out->error), "model refused or output was filtered");
            return finish(a, AG_ERROR, out, t_start);
        }

        if (r->n_calls == 0) {
            if (!llm_conv_add_assistant(&a->conv, a->run_id, r->text, r->text_len, NULL, 0)) {
                snprintf(out->error, sizeof(out->error), "answer exceeds context budget");
                return finish(a, AG_ERROR, out, t_start);
            }
            set_state(a, AG_VERIFYING, NULL);
            if (out->unknown_outcomes) {
                snprintf(out->error, sizeof(out->error), "completed; %u action(s) have unknown outcome - check device",
                         out->unknown_outcomes);
            } else if (r->stop == LLM_STOP_MAX_TOKENS) {
                snprintf(out->error, sizeof(out->error), "answer truncated at max_tokens");
            }
            return finish(a, AG_COMPLETED, out, t_start);
        }

        /* Record the assistant turn exactly as received (ids preserved). */
        llm_call_view_t views[LLM_MAX_CALLS];
        for (uint8_t i = 0; i < r->n_calls; i++) {
            llm_tool_call_t *c = &r->calls[i];
            views[i].id = (lla_str_t){c->id, (uint16_t)strlen(c->id)};
            views[i].name = (lla_str_t){c->name, (uint16_t)strlen(c->name)};
            /* unusable args are replaced so the stored history stays valid JSON */
            views[i].args = c->invalid ? (lla_str_t){"{}", 2} : (lla_str_t){c->args, c->args_len};
        }
        if (!llm_conv_add_assistant(&a->conv, a->run_id, r->text, r->text_len, views, r->n_calls)) {
            snprintf(out->error, sizeof(out->error), "context budget exhausted");
            return finish(a, AG_ERROR, out, t_start);
        }

        bool abort_run = false;
        uint8_t done = 0;
        for (uint8_t i = 0; i < r->n_calls; i++) {
            if (a->cancel) break;
            if (!run_call(a, i, out)) {
                abort_run = true;
                done = i + 1;
                break;
            }
            done = i + 1;
            if (a->results[i].status == TOOL_ST_REJECTED) out->rejected_calls++;
            if (a->hooks.on_tool) a->hooks.on_tool(a->hooks.u, r->calls[i].name, r->calls[i].args, &a->results[i]);
        }
        if (a->cancel) return finish(a, AG_CANCELED, out, t_start);
        if (abort_run) {
            (void)done;
            return finish(a, AG_ERROR, out, t_start);
        }

        set_state(a, AG_SUBMITTING_RESULT, NULL);
        llm_result_view_t rv[LLM_MAX_CALLS];
        for (uint8_t i = 0; i < r->n_calls; i++) {
            rv[i].id = views[i].id;
            rv[i].name = views[i].name;
            rv[i].content = (lla_str_t){a->results[i].json, (uint16_t)strlen(a->results[i].json)};
            rv[i].is_error = a->results[i].status != TOOL_ST_OK;
        }
        if (!llm_conv_add_results(&a->conv, a->run_id, rv, r->n_calls)) {
            snprintf(out->error, sizeof(out->error), "context budget exhausted by tool results");
            return finish(a, AG_ERROR, out, t_start);
        }
    }
}

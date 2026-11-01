/* Agent state machine tests with a scripted mock transport and mock tools. */
#include "agent.h"
#include "test.h"

/* ------------------------------------------------------------ mock transport */

typedef struct {
    int status;
    const char *body;     /* inline body, or NULL */
    const char *fixture;  /* fixture file name, or NULL */
    llm_tr_err_t err;     /* transport error to return */
    bool err_after_body;  /* deliver body then fail (lost response) */
    uint32_t advance_ms;  /* simulated latency */
    const char *retry_after;
    bool cancel_agent;
} step_t;

#define MAX_STEPS 16
typedef struct {
    llm_transport_t base;
    step_t steps[MAX_STEPS];
    int n, i;
    char reqs[MAX_STEPS][12288];
    char urls[MAX_STEPS][160];
    char auth[MAX_STEPS][96];
    agent_t *agent;
} mock_tr_t;

static mock_tr_t s_mock;

static llm_tr_err_t mock_perform(llm_transport_t *t, const llm_http_req_t *req, const llm_http_sink_t *sink)
{
    mock_tr_t *m = (mock_tr_t *)t;
    if (m->i >= m->n) return LLM_TR_CONNECT;
    int k = m->i++;
    step_t *s = &m->steps[k];
    snprintf(m->reqs[k], sizeof(m->reqs[k]), "%.*s", (int)req->body_len, req->body);
    snprintf(m->urls[k], sizeof(m->urls[k]), "%s", req->url);
    m->auth[k][0] = 0;
    for (int h = 0; h < req->n_headers; h++) {
        if (!strcmp(req->headers[h].name, "Authorization") || !strcmp(req->headers[h].name, "x-api-key"))
            snprintf(m->auth[k], sizeof(m->auth[k]), "%s", req->headers[h].value);
    }
    test_clock_advance(s->advance_ms);
    if (s->cancel_agent) agent_cancel(m->agent);
    if (s->err && !s->err_after_body) return s->err;
    sink->on_status(sink->u, s->status ? s->status : 200);
    if (s->retry_after) sink->on_header(sink->u, "Retry-After", s->retry_after);
    size_t n = 0;
    char *owned = NULL;
    const char *b = s->body;
    if (s->fixture) b = owned = test_read_file(s->fixture, &n);
    else n = b ? strlen(b) : 0;
    /* deliver in odd-sized chunks to exercise incremental parsing */
    for (size_t off = 0; off < n; off += 37) {
        size_t c = n - off < 37 ? n - off : 37;
        if (sink->on_data(sink->u, b + off, c)) {
            free(owned);
            return LLM_TR_ABORTED;
        }
    }
    free(owned);
    return s->err_after_body ? s->err : LLM_TR_OK;
}

static void mock_reset(void)
{
    memset(&s_mock, 0, sizeof(s_mock));
    s_mock.base.perform = mock_perform;
}

static step_t *push(void) { return &s_mock.steps[s_mock.n++]; }

static char s_bodies[MAX_STEPS][640];
static int s_nb;

/* OpenAI non-stream body with a single tool call */
static const char *oa_call(const char *id, const char *name, const char *args_escaped)
{
    char *b = s_bodies[s_nb++ % MAX_STEPS];
    snprintf(b, 640,
             "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":"
             "\"%s\",\"type\":\"function\",\"function\":{\"name\":\"%s\",\"arguments\":\"%s\"}}]},\"finish_reason\":"
             "\"tool_calls\"}]}",
             id, name, args_escaped);
    return b;
}

static const char *oa_text(const char *text)
{
    char *b = s_bodies[s_nb++ % MAX_STEPS];
    snprintf(b, 640, "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"%s\"},\"finish_reason\":\"stop\"}]}",
             text);
    return b;
}

/* ------------------------------------------------------------ mock tools */

static double s_temp = 29.5;
static int s_fan;
static int s_exec_sensor, s_exec_fan, s_exec_pulse, s_exec_flaky;
static bool s_cancel_in_tool;
static agent_t s_agent;

static const char *const k_devs[] = {"room_temp", NULL};
static const tool_arg_def_t k_sensor_args[] = {{.name = "device", .type = TARG_ENUM, .required = true, .enum_vals = k_devs}};

static void exec_sensor(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)a; (void)dl;
    s_exec_sensor++;
    lla_jw_kv_num(&r->w, "celsius", s_temp);
}

static const tool_arg_def_t k_act_args[] = {
    {.name = "device", .type = TARG_STR, .required = true, .max_len = 24},
    {.name = "value", .type = TARG_INT, .required = true, .min = 0, .max = 1},
};

static void exec_fan(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)dl;
    s_exec_fan++;
    if (strcmp(tool_arg_str(d, a, "device", ""), "cooling_fan") != 0) {
        tool_result_fail(r, TOOL_ST_ERROR, "unknown_device", NULL);
        return;
    }
    s_fan = (int)tool_arg_int(d, a, "value", 0);
    if (s_cancel_in_tool) agent_cancel(&s_agent);
    lla_jw_kv_int(&r->w, "value", s_fan);
}

static bool verify_fan(const tool_def_t *d, const tool_args_t *a, tool_result_t *r)
{
    (void)r;
    return s_fan == tool_arg_int(d, a, "value", -1);
}

static void exec_pulse(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)a; (void)dl;
    s_exec_pulse++;
    lla_jw_kv_bool(&r->w, "pulsed", true);
}

static void exec_flaky(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)a; (void)dl;
    s_exec_flaky++;
    r->status = TOOL_ST_UNKNOWN;
    lla_jw_kv_str(&r->w, "error", "bus_timeout_after_write");
}

static const tool_def_t k_tools[] = {
    {.name = "read_sensor", .desc = "Read a sensor", .args = k_sensor_args, .n_args = 1, .perm = TOOL_PERM_READ,
     .effect = TOOL_EFFECT_NONE, .timeout_ms = 500, .exec = exec_sensor},
    {.name = "set_actuator", .desc = "Set actuator", .args = k_act_args, .n_args = 2, .perm = TOOL_PERM_ACTUATE,
     .effect = TOOL_EFFECT_IDEMPOTENT, .timeout_ms = 500, .exec = exec_fan, .verify = verify_fan},
    {.name = "pulse_relay", .desc = "Pulse relay", .perm = TOOL_PERM_ACTUATE, .effect = TOOL_EFFECT_NON_IDEMPOTENT,
     .timeout_ms = 500, .exec = exec_pulse},
    {.name = "flaky_write", .desc = "Write that may time out", .perm = TOOL_PERM_ACTUATE,
     .effect = TOOL_EFFECT_NON_IDEMPOTENT, .timeout_ms = 500, .exec = exec_flaky},
};

static tool_registry_t s_reg;
static char s_req[12288], s_respbuf[8192];
static lla_jtok_t s_toks[512];
static uint8_t s_conv[6144];
static int s_states[64], s_nstates;

static void on_state(void *u, agent_state_t st, const char *d)
{
    (void)u; (void)d;
    if (s_nstates < 64) s_states[s_nstates++] = st;
}

static bool saw_state(agent_state_t st)
{
    for (int i = 0; i < s_nstates; i++)
        if (s_states[i] == (int)st) return true;
    return false;
}

static void setup(llm_kind_t kind, uint16_t conv_cap)
{
    mock_reset();
    tool_registry_init(&s_reg);
    s_reg.policy.min_interval_ms = 0;
    for (size_t i = 0; i < sizeof(k_tools) / sizeof(k_tools[0]); i++) tool_registry_add(&s_reg, &k_tools[i]);
    agent_mem_t mem = {s_req, sizeof(s_req), s_respbuf, sizeof(s_respbuf), s_toks, 512, s_conv, conv_cap};
    agent_init(&s_agent, &mem, &s_mock.base, &s_reg);
    s_mock.agent = &s_agent;
    llm_cfg_t cfg;
    llm_cfg_default(&cfg, kind);
    cfg.stream = false;
    snprintf(cfg.api_key, sizeof(cfg.api_key), "sk-test-123");
    agent_set_provider(&s_agent, &cfg);
    agent_hooks_t h = {.on_state = on_state};
    agent_set_hooks(&s_agent, &h);
    s_fan = 0;
    s_temp = 29.5;
    s_exec_sensor = s_exec_fan = s_exec_pulse = s_exec_flaky = 0;
    s_cancel_in_tool = false;
    s_nstates = 0;
    test_clock_reset_slept();
}

/* ------------------------------------------------------------ tests */

static void test_multi_step_workflow(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    push()->body = oa_call("call_1", "read_sensor", "{\\\"device\\\":\\\"room_temp\\\"}");
    push()->body = oa_call("call_2", "set_actuator", "{\\\"device\\\":\\\"cooling_fan\\\",\\\"value\\\":1}");
    push()->body = oa_text("Temperature 29.5 C; fan switched on.");
    agent_run_result_t r;
    agent_state_t st = agent_run(&s_agent, TOOL_SRC_CONSOLE, "Check the temperature and switch on the fan if above 28", &r);
    CHECK_EQ_INT(st, AG_COMPLETED);
    CHECK_EQ_INT(s_fan, 1);
    CHECK_EQ_INT(r.model_turns, 3);
    CHECK_EQ_INT(r.tool_calls, 2);
    CHECK_STR(agent_last_text(&s_agent), "Temperature 29.5 C; fan switched on.");
    /* request 2 carries the sensor result with the original id */
    CHECK_CONTAINS(s_mock.reqs[1], "\"tool_call_id\":\"call_1\"");
    CHECK_CONTAINS(s_mock.reqs[1], "29.5");
    CHECK_CONTAINS(s_mock.reqs[2], "\"tool_call_id\":\"call_2\"");
    CHECK_CONTAINS(s_mock.reqs[2], "\\\"verified\\\":true");
    CHECK_CONTAINS(s_mock.reqs[0], "\"tools\":[");
    CHECK_STR(s_mock.urls[0], "http://192.168.1.100:8080/v1/chat/completions");
    CHECK_STR(s_mock.auth[0], "Bearer sk-test-123");
    CHECK(s_agent.hdr_scratch[0] == 0); /* secret copy wiped */
    CHECK(saw_state(AG_CONNECTING) && saw_state(AG_VALIDATING_TOOL) && saw_state(AG_EXECUTING_TOOL) &&
          saw_state(AG_VERIFYING) && saw_state(AG_SUBMITTING_RESULT) && saw_state(AG_COMPLETED));
    /* follow-up run sees the previous exchange */
    push()->body = oa_text("Fan is on.");
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "Is the fan on?", NULL), AG_COMPLETED);
    CHECK_CONTAINS(s_mock.reqs[3], "switch on the fan");
}

static void test_unknown_tool_and_bad_args(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    push()->body = oa_call("c1", "format_flash", "{}");
    push()->body = oa_call("c2", "set_actuator", "{\\\"device\\\":\\\"cooling_fan\\\",\\\"value\\\":7}");
    push()->body = oa_call("c3", "set_actuator", "{\\\"device\\\":\\\"cooling_fan\\\",\\\"value\\\":1,\\\"x\\\":1}");
    push()->body = oa_text("Could not do it.");
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "go", &r), AG_COMPLETED);
    CHECK_EQ_INT(s_exec_fan, 0);
    CHECK_EQ_INT(r.rejected_calls, 3);
    CHECK_CONTAINS(s_mock.reqs[1], "unknown_tool");
    CHECK_CONTAINS(s_mock.reqs[2], "out of range");
    CHECK_CONTAINS(s_mock.reqs[3], "unknown argument");
}

static void test_retry_429_then_ok(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    step_t *s = push();
    s->status = 429;
    s->fixture = "openai_error_429.json";
    s->retry_after = "3";
    push()->body = oa_text("ok");
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "hi", &r), AG_COMPLETED);
    CHECK_EQ_INT(r.retries, 1);
    CHECK(test_clock_slept() >= 3000);
    CHECK(saw_state(AG_RECOVERING));
}

static void test_http_500_exhausts(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    for (int i = 0; i < 5; i++) {
        step_t *s = push();
        s->status = 500;
        s->fixture = "openai_error_500.json";
    }
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "hi", &r), AG_ERROR);
    CHECK_EQ_INT(s_mock.i, 4); /* 1 + max_retries */
    CHECK_CONTAINS(r.error, "HTTP 500");
    CHECK_EQ_INT(llm_conv_count(&s_agent.conv), 0); /* failed run rolled back */
}

static void test_http_401_fatal(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    step_t *s = push();
    s->status = 401;
    s->body = "{\"error\":{\"message\":\"Incorrect API key provided\",\"type\":\"invalid_request_error\"}}";
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "hi", &r), AG_ERROR);
    CHECK_EQ_INT(s_mock.i, 1);
    CHECK_CONTAINS(r.error, "Incorrect API key");
}

static void test_transport_failures(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    push()->err = LLM_TR_CONNECT;
    push()->err = LLM_TR_TIMEOUT;
    push()->body = oa_text("ok");
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "hi", &r), AG_COMPLETED);
    CHECK_EQ_INT(r.retries, 2);
    /* TLS failures are not retried */
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    push()->err = LLM_TR_TLS;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "hi", &r), AG_ERROR);
    CHECK_EQ_INT(s_mock.i, 1);
}

static void test_truncated_never_executes(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    s_agent.cfg.stream = true;
    push()->fixture = "openai_stream_truncated.sse";
    step_t *s = push();
    s->fixture = "openai_stream_length.sse";
    push()->body = oa_text("Sorry.");
    agent_run_result_t r;
    /* truncated → retried; length-limited tool call → error, never executed */
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "fan on", &r), AG_ERROR);
    CHECK_EQ_INT(s_exec_fan, 0);
    CHECK_CONTAINS(r.error, "max_tokens");
}

static void test_malformed_recovers(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    push()->fixture = "openai_garbage.json";
    push()->fixture = "openai_reason_without_calls.json";
    push()->body = oa_text("Recovered.");
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "hi", &r), AG_COMPLETED);
    CHECK_EQ_INT(r.retries, 2);
    CHECK_STR(agent_last_text(&s_agent), "Recovered.");
}

static void test_loop_detection(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    for (int i = 0; i < 8; i++) push()->body = oa_call(i % 2 ? "a" : "b", "read_sensor", "{\\\"device\\\":\\\"room_temp\\\"}");
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "loop", &r), AG_ERROR);
    CHECK_CONTAINS(r.error, "non-progressing loop");
    CHECK_EQ_INT(s_exec_sensor, 2); /* max_identical_calls */
}

static void test_turn_budget(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    s_agent.budget.max_model_turns = 3;
    s_agent.budget.max_identical_calls = 10;
    char args[3][64];
    for (int i = 0; i < 6; i++) {
        snprintf(args[i % 3], 64, "{\\\"device\\\":\\\"cooling_fan\\\",\\\"value\\\":%d}", i % 2);
        push()->body = oa_call(i % 2 ? "x" : "y", "set_actuator", args[i % 3]);
    }
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "toggle forever", &r), AG_ERROR);
    CHECK_CONTAINS(r.error, "turn budget");
    CHECK_EQ_INT(r.model_turns, 3);
}

static void test_lost_response_no_replay(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    push()->body = oa_call("p1", "pulse_relay", "{}");
    step_t *lost = push(); /* response to the request carrying the result is lost */
    lost->body = oa_text("partial");
    lost->err = LLM_TR_IO;
    lost->err_after_body = true;
    push()->body = oa_call("p2", "pulse_relay", "{}"); /* model asks again */
    push()->body = oa_text("Relay pulsed once.");
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "pulse the relay once", &r), AG_COMPLETED);
    CHECK_EQ_INT(s_exec_pulse, 1);
    /* the retried request still carries the first result */
    CHECK_CONTAINS(s_mock.reqs[2], "\"tool_call_id\":\"p1\"");
    CHECK_CONTAINS(s_mock.reqs[3], "duplicate_suppressed");
}

static void test_unknown_outcome_not_retried(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    push()->body = oa_call("f1", "flaky_write", "{}");
    push()->body = oa_call("f2", "flaky_write", "{}");
    push()->body = oa_text("Outcome unknown; please check.");
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "write", &r), AG_COMPLETED);
    CHECK_EQ_INT(s_exec_flaky, 1);
    CHECK_EQ_INT(r.unknown_outcomes, 1);
    CHECK_CONTAINS(s_mock.reqs[2], "previous_outcome_unknown");
    CHECK_CONTAINS(r.error, "unknown outcome");
}

static void test_cancel(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    s_cancel_in_tool = true;
    push()->body = oa_call("c1", "set_actuator", "{\\\"device\\\":\\\"cooling_fan\\\",\\\"value\\\":1}");
    push()->body = oa_text("never");
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "fan", &r), AG_CANCELED);
    CHECK_EQ_INT(s_mock.i, 1);
    CHECK_EQ_INT(llm_conv_count(&s_agent.conv), 0);
    /* cancel during transport */
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    step_t *s = push();
    s->body = oa_text("x");
    s->cancel_agent = true;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "fan", &r), AG_CANCELED);
    /* next run works (flag cleared) */
    push()->body = oa_text("fine");
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "again", &r), AG_COMPLETED);
}

static void test_deadline(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    s_agent.budget.deadline_ms = 10000;
    step_t *s = push();
    s->err = LLM_TR_TIMEOUT;
    s->advance_ms = 9900;
    push()->body = oa_text("late");
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "slow", &r), AG_ERROR);
    CHECK(r.elapsed_ms <= 10000);
}

static void test_permission_by_source(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    push()->body = oa_call("c1", "set_actuator", "{\\\"device\\\":\\\"cooling_fan\\\",\\\"value\\\":1}");
    push()->body = oa_text("not allowed");
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_AUTONOMOUS, "event: hot", &r), AG_COMPLETED);
    CHECK_EQ_INT(s_exec_fan, 0);
    CHECK_CONTAINS(s_mock.reqs[1], "permission_denied");
    CHECK(strstr(s_mock.reqs[0], "set_actuator") == NULL); /* not advertised */
    CHECK_CONTAINS(s_mock.reqs[0], "read_sensor");
}

static void test_network_unavailable(void)
{
    setup(LLM_KIND_OPENAI, sizeof(s_conv));
    agent_hooks_t h = {.net_ready = NULL, .on_state = on_state};
    bool no_net(void *u, uint32_t w);
    h.net_ready = no_net;
    agent_set_hooks(&s_agent, &h);
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "hi", &r), AG_ERROR);
    CHECK_CONTAINS(r.error, "network unavailable");
    CHECK_EQ_INT(s_mock.i, 0);
}

bool no_net(void *u, uint32_t w)
{
    (void)u; (void)w;
    return false;
}

static void test_anthropic_workflow(void)
{
    setup(LLM_KIND_ANTHROPIC, sizeof(s_conv));
    push()->fixture = "anthropic_tool_use.json";
    push()->fixture = "anthropic_text.json";
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "temp?", &r), AG_COMPLETED);
    CHECK_EQ_INT(s_exec_sensor, 1);
    CHECK_STR(s_mock.urls[0], "https://api.anthropic.com/v1/messages");
    CHECK_STR(s_mock.auth[0], "sk-test-123");
    CHECK_CONTAINS(s_mock.reqs[0], "\"input_schema\"");
    CHECK_CONTAINS(s_mock.reqs[1], "\"type\":\"tool_use\",\"id\":\"toolu_01A\",\"name\":\"read_sensor\",\"input\":{\"device\":\"room_temp\"}");
    CHECK_CONTAINS(s_mock.reqs[1], "\"type\":\"tool_result\",\"tool_use_id\":\"toolu_01A\"");
    CHECK_CONTAINS(s_mock.reqs[1], "\"cache_control\"");
    CHECK_STR(agent_last_text(&s_agent), "Done: fan is on.");
    CHECK_EQ_INT(r.in_tokens, 512 + 700);
}

static void test_anthropic_stream_workflow(void)
{
    setup(LLM_KIND_ANTHROPIC, sizeof(s_conv));
    s_agent.cfg.stream = true;
    push()->fixture = "anthropic_stream_truncated.sse";
    push()->fixture = "anthropic_stream_tool.sse";
    push()->fixture = "anthropic_text.json"; /* server answered without SSE */
    agent_run_result_t r;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, "temp?", &r), AG_COMPLETED);
    CHECK_EQ_INT(s_exec_fan, 0);
    CHECK_EQ_INT(s_exec_sensor, 1);
    CHECK_EQ_INT(r.retries, 1);
    CHECK_CONTAINS(s_mock.reqs[0], "\"stream\":true");
    CHECK(strstr(s_mock.reqs[0], "stream_options") == NULL); /* OpenAI-only field */
}

static void test_context_eviction(void)
{
    setup(LLM_KIND_OPENAI, 600);
    agent_run_result_t r;
    for (int i = 0; i < 6; i++) {
        push()->body = oa_text("A fairly long answer that takes up context space in the arena buffer......");
        char q[64];
        snprintf(q, sizeof(q), "question number %d with padding padding padding", i);
        CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, q, &r), AG_COMPLETED);
    }
    CHECK(s_agent.conv.evictions > 0);
    CHECK(s_agent.conv.len <= 600);
    CHECK(strstr(s_mock.reqs[5], "question number 0") == NULL);
    CHECK_CONTAINS(s_mock.reqs[5], "question number 5");
    /* a single request larger than the whole budget is refused */
    char big[700];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    CHECK_EQ_INT(agent_run(&s_agent, TOOL_SRC_CONSOLE, big, &r), AG_ERROR);
}

static void test_call_hash_normalizes_whitespace(void)
{
    const char *a = "{\"device\": \"x\", \"value\": 1}";
    const char *b = "{\"device\":\"x\",\"value\":1}";
    const char *c = "{\"device\":\"x \",\"value\":1}";
    CHECK(agent_call_hash("t", a, strlen(a)) == agent_call_hash("t", b, strlen(b)));
    CHECK(agent_call_hash("t", b, strlen(b)) != agent_call_hash("t", c, strlen(c)));
    CHECK(agent_call_hash("t", b, strlen(b)) != agent_call_hash("u", b, strlen(b)));
}

void suite_agent(void)
{
    RUN(test_multi_step_workflow);
    RUN(test_unknown_tool_and_bad_args);
    RUN(test_retry_429_then_ok);
    RUN(test_http_500_exhausts);
    RUN(test_http_401_fatal);
    RUN(test_transport_failures);
    RUN(test_truncated_never_executes);
    RUN(test_malformed_recovers);
    RUN(test_loop_detection);
    RUN(test_turn_budget);
    RUN(test_lost_response_no_replay);
    RUN(test_unknown_outcome_not_retried);
    RUN(test_cancel);
    RUN(test_deadline);
    RUN(test_permission_by_source);
    RUN(test_network_unavailable);
    RUN(test_anthropic_workflow);
    RUN(test_anthropic_stream_workflow);
    RUN(test_context_eviction);
    RUN(test_call_hash_normalizes_whitespace);
}

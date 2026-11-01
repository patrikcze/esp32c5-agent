/* Protocol fixture tests for the OpenAI-compatible and Anthropic parsers. */
#include "llm_provider.h"
#include "test.h"

static char s_buf[8192];
static lla_jtok_t s_toks[512];
static llm_response_t s_resp;

/* Feed a fixture in chunks of @p chunk bytes (0 = all at once). */
static int run_fixture(llm_kind_t kind, bool stream, const char *file, int status, size_t chunk, llm_cfg_t *cfg_opt)
{
    llm_cfg_t cfg;
    llm_cfg_default(&cfg, kind);
    cfg.stream = stream;
    if (cfg_opt) {
        cfg = *cfg_opt;
        cfg.stream = stream;
    }
    static llm_cfg_t keep;
    keep = cfg;
    llm_parser_t p;
    llm_parser_init(&p, llm_provider_get(kind), &keep, &s_resp, s_buf, sizeof(s_buf), s_toks, 512);
    llm_parser_status(&p, status);
    size_t n;
    char *data = test_read_file(file, &n);
    if (chunk == 0) chunk = n;
    for (size_t off = 0; off < n; off += chunk) {
        size_t k = n - off < chunk ? n - off : chunk;
        if (llm_parser_feed(&p, data + off, k) != 0) break;
    }
    free(data);
    return llm_parser_finish(&p);
}

static void test_oa_tool_call(void)
{
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, false, "openai_tool_call.json", 200, 0, NULL), LLM_OK);
    CHECK_EQ_INT(s_resp.stop, LLM_STOP_TOOL_USE);
    CHECK_EQ_INT(s_resp.n_calls, 1);
    CHECK_STR(s_resp.calls[0].id, "call_abc123");
    CHECK_STR(s_resp.calls[0].name, "read_sensor");
    CHECK_STR(s_resp.calls[0].args, "{\"device\":\"room_temp\"}");
    CHECK_EQ_INT(s_resp.in_tokens, 321);
    CHECK(s_resp.calls[0].invalid == NULL);
}

static void test_oa_multi(void)
{
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, false, "openai_multi_tool.json", 200, 0, NULL), LLM_OK);
    CHECK_EQ_INT(s_resp.n_calls, 2);
    CHECK_STR(s_resp.calls[1].id, "call_2");
    CHECK_STR(s_resp.calls[1].name, "get_free_heap");
    CHECK_STR(s_resp.text, "Checking both.");
}

static void test_oa_text(void)
{
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, false, "openai_text.json", 200, 0, NULL), LLM_OK);
    CHECK_EQ_INT(s_resp.stop, LLM_STOP_END);
    CHECK_EQ_INT(s_resp.n_calls, 0);
    CHECK_CONTAINS(s_resp.text, "29.5 \xc2\xb0" "C");
}

static void test_oa_invalid(void)
{
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, false, "openai_invalid.json", 200, 0, NULL), LLM_ERR_TRUNCATED);
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, false, "openai_garbage.json", 200, 0, NULL), LLM_ERR_MALFORMED);
}

static void test_oa_quirks(void)
{
    /* llama.cpp-style finish_reason "stop" with complete tool calls */
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, false, "openai_finish_stop_with_calls.json", 200, 0, NULL), LLM_OK);
    CHECK_EQ_INT(s_resp.stop, LLM_STOP_TOOL_USE);
    CHECK(s_resp.diag & LLM_DIAG_FINISH_MISMATCH);

    /* missing id: rejected unless compat flag set */
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, false, "openai_missing_id.json", 200, 0, NULL), LLM_ERR_PROTOCOL);
    llm_cfg_t cfg;
    llm_cfg_default(&cfg, LLM_KIND_OPENAI);
    cfg.allow_missing_ids = true;
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, false, "openai_missing_id.json", 200, 0, &cfg), LLM_OK);
    CHECK(s_resp.calls[0].synth_id);
    CHECK(s_resp.diag & LLM_DIAG_SYNTH_ID);

    /* finish_reason tool_calls without any calls: backend dropped them */
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, false, "openai_reason_without_calls.json", 200, 0, NULL), LLM_ERR_PROTOCOL);
}

static void test_oa_tool_in_text_not_executed(void)
{
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, false, "openai_tool_in_text.json", 200, 0, NULL), LLM_OK);
    CHECK_EQ_INT(s_resp.n_calls, 0); /* never invented from prose */
    CHECK(s_resp.diag & LLM_DIAG_TOOLCALL_IN_TEXT);
}

static void test_oa_bad_args(void)
{
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, false, "openai_bad_args.json", 200, 0, NULL), LLM_OK);
    CHECK_EQ_INT(s_resp.n_calls, 1);
    CHECK(s_resp.calls[0].invalid != NULL);
}

static void test_oa_http_errors(void)
{
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, false, "openai_error_429.json", 429, 0, NULL), LLM_ERR_API);
    CHECK_CONTAINS(s_resp.err_msg, "Rate limit");
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, true, "openai_error_500.json", 500, 0, NULL), LLM_ERR_API);
    CHECK_STR(s_resp.err_type, "server_error");
}

static void test_oa_stream(void)
{
    size_t chunks[] = {0, 1, 3, 17, 64};
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, true, "openai_stream_tool.sse", 200, chunks[i], NULL), LLM_OK);
        CHECK_EQ_INT(s_resp.n_calls, 2);
        CHECK_STR(s_resp.calls[0].id, "call_s1");
        CHECK_STR(s_resp.calls[0].args, "{\"device\": \"room_temp\"}");
        CHECK_STR(s_resp.calls[1].name, "get_uptime");
        CHECK_EQ_INT(s_resp.stop, LLM_STOP_TOOL_USE);
    }
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, true, "openai_stream_text.sse", 200, 5, NULL), LLM_OK);
    CHECK_STR(s_resp.text, "Hello \xc3\xbc");
    CHECK_EQ_INT(s_resp.out_tokens, 3);
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, true, "openai_stream_crlf_nodone.sse", 200, 2, NULL), LLM_OK);
    CHECK_STR(s_resp.text, "ok");
    CHECK(s_resp.diag & LLM_DIAG_NO_DONE);
}

static void test_oa_stream_truncation(void)
{
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, true, "openai_stream_truncated.sse", 200, 7, NULL), LLM_ERR_TRUNCATED);
    /* finish_reason length: parse OK but stop=MAX_TOKENS → agent must not execute */
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, true, "openai_stream_length.sse", 200, 0, NULL), LLM_OK);
    CHECK_EQ_INT(s_resp.stop, LLM_STOP_MAX_TOKENS);
}

static void test_oa_stream_ignored(void)
{
    /* server ignored stream:true and returned plain JSON */
    CHECK_EQ_INT(run_fixture(LLM_KIND_OPENAI, true, "openai_tool_call.json", 200, 9, NULL), LLM_OK);
    CHECK_EQ_INT(s_resp.n_calls, 1);
}

static void test_too_large(void)
{
    llm_cfg_t cfg;
    llm_cfg_default(&cfg, LLM_KIND_OPENAI);
    cfg.stream = false;
    llm_parser_t p;
    char small[64];
    llm_parser_init(&p, &llm_provider_openai, &cfg, &s_resp, small, sizeof(small), s_toks, 512);
    llm_parser_status(&p, 200);
    size_t n;
    char *d = test_read_file("openai_tool_call.json", &n);
    CHECK(llm_parser_feed(&p, d, n) == LLM_ERR_TOO_LARGE);
    CHECK_EQ_INT(llm_parser_finish(&p), LLM_ERR_TOO_LARGE);
    free(d);
}

static void test_an_tool_use(void)
{
    CHECK_EQ_INT(run_fixture(LLM_KIND_ANTHROPIC, false, "anthropic_tool_use.json", 200, 0, NULL), LLM_OK);
    CHECK_EQ_INT(s_resp.stop, LLM_STOP_TOOL_USE);
    CHECK_EQ_INT(s_resp.n_calls, 1);
    CHECK_STR(s_resp.calls[0].id, "toolu_01A");
    CHECK_STR(s_resp.calls[0].args, "{\"device\":\"room_temp\"}");
    CHECK_STR(s_resp.text, "Let me check.");
    CHECK_EQ_INT(run_fixture(LLM_KIND_ANTHROPIC, false, "anthropic_multi.json", 200, 0, NULL), LLM_OK);
    CHECK_EQ_INT(s_resp.n_calls, 2);
    CHECK_STR(s_resp.calls[1].id, "toolu_2");
    CHECK_EQ_INT(run_fixture(LLM_KIND_ANTHROPIC, false, "anthropic_text.json", 200, 0, NULL), LLM_OK);
    CHECK_EQ_INT(s_resp.stop, LLM_STOP_END);
}

static void test_an_stream(void)
{
    size_t chunks[] = {0, 1, 11, 100};
    for (size_t i = 0; i < 4; i++) {
        CHECK_EQ_INT(run_fixture(LLM_KIND_ANTHROPIC, true, "anthropic_stream_tool.sse", 200, chunks[i], NULL), LLM_OK);
        CHECK_EQ_INT(s_resp.n_calls, 1);
        CHECK_STR(s_resp.calls[0].id, "toolu_S1");
        CHECK_STR(s_resp.calls[0].args, "{\"device\": \"room_temp\"}");
        CHECK_STR(s_resp.text, "Checking the sensor.");
        CHECK_EQ_INT(s_resp.out_tokens, 89);
        CHECK_EQ_INT(s_resp.in_tokens, 472);
    }
}

static void test_an_failures(void)
{
    CHECK_EQ_INT(run_fixture(LLM_KIND_ANTHROPIC, true, "anthropic_stream_truncated.sse", 200, 13, NULL), LLM_ERR_TRUNCATED);
    CHECK_EQ_INT(run_fixture(LLM_KIND_ANTHROPIC, true, "anthropic_stream_unclosed_block.sse", 200, 0, NULL), LLM_ERR_TRUNCATED);
    CHECK_EQ_INT(run_fixture(LLM_KIND_ANTHROPIC, true, "anthropic_stream_error.sse", 200, 0, NULL), LLM_ERR_API);
    CHECK_STR(s_resp.err_type, "overloaded_error");
    CHECK_EQ_INT(run_fixture(LLM_KIND_ANTHROPIC, false, "anthropic_error_429.json", 429, 0, NULL), LLM_ERR_API);
    CHECK_STR(s_resp.err_type, "rate_limit_error");
}

static void count_cb(void *u, const char *id)
{
    (void)id;
    (*(int *)u)++;
}

static void test_models_list(void)
{
    const char *js = "{\"object\":\"list\",\"data\":[{\"id\":\"qwen2.5\"},{\"id\":\"llama3.2\"}]}";
    int count = 0;
    CHECK_EQ_INT(llm_parse_models(js, strlen(js), s_toks, 512, count_cb, &count), 2);
    CHECK_EQ_INT(count, 2);
    /* llama-server: {"models":[...],"data":[{"id":"<long gguf path>"}]} */
    const char *ls = "{\"models\":[{\"name\":\"x\"}],\"object\":\"list\",\"data\":[{\"id\":\"/Users/someone/.lmstudio/models/"
                     "lmstudio-community/gemma-4-E4B-it-GGUF/gemma-4-E4B-it-Q4_K_M.gguf\"}]}";
    count = 0;
    CHECK_EQ_INT(llm_parse_models(ls, strlen(ls), s_toks, 512, count_cb, &count), 1);
}

static void test_oa_request_stream_usage(void)
{
    char body[4096];
    uint8_t cb[64];
    llm_conv_t conv;
    llm_conv_init(&conv, cb, sizeof(cb));
    llm_conv_add_user(&conv, 1, "hi", 2);
    llm_cfg_t c;
    llm_cfg_default(&c, LLM_KIND_OPENAI);
    llm_req_ctx_t ctx = {.system_prompt = "s", .conv = &conv};
    lla_jw_t w;
    lla_jw_init(&w, body, sizeof(body));
    CHECK(llm_provider_openai.build(&c, &ctx, &w));
    CHECK_CONTAINS(body, "\"stream_options\":{\"include_usage\":true}");
    c.stream = false;
    lla_jw_init(&w, body, sizeof(body));
    CHECK(llm_provider_openai.build(&c, &ctx, &w));
    CHECK(strstr(body, "stream_options") == NULL);
}

static void test_url_join(void)
{
    char u[64];
    CHECK(llm_join_url(u, sizeof(u), "http://h:8080/v1/", "/chat/completions"));
    CHECK_STR(u, "http://h:8080/v1/chat/completions");
    CHECK(!llm_join_url(u, 10, "http://h:8080/v1/", "/chat/completions"));
}

void suite_openai(void)
{
    RUN(test_oa_tool_call);
    RUN(test_oa_multi);
    RUN(test_oa_text);
    RUN(test_oa_invalid);
    RUN(test_oa_quirks);
    RUN(test_oa_tool_in_text_not_executed);
    RUN(test_oa_bad_args);
    RUN(test_oa_http_errors);
    RUN(test_oa_stream);
    RUN(test_oa_stream_truncation);
    RUN(test_oa_stream_ignored);
    RUN(test_too_large);
    RUN(test_models_list);
    RUN(test_url_join);
    RUN(test_oa_request_stream_usage);
}

void suite_anthropic(void)
{
    RUN(test_an_tool_use);
    RUN(test_an_stream);
    RUN(test_an_failures);
}

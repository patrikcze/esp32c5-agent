/*
 * agent_service_esp.c — agent task, job queue, provider/policy persistence.
 *
 * Memory: all agent buffers are allocated once here. When PSRAM is present
 * the large buffers move there and are enlarged; otherwise conservative
 * internal-RAM sizes from Kconfig are used.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_service.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "agentsvc";

#define JOB_TEXT 320
typedef struct {
    tool_source_t src;
    char text[JOB_TEXT];
} job_t;

static agent_t s_agent;
static QueueHandle_t s_jobs;
static SemaphoreHandle_t s_submit_mtx;
static TaskHandle_t s_task;
static agent_service_env_t s_env;
static volatile bool s_busy;
static volatile bool s_reload;
static size_t s_req_sz, s_resp_sz, s_conv_sz;
static bool s_in_psram;

/* ------------------------------------------------------------ provider config */

static const char *pfx(llm_kind_t k) { return k == LLM_KIND_ANTHROPIC ? "an" : "oa"; }

static void key(char *out, size_t cap, llm_kind_t k, const char *name) { snprintf(out, cap, "%s_%s", pfx(k), name); }

int provider_load(llm_kind_t kind, llm_cfg_t *c)
{
    llm_cfg_default(c, kind);
    char k[16], v[32];
    key(k, sizeof(k), kind, "url");
    char url[sizeof(c->base_url)];
    if (cfg_get_str(CFG_NS, k, url, sizeof(url)) == CFG_OK) strlcpy(c->base_url, url, sizeof(c->base_url));
    key(k, sizeof(k), kind, "model");
    char model[sizeof(c->model)];
    if (cfg_get_str(CFG_NS, k, model, sizeof(model)) == CFG_OK) strlcpy(c->model, model, sizeof(c->model));
    key(k, sizeof(k), kind, "maxtok");
    if (cfg_get_str(CFG_NS, k, v, sizeof(v)) == CFG_OK) c->max_tokens = (uint16_t)atoi(v);
    key(k, sizeof(k), kind, "stream");
    if (cfg_get_str(CFG_NS, k, v, sizeof(v)) == CFG_OK) c->stream = v[0] == '1';
    key(k, sizeof(k), kind, "compat");
    if (cfg_get_str(CFG_NS, k, v, sizeof(v)) == CFG_OK) c->allow_missing_ids = v[0] == '1';
    key(k, sizeof(k), kind, "timeout");
    if (cfg_get_str(CFG_NS, k, v, sizeof(v)) == CFG_OK) c->timeout_ms = (uint32_t)atoi(v);
    key(k, sizeof(k), kind, "key");
    cfg_get_str(CFG_NS_SECRET, k, c->api_key, sizeof(c->api_key));
    return 0;
}

int provider_save(const llm_cfg_t *c)
{
    char k[16], v[16];
    int rc = 0;
    key(k, sizeof(k), c->kind, "url");
    rc |= cfg_set_str(CFG_NS, k, c->base_url) < 0;
    key(k, sizeof(k), c->kind, "model");
    rc |= cfg_set_str(CFG_NS, k, c->model) < 0;
    key(k, sizeof(k), c->kind, "maxtok");
    snprintf(v, sizeof(v), "%u", c->max_tokens);
    rc |= cfg_set_str(CFG_NS, k, v) < 0;
    key(k, sizeof(k), c->kind, "stream");
    rc |= cfg_set_str(CFG_NS, k, c->stream ? "1" : "0") < 0;
    key(k, sizeof(k), c->kind, "compat");
    rc |= cfg_set_str(CFG_NS, k, c->allow_missing_ids ? "1" : "0") < 0;
    key(k, sizeof(k), c->kind, "timeout");
    snprintf(v, sizeof(v), "%u", (unsigned)c->timeout_ms);
    rc |= cfg_set_str(CFG_NS, k, v) < 0;
    key(k, sizeof(k), c->kind, "key");
    rc |= cfg_set_str(CFG_NS_SECRET, k, c->api_key) < 0;
    s_reload = true;
    return rc ? -1 : 0;
}

llm_kind_t provider_active_kind(void)
{
    char v[16];
    if (cfg_get_str(CFG_NS, "prov", v, sizeof(v)) == CFG_OK && strcmp(v, "anthropic") == 0) return LLM_KIND_ANTHROPIC;
    return LLM_KIND_OPENAI;
}

int provider_set_active(llm_kind_t kind)
{
    s_reload = true;
    return cfg_set_str(CFG_NS, "prov", llm_provider_get(kind)->name) < 0 ? -1 : 0;
}

void agent_service_reload_provider(void) { s_reload = true; }

static void apply_provider(void)
{
    llm_cfg_t c;
    provider_load(provider_active_kind(), &c);
    agent_set_provider(&s_agent, &c);
    memset(&c, 0, sizeof(c)); /* wipe the stack copy of the key */
    char sys[AGENT_SYSTEM_MAX];
    if (cfg_get_str(CFG_NS, "sysprompt", sys, sizeof(sys)) == CFG_OK) agent_set_system_prompt(&s_agent, sys);
    char bud[48];
    if (cfg_get_str(CFG_NS, "budget", bud, sizeof(bud)) == CFG_OK) {
        unsigned turns, calls, dl;
        if (sscanf(bud, "%u,%u,%u", &turns, &calls, &dl) == 3 && turns && turns <= 20 && calls && calls <= 40 &&
            dl >= 5 && dl <= 600) {
            s_agent.budget.max_model_turns = (uint8_t)turns;
            s_agent.budget.max_tool_calls = (uint8_t)calls;
            s_agent.budget.deadline_ms = dl * 1000;
        }
    }
    s_reload = false;
}

/* ------------------------------------------------------------ policy */

int policy_load(tool_registry_t *reg)
{
    tool_policy_t p;
    size_t len = sizeof(p);
    if (cfg_get(CFG_NS, "policy", &p, &len) == CFG_OK && len == sizeof(p)) {
        for (int i = 0; i < TOOL_SRC_COUNT; i++) {
            if (p.ceiling[i] > TOOL_PERM_DANGEROUS) return -1;
        }
        reg->policy = p;
        return 0;
    }
    return -1;
}

int policy_save(const tool_registry_t *reg)
{
    return cfg_set(CFG_NS, "policy", &reg->policy, sizeof(reg->policy)) < 0 ? -1 : 0;
}

/* ------------------------------------------------------------ hooks */

static bool hook_net(void *u, uint32_t wait_ms)
{
    (void)u;
    return s_env.net_ready ? s_env.net_ready(wait_ms) : true;
}

static void hook_state(void *u, agent_state_t st, const char *detail)
{
    (void)u;
    if (st == AG_EXECUTING_TOOL || st == AG_RECOVERING || st == AG_ERROR || st == AG_CANCELED ||
        st == AG_REQUESTING_MODEL || st == AG_VERIFYING) {
        printf("  [agent] %s%s%s\n", agent_state_str(st), detail ? " " : "", detail ? detail : "");
    }
}

static void hook_tool(void *u, const char *name, const char *args, const tool_result_t *res)
{
    (void)u;
    printf("  [tool] %s %.120s -> %s %.200s\n", name, args, tool_status_str(res->status), res->json);
}

/* ------------------------------------------------------------ task */

static void agent_task(void *arg)
{
    (void)arg;
    static job_t job;
    for (;;) {
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) continue;
        if (s_reload) apply_provider();
        s_busy = true;
        agent_run_result_t r;
        printf("\n[agent] %s request: %.80s%s\n", tool_source_str(job.src), job.text, strlen(job.text) > 80 ? "..." : "");
        agent_state_t st = agent_run(&s_agent, job.src, job.text, &r);
        if (st == AG_COMPLETED) {
            printf("\n%s\n", agent_last_text(&s_agent));
        }
        printf("[agent] %s in %u ms: %u model turns (%u ms), %u tool calls (%u ms), %u retries, tokens %u/%u%s%s\n",
               agent_state_str(st), (unsigned)r.elapsed_ms, r.model_turns, (unsigned)r.model_ms, r.tool_calls,
               (unsigned)r.tool_ms, r.retries, (unsigned)r.in_tokens, (unsigned)r.out_tokens, r.error[0] ? " — " : "",
               r.error);
        if (r.diag & LLM_DIAG_TOOLCALL_IN_TEXT) {
            printf("[agent] WARNING: the model wrote a tool call as plain text; it was NOT executed. The server's\n"
                   "        tool-call parser likely did not recognise it (llama.cpp: use --jinja and a tool-capable\n"
                   "        chat template).\n");
        }
        if (r.diag & LLM_DIAG_FINISH_MISMATCH) printf("[agent] note: server sent finish_reason=stop with tool calls\n");
        s_busy = false;
    }
}

static void *alloc_buf(size_t n)
{
    void *p = NULL;
    if (s_in_psram) p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

int agent_service_start(tool_registry_t *reg, const agent_service_env_t *env)
{
    s_env = *env;
    s_in_psram = esp_psram_is_initialized();
    s_req_sz = s_in_psram ? 32768 : CONFIG_LLA_AGENT_REQ_BUF;
    s_resp_sz = s_in_psram ? 16384 : CONFIG_LLA_AGENT_RESP_BUF;
    s_conv_sz = s_in_psram ? 16384 : CONFIG_LLA_AGENT_CONV_BUF;
    uint16_t ntoks = s_in_psram ? 1024 : 512;
    agent_mem_t m = {
        .req_buf = alloc_buf(s_req_sz),
        .req_cap = s_req_sz,
        .resp_buf = alloc_buf(s_resp_sz),
        .resp_cap = s_resp_sz,
        .toks = alloc_buf(sizeof(lla_jtok_t) * ntoks),
        .max_toks = ntoks,
        .conv_buf = alloc_buf(s_conv_sz),
        .conv_cap = (uint16_t)s_conv_sz,
    };
    if (!m.req_buf || !m.resp_buf || !m.toks || !m.conv_buf) {
        ESP_LOGE(TAG, "agent buffer allocation failed");
        return -1;
    }
    agent_init(&s_agent, &m, NULL, reg);
    extern llm_transport_t *net_transport(void);
    s_agent.tr = net_transport();
    agent_hooks_t h = {.net_ready = hook_net, .on_state = hook_state, .on_tool = hook_tool};
    agent_set_hooks(&s_agent, &h);
    apply_provider();
    s_submit_mtx = xSemaphoreCreateMutex();
    s_jobs = xQueueCreate(2, sizeof(job_t));
    if (!s_jobs || !s_submit_mtx) return -1;
    /* TLS handshakes (mbedTLS) need a deep stack; measured high-water mark is
     * reported by /memory. */
    if (xTaskCreate(agent_task, "agent", CONFIG_LLA_AGENT_STACK, NULL, 4, &s_task) != pdPASS) return -1;
    ESP_LOGI(TAG, "agent buffers: req %u, resp %u, ctx %u, %u tokens (%s)", (unsigned)s_req_sz, (unsigned)s_resp_sz,
             (unsigned)s_conv_sz, ntoks, s_in_psram ? "PSRAM" : "internal RAM");
    return 0;
}

bool agent_service_submit(tool_source_t src, const char *text)
{
    static job_t j; /* serialised by s_submit_mtx; callers are console/event/BLE tasks */
    /* Event triggers can fire during boot (a rule tripping on the first
     * sample) before the agent service exists: refuse instead of crashing. */
    if (!s_jobs || !s_submit_mtx) {
        ESP_LOGW(TAG, "agent not running; request from %s dropped", tool_source_str(src));
        return false;
    }
    xSemaphoreTake(s_submit_mtx, portMAX_DELAY);
    j.src = src;
    strlcpy(j.text, text, sizeof(j.text));
    bool ok = xQueueSend(s_jobs, &j, 0) == pdTRUE;
    xSemaphoreGive(s_submit_mtx);
    if (!ok) ESP_LOGW(TAG, "agent queue full; request from %s dropped", tool_source_str(src));
    return ok;
}

void agent_service_cancel(void) { agent_cancel(&s_agent); }
bool agent_service_busy(void) { return s_busy; }
agent_t *agent_service_agent(void) { return &s_agent; }

void agent_service_mem_report(void)
{
    printf("  agent buffers  : req %u B, resp %u B, context %u B (%u used, %u records, %u evictions), %s\n",
           (unsigned)s_req_sz, (unsigned)s_resp_sz, (unsigned)s_conv_sz, s_agent.conv.len,
           llm_conv_count(&s_agent.conv), s_agent.conv.evictions, s_in_psram ? "PSRAM" : "internal");
    printf("  agent_t        : %u B static\n", (unsigned)sizeof(agent_t));
    printf("  agent stack HWM: %u B free of %u\n", (unsigned)uxTaskGetStackHighWaterMark(s_task),
           (unsigned)CONFIG_LLA_AGENT_STACK);
    printf("  last run       : %u ms, %u turns, %u tools, largest request %u B\n", (unsigned)s_agent.last.elapsed_ms,
           s_agent.last.model_turns, s_agent.last.tool_calls, (unsigned)s_agent.last.req_bytes_max);
}

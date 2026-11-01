/*
 * console_esp.c — interactive serial console (esp_console REPL on UART0).
 *
 * Commands start with '/'. Anything typed after /ask is sent to the agent.
 * Secrets typed here are stored in the secret NVS namespace and are never
 * echoed back (only "<set, N chars>").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_service.h"
#include "config.h"
#include "console.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "event_engine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "periph.h"
#include "sdkconfig.h"
#if CONFIG_LLA_ENABLE_BLE
#include "ble_mgr.h"
#endif

static tool_registry_t *s_reg;
static console_env_t s_env;

static void join(int argc, char **argv, int from, char *out, size_t cap)
{
    out[0] = '\0';
    size_t n = 0;
    for (int i = from; i < argc; i++) {
        int w = snprintf(out + n, cap - n, "%s%s", i > from ? " " : "", argv[i]);
        if (w < 0 || (size_t)w >= cap - n) break;
        n += (size_t)w;
    }
}

static bool is(const char *a, const char *b) { return a && strcmp(a, b) == 0; }

/* Large command buffers are allocated on demand (PSRAM when present) instead
 * of permanently occupying internal .bss. */
static void *scratch(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

/* ------------------------------------------------------------ /help */

static int c_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("llmagent console — commands:\n"
           "  /ask <text>                       send a request to the agent\n"
           "  /agent [status|cancel|clear|system <prompt>|budget <turns> <calls> <secs>]\n"
           "  /status                           overall status\n"
           "  /wifi [set <ssid> <pass>|scan|drop]\n"
           "  /provider [show|use openai|anthropic|url <u>|model <m>|key <k>|stream on|off|\n"
           "             maxtok <n>|compat on|off|ca|ca clear]\n"
           "  /models                           list models from the active provider\n"
           "  /tools                            registered tools and permissions\n"
           "  /policy [show|<source> <read|actuate|config|dangerous>|auto-allow <tool>|deny <tool>|save]\n"
           "  /devices [list|read <dev>|set <dev> <value>|profile [show|load|reset]]\n"
           "  /rules [list|approve|reject]\n"
#if CONFIG_LLA_ENABLE_BLE
           "  /ble [status|scan [secs] [prefix]|list|allow <addr> [rw]|deny <addr>|connect <n>|\n"
           "        disconnect|services|read <uuid>|sub <uuid>]\n"
#endif
           "  /memory                           heap, stacks, context and flash-write stats\n"
           "  /config                           export non-secret configuration\n"
           "  /ota <https-url>                  firmware update\n"
#if CONFIG_LLA_BENCHMARK
           "  /bench [json|llm <n>]             benchmarks\n"
#endif
           "  /reset [reboot|factory|context]\n");
    return 0;
}

/* ------------------------------------------------------------ /status */

static int c_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    net_status_t ns;
    net_get_status(&ns);
    llm_cfg_t c;
    provider_load(provider_active_kind(), &c);
    char red[32];
    cfg_redact(c.api_key, red, sizeof(red));
    printf("uptime      : %llu s\n", (unsigned long long)(esp_timer_get_time() / 1000000));
    printf("wifi        : %s ssid='%s' ip=%s rssi=%d ch=%u time=%s reconnects=%u last_reconnect=%ums\n",
           net_state_str(ns.state), ns.ssid, ns.ip[0] ? ns.ip : "-", ns.rssi, ns.channel,
           ns.time_synced ? "synced" : "unsynced", (unsigned)ns.disconnects, (unsigned)ns.last_reconnect_ms);
    printf("provider    : %s %s model=%s stream=%s key=%s\n", llm_provider_get(c.kind)->name, c.base_url, c.model,
           c.stream ? "on" : "off", red);
    memset(&c, 0, sizeof(c));
    printf("agent       : %s\n", agent_service_busy() ? "busy" : "idle");
    printf("heap        : free %u, min %u, largest %u (internal)\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    printf("devices     : %d registered\n", periph_count());
    static char buf[768];
    lla_jw_t w;
    lla_jw_init(&w, buf, sizeof(buf));
    event_engine_describe(&w);
    printf("automation  : %s\n", buf);
#if CONFIG_LLA_ENABLE_BLE
    ble_mgr_print_status();
#endif
    return 0;
}

/* ------------------------------------------------------------ /wifi */

static int c_wifi(int argc, char **argv)
{
    if (argc >= 3 && is(argv[1], "set")) {
        const char *pass = argc >= 4 ? argv[3] : "";
        int rc = net_set_credentials(argv[2], pass);
        if (rc == 0) printf("credentials stored (password hidden), connecting...\n");
        else printf("failed (%d): ssid 1-32 chars, password empty or 8-64\n", rc);
        return 0;
    }
    if (argc >= 2 && is(argv[1], "scan")) {
        printf("%d networks\n", net_scan_print());
        return 0;
    }
    if (argc >= 2 && is(argv[1], "drop")) {
        net_force_disconnect();
        printf("disconnect requested (automatic reconnect with backoff follows)\n");
        return 0;
    }
    net_status_t ns;
    net_get_status(&ns);
    printf("%s ssid='%s' ip=%s rssi=%d http_requests=%u reused=%u last_connect=%ums max_connect=%ums tls_fail=%u\n",
           net_state_str(ns.state), ns.ssid, ns.ip, ns.rssi, (unsigned)ns.http_requests, (unsigned)ns.http_conn_reused,
           (unsigned)ns.last_connect_ms, (unsigned)ns.max_connect_ms, (unsigned)ns.tls_failures);
    return 0;
}

/* ------------------------------------------------------------ /provider */

static int read_pem(char *out, size_t cap)
{
    printf("paste PEM certificate, end with the -----END CERTIFICATE----- line:\n");
    size_t n = 0;
    char line[128];
    while (fgets(line, sizeof(line), stdin)) {
        size_t l = strlen(line);
        if (n + l + 1 >= cap) return -1;
        memcpy(out + n, line, l);
        n += l;
        out[n] = '\0';
        if (strstr(line, "-----END CERTIFICATE-----")) return (int)n;
    }
    return -1;
}

static int c_provider(int argc, char **argv)
{
    llm_kind_t kind = provider_active_kind();
    llm_cfg_t c;
    provider_load(kind, &c);
    int rc = 0;
    bool save = true;
    if (argc >= 3 && is(argv[1], "use")) {
        const llm_provider_t *p = llm_provider_by_name(argv[2]);
        if (!p) {
            printf("unknown provider (openai|anthropic)\n");
            return 1;
        }
        rc = provider_set_active(p->kind);
        save = false;
        printf("active provider: %s\n", p->name);
    } else if (argc >= 3 && is(argv[1], "url")) {
        if (strncmp(argv[2], "http://", 7) && strncmp(argv[2], "https://", 8)) {
            printf("url must start with http:// or https://\n");
            return 1;
        }
        strlcpy(c.base_url, argv[2], sizeof(c.base_url));
    } else if (argc >= 3 && is(argv[1], "model")) {
        strlcpy(c.model, argv[2], sizeof(c.model));
    } else if (argc >= 3 && is(argv[1], "key")) {
        strlcpy(c.api_key, argv[2], sizeof(c.api_key));
        memset(argv[2], 0, strlen(argv[2])); /* wipe the line buffer copy */
    } else if (argc >= 2 && is(argv[1], "nokey")) {
        c.api_key[0] = '\0';
    } else if (argc >= 3 && is(argv[1], "stream")) {
        c.stream = is(argv[2], "on");
    } else if (argc >= 3 && is(argv[1], "compat")) {
        c.allow_missing_ids = is(argv[2], "on");
    } else if (argc >= 3 && is(argv[1], "maxtok")) {
        int v = atoi(argv[2]);
        if (v < 16 || v > 4096) {
            printf("maxtok 16..4096\n");
            return 1;
        }
        c.max_tokens = (uint16_t)v;
    } else if (argc >= 2 && is(argv[1], "ca")) {
        save = false;
        if (argc >= 3 && is(argv[2], "clear")) {
            cfg_erase(CFG_NS, "ca_pem");
            net_set_ca_pem(NULL);
            printf("custom CA removed; using the ESP x509 bundle\n");
        } else {
            char *pem = scratch(3072);
            if (!pem || read_pem(pem, 3072) < 0 || net_set_ca_pem(pem) != 0) {
                printf("invalid or too large PEM\n");
                free(pem);
                return 1;
            }
            rc = cfg_set_str(CFG_NS, "ca_pem", pem) < 0;
            free(pem);
            printf("custom CA %s\n", rc ? "loaded but NOT persisted" : "stored");
        }
    } else {
        save = false;
        for (int k = 0; k < LLM_KIND_COUNT; k++) {
            llm_cfg_t x;
            provider_load((llm_kind_t)k, &x);
            char red[32];
            cfg_redact(x.api_key, red, sizeof(red));
            printf("%c %-9s url=%s model=%s stream=%s maxtok=%u compat=%s key=%s\n", k == (int)kind ? '*' : ' ',
                   llm_provider_get((llm_kind_t)k)->name, x.base_url, x.model, x.stream ? "on" : "off", x.max_tokens,
                   x.allow_missing_ids ? "on" : "off", red);
            memset(&x, 0, sizeof(x));
        }
        printf("  custom CA: %s\n", net_has_custom_ca() ? "yes" : "no (ESP x509 bundle)");
    }
    if (save) {
        rc = provider_save(&c);
        printf(rc ? "save failed\n" : "saved\n");
    }
    memset(&c, 0, sizeof(c));
    return rc;
}

/* ------------------------------------------------------------ /models */

static void model_cb(void *u, const char *id)
{
    (void)u;
    printf("  %s\n", id);
}

typedef struct {
    char *buf;
    size_t cap, len;
    int status;
} model_sink_t;

static void ms_status(void *u, int s) { ((model_sink_t *)u)->status = s; }
static void ms_header(void *u, const char *k, const char *v) { (void)u; (void)k; (void)v; }
static int ms_data(void *u, const char *d, size_t n)
{
    model_sink_t *m = u;
    if (m->len + n >= m->cap) return 1;
    memcpy(m->buf + m->len, d, n);
    m->len += n;
    return 0;
}

static int c_models(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (agent_service_busy()) {
        printf("agent busy; try again\n");
        return 1;
    }
    llm_cfg_t c;
    provider_load(provider_active_kind(), &c);
    const llm_provider_t *p = llm_provider_get(c.kind);
    char url[192], hdr[224];
    char *body = scratch(6144);
    lla_jtok_t *toks = scratch(sizeof(lla_jtok_t) * 400);
    if (!body || !toks || !llm_join_url(url, sizeof(url), c.base_url, p->models_path)) {
        free(body);
        free(toks);
        return 1;
    }
    llm_http_req_t req = {.method = "GET", .url = url, .timeout_ms = 15000};
    c.stream = false;
    req.n_headers = p->headers(&c, req.headers, LLM_MAX_HEADERS, hdr, sizeof(hdr));
    model_sink_t ms = {.buf = body, .cap = 6144};
    llm_http_sink_t sink = {ms_status, ms_header, ms_data, &ms};
    llm_tr_err_t e = net_transport()->perform(net_transport(), &req, &sink);
    memset(hdr, 0, sizeof(hdr));
    memset(&c, 0, sizeof(c));
    int rc = 1;
    if (e != LLM_TR_OK && e != LLM_TR_ABORTED) {
        printf("request failed: %s\n", llm_tr_str(e));
    } else if (ms.status != 200) {
        body[ms.len < 6143 ? ms.len : 6143] = '\0';
        printf("HTTP %d: %.200s\n", ms.status, body);
    } else {
        int n = llm_parse_models(body, ms.len, toks, 400, model_cb, NULL);
        printf(n >= 0 ? "%d models%s\n" : "could not parse model list (%d)%s\n", n,
               e == LLM_TR_ABORTED ? " (list truncated)" : "");
        rc = n < 0;
    }
    free(body);
    free(toks);
    return rc;
}

/* ------------------------------------------------------------ /tools /policy */

static const char *effect_str(tool_effect_t e)
{
    return e == TOOL_EFFECT_NONE ? "read-only" : e == TOOL_EFFECT_IDEMPOTENT ? "idempotent" : "non-idempotent";
}

static int c_tools(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("%-28s %-9s %-14s %s\n", "tool", "perm", "effect", "allowed for: console autonomous ble remote");
    for (int i = 0; i < s_reg->count; i++) {
        const tool_def_t *t = s_reg->tools[i];
        printf("%-28s %-9s %-14s ", t->name, tool_perm_str(t->perm), effect_str(t->effect));
        for (int s = 0; s < TOOL_SRC_COUNT; s++) printf("%s ", tool_registry_permitted(s_reg, i, (tool_source_t)s) ? "Y" : "-");
        printf("\n");
    }
    return 0;
}

static int perm_from(const char *s)
{
    for (int p = 0; p <= TOOL_PERM_DANGEROUS; p++)
        if (is(s, tool_perm_str((tool_perm_t)p))) return p;
    return -1;
}

static int c_policy(int argc, char **argv)
{
    tool_policy_t *p = &s_reg->policy;
    if (argc >= 3) {
        int tool = tool_registry_find(s_reg, argv[2], strlen(argv[2]));
        if (is(argv[1], "auto-allow") || is(argv[1], "deny") || is(argv[1], "undeny")) {
            if (tool < 0) {
                printf("unknown tool\n");
                return 1;
            }
            if (is(argv[1], "auto-allow")) p->allow_autonomous_mask |= 1ULL << tool;
            else if (is(argv[1], "deny")) p->deny_mask |= 1ULL << tool;
            else p->deny_mask &= ~(1ULL << tool);
        } else {
            int perm = perm_from(argv[2]);
            int src = -1;
            for (int s = 0; s < TOOL_SRC_COUNT; s++)
                if (is(argv[1], tool_source_str((tool_source_t)s))) src = s;
            if (perm < 0 || src < 0) {
                printf("usage: /policy <console|autonomous|ble|remote> <read|actuate|config|dangerous>\n");
                return 1;
            }
            if (perm == TOOL_PERM_DANGEROUS) printf("WARNING: dangerous tools enabled for %s\n", argv[1]);
            p->ceiling[src] = (tool_perm_t)perm;
        }
        printf("updated (not persisted; /policy save)\n");
        return 0;
    }
    if (argc == 2 && is(argv[1], "save")) {
        printf(policy_save(s_reg) == 0 ? "policy saved\n" : "save failed\n");
        return 0;
    }
    for (int s = 0; s < TOOL_SRC_COUNT; s++)
        printf("  %-10s ceiling=%s\n", tool_source_str((tool_source_t)s), tool_perm_str(p->ceiling[s]));
    printf("  min interval between side effects of one tool: %u ms\n", p->min_interval_ms);
    for (int i = 0; i < s_reg->count; i++) {
        if (p->deny_mask & (1ULL << i)) printf("  denied: %s\n", s_reg->tools[i]->name);
        if (p->allow_autonomous_mask & (1ULL << i)) printf("  autonomous-allowed: %s\n", s_reg->tools[i]->name);
    }
    return 0;
}

/* ------------------------------------------------------------ /devices */

static int c_devices(int argc, char **argv)
{
    if (argc >= 3 && is(argv[1], "read")) {
        int i = periph_find(argv[2]);
        float v, v2;
        int rc = periph_read(i, &v, &v2);
        if (rc == PER_OK) printf("%s = %.3f %s\n", argv[2], (double)v, periph_get(i)->cfg.unit);
        else printf("error: %s\n", per_err_str(rc));
        return rc != PER_OK;
    }
    if (argc >= 4 && is(argv[1], "set")) {
        int rc = periph_set(periph_find(argv[2]), (float)atof(argv[3]), PERIPH_BY_LOCAL);
        printf("%s\n", per_err_str(rc));
        return rc != PER_OK;
    }
    if (argc >= 2 && is(argv[1], "profile")) {
        if (argc >= 3 && is(argv[2], "reset")) {
            cfg_erase(CFG_NS, "profile");
            printf("stored profile removed; default profile applies after /reset reboot\n");
            return 0;
        }
        char *js = scratch(4096);
        if (!js) return 1;
        int rc = 0;
        if (argc >= 3 && is(argv[2], "load")) {
            printf("paste the profile JSON on ONE line:\n");
            size_t n = 0;
            do { /* skip the line terminator left over from the command itself */
                if (!fgets(js, 4096, stdin)) {
                    free(js);
                    return 1;
                }
                n = strcspn(js, "\r\n");
            } while (n == 0);
            js[n] = '\0';
            char err[128];
            rc = periph_load_profile(js, n, err, sizeof(err));
            if (rc == PER_OK) rc = event_engine_load_profile(js, n, err, sizeof(err));
            if (rc != 0) printf("rejected: %s\n", err);
            else printf(cfg_set_str(CFG_NS, "profile", js) < 0 ? "applied, NOT persisted\n" : "applied and stored\n");
        } else if (cfg_get_str(CFG_NS, "profile", js, 4096) == CFG_OK) {
            printf("%s\n", js);
        } else {
            printf("(using built-in default profile)\n%s\n", s_env.default_profile);
        }
        free(js);
        return rc != 0;
    }
    printf("%-14s %-9s %-4s %-8s %-10s %s\n", "name", "type", "pin", "agent_rw", "value", "notes");
    for (int i = 0; i < periph_count(); i++) {
        const periph_dev_t *d = periph_get(i);
        printf("%-14s %-9s %-4d %-8s %-10.3f %s%s%s\n", d->cfg.name, dev_type_str(d->cfg.type), d->cfg.pin,
               d->cfg.llm_write ? "yes" : "no", (double)d->value, d->locked_by >= 0 ? "LOCKED " : "",
               d->watched ? "watched " : "", d->faults ? "FAULTS" : "");
    }
    return 0;
}

/* ------------------------------------------------------------ /rules */

static int c_rules(int argc, char **argv)
{
    if (argc >= 2 && is(argv[1], "approve")) {
        printf(event_engine_approve_rule() == 0 ? "rule installed and stored\n" : "nothing pending / no space\n");
        return 0;
    }
    if (argc >= 2 && is(argv[1], "reject")) {
        event_engine_reject_rule();
        printf("proposal discarded\n");
        return 0;
    }
    static char buf[1024];
    lla_jw_t w;
    lla_jw_init(&w, buf, sizeof(buf));
    event_engine_describe(&w);
    printf("%s\n", buf);
    rule_t r;
    if (event_engine_pending_rule(&r)) {
        lla_jw_init(&w, buf, sizeof(buf));
        rules_describe_one(&r, &w);
        printf("PENDING proposal (device indexes): %s\n", buf);
    }
    return 0;
}

/* ------------------------------------------------------------ /memory */

static int c_memory(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("  internal heap  : free %u, min-ever %u, largest block %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    printf("  PSRAM heap     : free %u, largest %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    agent_service_mem_report();
    cfg_stats_t cs;
    cfg_stats(&cs);
    printf("  flash writes   : %u done, %u skipped (unchanged), %u rate-limited, %u this hour (max %d)\n",
           (unsigned)cs.writes, (unsigned)cs.skipped_unchanged, (unsigned)cs.rate_limited, (unsigned)cs.window_writes,
           CFG_MAX_WRITES_PER_HOUR);
    uint32_t dr, dd, sm;
    event_engine_stats(&dr, &dd, &sm);
    printf("  events         : %u samples, %u deduplicated, %u dropped (queue full)\n", (unsigned)sm, (unsigned)dd,
           (unsigned)dr);
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    static TaskStatus_t ts[20];
    UBaseType_t n = uxTaskGetSystemState(ts, 20, NULL);
    printf("  task stacks (free bytes at high-water mark):\n");
    for (UBaseType_t i = 0; i < n; i++) printf("    %-16s %5u  prio %u\n", ts[i].pcTaskName, (unsigned)ts[i].usStackHighWaterMark, (unsigned)ts[i].uxCurrentPriority);
#endif
    return 0;
}

/* ------------------------------------------------------------ /agent /ask */

static int c_ask(int argc, char **argv)
{
    static char text[320];
    join(argc, argv, 1, text, sizeof(text));
    if (!text[0]) {
        printf("usage: /ask <request>\n");
        return 1;
    }
    if (!agent_service_submit(TOOL_SRC_CONSOLE, text)) printf("agent queue full\n");
    return 0;
}

static int c_agent(int argc, char **argv)
{
    agent_t *a = agent_service_agent();
    if (argc >= 2 && is(argv[1], "ask")) return c_ask(argc - 1, argv + 1);
    if (argc >= 2 && is(argv[1], "cancel")) {
        agent_service_cancel();
        printf("cancel requested\n");
        return 0;
    }
    if (argc >= 2 && is(argv[1], "clear")) {
        if (agent_service_busy()) {
            printf("busy; cancel first\n");
            return 1;
        }
        agent_clear_context(a);
        printf("context cleared\n");
        return 0;
    }
    if (argc >= 3 && is(argv[1], "system")) {
        static char p[AGENT_SYSTEM_MAX];
        join(argc, argv, 2, p, sizeof(p));
        printf(cfg_set_str(CFG_NS, "sysprompt", p) < 0 ? "save failed\n" : "system prompt stored\n");
        agent_service_reload_provider();
        return 0;
    }
    if (argc >= 5 && is(argv[1], "budget")) {
        char b[48];
        snprintf(b, sizeof(b), "%s,%s,%s", argv[2], argv[3], argv[4]);
        printf(cfg_set_str(CFG_NS, "budget", b) < 0 ? "save failed\n" : "budget stored (turns,calls,seconds)\n");
        agent_service_reload_provider();
        return 0;
    }
    printf("state=%s busy=%s budget: turns %u, tool calls %u, deadline %u s, retries %u\n", agent_state_str(a->state),
           agent_service_busy() ? "yes" : "no", a->budget.max_model_turns, a->budget.max_tool_calls,
           (unsigned)(a->budget.deadline_ms / 1000), a->budget.max_retries);
    printf("system prompt: %s\n", a->system_prompt);
    printf("last run: %s %s\n", agent_state_str(a->last.final_state), a->last.error);
    return 0;
}

/* ------------------------------------------------------------ /config /ota /reset */

static int c_config(int argc, char **argv)
{
    (void)argc; (void)argv;
    static const char *const keys[] = {"wifi_ssid", "prov", "oa_url", "oa_model", "oa_maxtok", "oa_stream",
                                       "an_url",    "an_model", "an_maxtok", "an_stream", "budget", "sysprompt", NULL};
    char v[160];
    for (int i = 0; keys[i]; i++) {
        if (cfg_get_str(CFG_NS, keys[i], v, sizeof(v)) == CFG_OK) printf("  %-10s = %s\n", keys[i], v);
    }
    static const char *const secrets[] = {"wifi_pass", "oa_key", "an_key", NULL};
    for (int i = 0; secrets[i]; i++) {
        char s[200], red[32];
        cfg_get_str(CFG_NS_SECRET, secrets[i], s, sizeof(s));
        cfg_redact(s, red, sizeof(red));
        memset(s, 0, sizeof(s));
        printf("  %-10s = %s\n", secrets[i], red);
    }
    return 0;
}

static int c_ota(int argc, char **argv)
{
    if (argc < 2) {
        const esp_partition_t *p = esp_ota_get_running_partition();
        printf("running from %s\n", p ? p->label : "?");
        return 0;
    }
    if (agent_service_busy()) {
        printf("agent busy\n");
        return 1;
    }
    periph_all_safe();
    return net_ota(argv[1]) == 0 ? 0 : 1;
}

static int c_reset(int argc, char **argv)
{
    if (argc >= 2 && is(argv[1], "factory")) {
        periph_all_safe();
        printf(cfg_factory_reset() == 0 ? "configuration erased; rebooting\n" : "erase failed\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    if (argc >= 2 && is(argv[1], "context")) {
        agent_clear_context(agent_service_agent());
        printf("context cleared\n");
        return 0;
    }
    if (argc >= 2 && is(argv[1], "reboot")) {
        esp_restart(); /* shutdown handler drives outputs safe */
    }
    printf("usage: /reset reboot|factory|context\n");
    return 1;
}

/* ------------------------------------------------------------ /bench */

#if CONFIG_LLA_BENCHMARK
static int c_bench(int argc, char **argv)
{
    if (argc >= 2 && is(argv[1], "json")) {
        static const char doc[] =
            "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":"
            "\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"read_sensor\",\"arguments\":\"{\\\"device\\\":"
            "\\\"room_temp\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10}}";
        static lla_jtok_t t[64];
        int64_t t0 = esp_timer_get_time();
        int n = 0;
        for (int i = 0; i < 1000; i++) n = lla_json_parse(doc, sizeof(doc) - 1, t, 64);
        int64_t dt = esp_timer_get_time() - t0;
        printf("json: %u-byte tool-call response, %d tokens, %.1f us/parse, %.2f MB/s, token array %u B\n",
               (unsigned)sizeof(doc) - 1, n, dt / 1000.0, (sizeof(doc) - 1) * 1000.0 / dt, (unsigned)(n * sizeof(lla_jtok_t)));
        return 0;
    }
    if (argc >= 2 && is(argv[1], "llm")) {
        int n = argc >= 3 ? atoi(argv[2]) : 3;
        if (n < 1 || n > 10) n = 3;
        for (int i = 0; i < n; i++) {
            if (!agent_service_submit(TOOL_SRC_CONSOLE, "Reply with the single word OK. Do not call tools.")) break;
            while (!agent_service_busy()) vTaskDelay(pdMS_TO_TICKS(20));
            while (agent_service_busy()) vTaskDelay(pdMS_TO_TICKS(50));
        }
        net_status_t ns;
        net_get_status(&ns);
        printf("bench llm: %d requests; connection reused %u times; last new connection %u ms (max %u ms)\n", n,
               (unsigned)ns.http_conn_reused, (unsigned)ns.last_connect_ms, (unsigned)ns.max_connect_ms);
        return 0;
    }
    printf("usage: /bench json | /bench llm <n>\n");
    return 1;
}
#endif

/* ------------------------------------------------------------ registration */

#if CONFIG_LLA_ENABLE_BLE
int console_ble_cmd(int argc, char **argv);
#endif

int console_start(tool_registry_t *reg, const console_env_t *env)
{
    s_reg = reg;
    s_env = *env;
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "llmagent> ";
    rc.max_cmdline_length = 512;
    rc.task_stack_size = 6144;
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    if (esp_console_new_repl_uart(&uc, &rc, &repl) != ESP_OK) return -1;
    const esp_console_cmd_t cmds[] = {
        {.command = "/help", .help = "list commands", .func = c_help},
        {.command = "/status", .help = "status", .func = c_status},
        {.command = "/wifi", .help = "wifi", .func = c_wifi},
        {.command = "/provider", .help = "LLM provider", .func = c_provider},
        {.command = "/models", .help = "list models", .func = c_models},
        {.command = "/tools", .help = "tools", .func = c_tools},
        {.command = "/policy", .help = "tool policy", .func = c_policy},
        {.command = "/devices", .help = "devices", .func = c_devices},
        {.command = "/rules", .help = "automation rules", .func = c_rules},
        {.command = "/memory", .help = "memory", .func = c_memory},
        {.command = "/agent", .help = "agent", .func = c_agent},
        {.command = "/ask", .help = "ask the agent", .func = c_ask},
        {.command = "/config", .help = "config export", .func = c_config},
        {.command = "/ota", .help = "OTA update", .func = c_ota},
        {.command = "/reset", .help = "reset", .func = c_reset},
#if CONFIG_LLA_BENCHMARK
        {.command = "/bench", .help = "benchmarks", .func = c_bench},
#endif
#if CONFIG_LLA_ENABLE_BLE
        {.command = "/ble", .help = "bluetooth LE", .func = console_ble_cmd},
#endif
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    printf("\nType /help for commands.\n");
    return esp_console_start_repl(repl) == ESP_OK ? 0 : -1;
}

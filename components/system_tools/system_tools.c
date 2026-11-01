/*
 * system_tools.c — read-only system introspection tools plus rule
 * proposal (which only queues a proposal for local approval).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "event_engine.h"
#include "net.h"
#include "periph.h"
#include "system_tools.h"

static void x_system_info(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)a; (void)dl;
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_physical_size(NULL, &flash);
    const esp_app_desc_t *app = esp_app_get_description();
    lla_jw_kv_str(&r->w, "chip", "ESP32-C5");
    lla_jw_kv_int(&r->w, "revision", ci.revision);
    lla_jw_kv_int(&r->w, "cores", ci.cores);
    lla_jw_kv_int(&r->w, "flash_bytes", flash);
    lla_jw_kv_int(&r->w, "psram_bytes", esp_psram_is_initialized() ? (int64_t)esp_psram_get_size() : 0);
    lla_jw_kv_str(&r->w, "idf", esp_get_idf_version());
    lla_jw_kv_str(&r->w, "app_version", app->version);
    lla_jw_kv_str(&r->w, "board", periph_board() ? periph_board()->name : "?");
    lla_jw_kv_str(&r->w, "bluetooth", "LE only");
}

static void x_uptime(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)a; (void)dl;
    lla_jw_kv_int(&r->w, "uptime_s", esp_timer_get_time() / 1000000);
    lla_jw_kv_int(&r->w, "reset_reason", esp_reset_reason());
}

static void x_time(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)a; (void)dl;
    net_status_t s;
    net_get_status(&s);
    time_t now = time(NULL);
    if (!s.time_synced && now < 1735689600) { /* RTC not yet set (before 2025) */
        tool_result_fail(r, TOOL_ST_ERROR, "time_not_synchronised", "no network time yet");
        return;
    }
    struct tm utc, loc;
    gmtime_r(&now, &utc);
    localtime_r(&now, &loc);
    char b[40];
    strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &utc);
    lla_jw_kv_str(&r->w, "utc", b);
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S %Z", &loc);
    lla_jw_kv_str(&r->w, "local", b);
    strftime(b, sizeof(b), "%A", &loc);
    lla_jw_kv_str(&r->w, "weekday", b);
    const char *tz = getenv("TZ");
    lla_jw_kv_str(&r->w, "timezone", tz && tz[0] ? tz : "UTC (not configured)");
}

static void x_heap(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)a; (void)dl;
    lla_jw_kv_int(&r->w, "free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    lla_jw_kv_int(&r->w, "min_free_internal", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    lla_jw_kv_int(&r->w, "largest_internal", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    lla_jw_kv_int(&r->w, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void x_net(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)a; (void)dl;
    net_status_t s;
    net_get_status(&s);
    lla_jw_kv_str(&r->w, "state", net_state_str(s.state));
    lla_jw_kv_str(&r->w, "ssid", s.ssid);
    lla_jw_kv_int(&r->w, "rssi", s.rssi);
    lla_jw_kv_int(&r->w, "channel", s.channel);
    lla_jw_kv_bool(&r->w, "time_synced", s.time_synced);
    lla_jw_kv_int(&r->w, "reconnects", s.disconnects);
}

static void x_list_rules(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)a; (void)dl;
    lla_jw_key(&r->w, "automation");
    event_engine_describe(&r->w);
}

static const char *const E_OP[] = {">", "<", NULL};
static const tool_arg_def_t A_RULE[] = {
    {.name = "name", .type = TARG_STR, .required = true, .max_len = 13},
    {.name = "sensor", .type = TARG_STR, .required = true, .max_len = 15},
    {.name = "op", .type = TARG_ENUM, .required = true, .enum_vals = E_OP},
    {.name = "threshold", .type = TARG_NUM, .required = true, .min = -1000, .max = 100000},
    {.name = "actuator", .type = TARG_STR, .required = true, .max_len = 15},
    {.name = "on", .desc = "actuator value when condition holds", .type = TARG_NUM, .required = true, .min = 0, .max = 100},
};

static void x_propose_rule(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)dl;
    rule_t ru = {0};
    snprintf(ru.name, sizeof(ru.name), "%s", tool_arg_str(d, a, "name", "rule"));
    ru.sensor = (int8_t)periph_find(tool_arg_str(d, a, "sensor", ""));
    ru.actuator = (int8_t)periph_find(tool_arg_str(d, a, "actuator", ""));
    if (ru.sensor < 0 || ru.actuator < 0) {
        tool_result_fail(r, TOOL_ST_REJECTED, "device_not_found", NULL);
        return;
    }
    const periph_dev_t *act = periph_get(ru.actuator);
    if (!act->cfg.llm_write) {
        tool_result_fail(r, TOOL_ST_REJECTED, "not_authorized_by_local_policy", "actuator not agent-writable");
        return;
    }
    ru.op = strcmp(tool_arg_str(d, a, "op", ">"), ">") == 0 ? RULE_GT : RULE_LT;
    ru.threshold = (float)tool_arg_num(d, a, "threshold", 0);
    ru.hysteresis = 1.0f;
    ru.on_value = (float)tool_arg_num(d, a, "on", 1);
    ru.off_value = act->cfg.safe_value;
    if (ru.on_value < act->cfg.min_value || ru.on_value > act->cfg.max_value) {
        tool_result_fail(r, TOOL_ST_REJECTED, "value_out_of_range", NULL);
        return;
    }
    event_engine_propose_rule(&ru);
    lla_jw_kv_str(&r->w, "status", "pending_local_approval");
    lla_jw_kv_str(&r->w, "note", "a person must run /rules approve on the device console");
}

static const tool_def_t k_tools[] = {
    {.name = "system_info", .desc = "Chip, memory sizes, firmware and SDK versions.", .perm = TOOL_PERM_READ,
     .timeout_ms = 100, .exec = x_system_info},
    {.name = "get_uptime", .desc = "Seconds since boot and last reset reason.", .perm = TOOL_PERM_READ,
     .timeout_ms = 100, .exec = x_uptime},
    {.name = "get_time", .desc = "Current date and time (UTC and the configured local time zone).",
     .perm = TOOL_PERM_READ, .timeout_ms = 100, .exec = x_time},
    {.name = "get_free_heap", .desc = "Free, minimum-ever and largest-block heap bytes.", .perm = TOOL_PERM_READ,
     .timeout_ms = 100, .exec = x_heap},
    {.name = "get_network_status", .desc = "Wi-Fi state, SSID, RSSI, time sync.", .perm = TOOL_PERM_READ,
     .timeout_ms = 100, .exec = x_net},
    {.name = "list_automation_rules", .desc = "Deterministic local rules and their state.", .perm = TOOL_PERM_READ,
     .timeout_ms = 100, .exec = x_list_rules},
    {.name = "propose_rule", .desc = "Propose a threshold rule; it only takes effect after local human approval.",
     .args = A_RULE, .n_args = 6, .perm = TOOL_PERM_ACTUATE, .effect = TOOL_EFFECT_IDEMPOTENT, .timeout_ms = 100,
     .exec = x_propose_rule},
};

int system_tools_register(tool_registry_t *reg)
{
    for (size_t i = 0; i < sizeof(k_tools) / sizeof(k_tools[0]); i++) {
        if (tool_registry_add(reg, &k_tools[i]) < 0) return -1;
    }
    return (int)(sizeof(k_tools) / sizeof(k_tools[0]));
}

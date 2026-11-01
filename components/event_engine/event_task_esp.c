/*
 * event_task_esp.c — the single "control" task: GPIO edge debouncing, sensor
 * sampling, deterministic rules, actuator watchdog and agent triggers.
 *
 * Runs every 50 ms, is registered with the task watchdog and never touches
 * the network, so local safety automation keeps working when Wi-Fi or the
 * LLM provider is down.
 */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "event_engine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lla_port.h"
#include "periph.h"
#include "periph_tools.h"

static const char *TAG = "events";

#define TICK_MS 50
#define ISR_Q_LEN 16
#define POST_Q_LEN 8

static struct {
    rules_t rules;
    triggers_t triggers;
    evq_t q;
    debounce_t deb[PERIPH_MAX_DEVICES];
    uint64_t next_sample[PERIPH_MAX_DEVICES];
    QueueHandle_t isr_q;
    QueueHandle_t post_q;
    event_agent_submit_fn submit;
    lla_lock_t lock;
    uint32_t samples;
    rule_t pending;
    bool has_pending;
    TaskHandle_t task;
    void (*long_press_cb)(void);
    uint64_t boot_down_since;
    bool boot_fired;
} E;

void event_engine_set_long_press_cb(void (*cb)(void)) { E.long_press_cb = cb; }

#define LONG_PRESS_MS 5000

/* BOOT long press: a local, physical gesture (e.g. start Wi-Fi provisioning). */
static void check_long_press(uint64_t now)
{
    const board_t *b = periph_board();
    if (!E.long_press_cb || !b || b->boot_btn_pin < 0) return;
    for (int i = 0; i < periph_count(); i++) {
        const periph_dev_t *d = periph_get(i);
        if (d->cfg.type != DEV_GPIO_IN || d->cfg.pin != b->boot_btn_pin) continue;
        float v;
        if (periph_read(i, &v, NULL) != PER_OK) return;
        if (v == 0) {
            E.boot_down_since = 0;
            E.boot_fired = false;
        } else if (!E.boot_down_since) {
            E.boot_down_since = now;
        } else if (!E.boot_fired && now - E.boot_down_since >= LONG_PRESS_MS) {
            E.boot_fired = true;
            ESP_LOGI(TAG, "BOOT held for %d s", LONG_PRESS_MS / 1000);
            E.long_press_cb();
        }
        return;
    }
}

/* ------------------------------------------------------------ rule env */

static int env_actuate(int a, float v, int rule) { return periph_set(a, v, rule); }
static int env_lock(int a, int rule, bool lock) { return lock ? periph_lock(a, rule) : periph_unlock(a, rule); }
static void env_emit(const event_t *e) { evq_push(&E.q, e); } /* only called from the event task */

static const rules_env_t k_env = {.actuate = env_actuate, .lock = env_lock, .emit = env_emit, .find_dev = periph_find};

/* ------------------------------------------------------------ ISR */

static void IRAM_ATTR edge_isr(int pin, void *u)
{
    int8_t idx = (int8_t)(intptr_t)u;
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(E.isr_q, &idx, &hp);
    if (hp) portYIELD_FROM_ISR();
}

int event_engine_watch(int idx, bool enable)
{
    int rc = periph_watch(idx, enable, edge_isr, (void *)(intptr_t)idx);
    if (rc == PER_OK && enable) {
        lla_lock(&E.lock);
        E.deb[idx].stable = -1; /* baseline on next poll */
        E.deb[idx].pending = true;
        E.deb[idx].pending_since = 0;
        lla_unlock(&E.lock);
    }
    return rc;
}

/* ------------------------------------------------------------ approved rules persistence */

static void rule_to_json(const rule_t *r, lla_jw_t *w)
{
    const periph_dev_t *s = periph_get(r->sensor), *a = periph_get(r->actuator);
    lla_jw_obj_begin(w);
    lla_jw_kv_str(w, "name", r->name);
    lla_jw_kv_str(w, "sensor", s ? s->cfg.name : "?");
    lla_jw_kv_str(w, "op", r->op == RULE_GT ? ">" : "<");
    lla_jw_kv_num(w, "threshold", r->threshold);
    lla_jw_kv_num(w, "hysteresis", r->hysteresis);
    lla_jw_kv_str(w, "actuator", a ? a->cfg.name : "?");
    lla_jw_kv_num(w, "on", r->on_value);
    lla_jw_kv_num(w, "off", r->off_value);
    lla_jw_obj_end(w);
}

static int persist_approved(void)
{
    /* Approved (non-profile) rules are stored by device name so a profile
     * reorder does not retarget them. Profile rules are never rewritten. */
    static char buf[512];
    lla_jw_t w;
    lla_jw_init(&w, buf, sizeof(buf));
    lla_jw_arr_begin(&w);
    for (uint8_t i = 0; i < E.rules.n; i++) {
        if (E.rules.r[i].name[0] == '+') rule_to_json(&E.rules.r[i], &w);
    }
    lla_jw_arr_end(&w);
    if (!lla_jw_ok(&w)) return -1;
    return cfg_set_str(CFG_NS, "rules_x", buf) < 0 ? -1 : 0;
}

/* ------------------------------------------------------------ profile */

int event_engine_load_profile(const char *json, size_t n, char *err, size_t errcap)
{
    lla_jtok_t *toks = malloc(sizeof(lla_jtok_t) * 384); /* rare: heap scratch */
    int nt = toks ? lla_json_parse(json, n, toks, 384) : LLA_JSON_ERR_NOMEM;
    if (nt < 0) {
        snprintf(err, errcap, "profile JSON invalid");
        free(toks);
        return -1;
    }
    lla_lock(&E.lock);
    rules_init(&E.rules, &k_env);
    int rc = rules_parse(&E.rules, json, toks, lla_json_obj_get(json, toks, 0, "rules"), err, errcap);
    if (rc == 0) {
        rc = triggers_parse(&E.triggers, json, toks, lla_json_obj_get(json, toks, 0, "triggers"), periph_find, err,
                            errcap);
    }
    if (rc == 0) {
        /* append locally approved agent proposals */
        char x[512];
        if (cfg_get_str(CFG_NS, "rules_x", x, sizeof(x)) == CFG_OK) {
            int xt = lla_json_parse(x, strlen(x), toks, 384);
            for (int i = 0; xt > 0 && toks[0].type == LLA_JT_ARRAY && i < toks[0].size && E.rules.n < RULES_MAX; i++) {
                char e2[64];
                rule_t r;
                if (rules_parse_one(&r, x, toks, lla_json_arr_get(toks, 0, i), e2, sizeof(e2)) == 0) {
                    E.rules.r[E.rules.n++] = r;
                } else {
                    ESP_LOGW(TAG, "stored rule %d skipped: %s", i, e2);
                }
            }
        }
    }
    memset(E.deb, 0, sizeof(E.deb));
    for (int i = 0; i < PERIPH_MAX_DEVICES; i++) E.deb[i].stable = -1;
    memset(E.next_sample, 0, sizeof(E.next_sample));
    lla_unlock(&E.lock);
    free(toks);
    if (rc == 0) ESP_LOGI(TAG, "%u rules, %u triggers loaded", E.rules.n, E.triggers.n);
    return rc;
}

/* ------------------------------------------------------------ proposals */

bool event_engine_propose_rule(const rule_t *r)
{
    lla_lock(&E.lock);
    E.pending = *r;
    E.has_pending = true;
    lla_unlock(&E.lock);
    printf("\n[rules] agent proposed rule '%s' — review with /rules, then /rules approve or /rules reject\n", r->name);
    return true;
}

bool event_engine_pending_rule(rule_t *out)
{
    lla_lock(&E.lock);
    bool h = E.has_pending;
    if (h && out) *out = E.pending;
    lla_unlock(&E.lock);
    return h;
}

int event_engine_approve_rule(void)
{
    lla_lock(&E.lock);
    int rc = -1;
    if (E.has_pending && E.rules.n < RULES_MAX) {
        rule_t r = E.pending;
        char n[16];
        snprintf(n, sizeof(n), "+%.14s", r.name); /* '+' marks locally approved proposals */
        memcpy(r.name, n, sizeof(r.name));
        r.safety = false; /* proposals never get safety locks; edit the profile for that */
        r.enabled = true;
        E.rules.r[E.rules.n++] = r;
        E.has_pending = false;
        rc = persist_approved();
    }
    lla_unlock(&E.lock);
    return rc;
}

void event_engine_reject_rule(void)
{
    lla_lock(&E.lock);
    E.has_pending = false;
    lla_unlock(&E.lock);
}

/* ------------------------------------------------------------ public */

rules_t *event_engine_rules(void) { return &E.rules; }

void event_engine_post(const event_t *e)
{
    if (E.post_q) xQueueSend(E.post_q, e, 0); /* never block the caller */
}

void event_engine_stats(uint32_t *dropped, uint32_t *deduped, uint32_t *samples)
{
    *dropped = E.q.dropped;
    *deduped = E.q.deduped;
    *samples = E.samples;
}

void event_engine_describe(lla_jw_t *w)
{
    lla_lock(&E.lock);
    lla_jw_obj_begin(w);
    lla_jw_key(w, "rules");
    lla_jw_arr_begin(w);
    for (uint8_t i = 0; i < E.rules.n; i++) {
        const rule_t *r = &E.rules.r[i];
        const periph_dev_t *s = periph_get(r->sensor), *a = periph_get(r->actuator);
        lla_jw_obj_begin(w);
        lla_jw_kv_str(w, "name", r->name);
        lla_jw_kv_str(w, "when", s ? s->cfg.name : "?");
        lla_jw_kv_str(w, "op", r->op == RULE_GT ? ">" : "<");
        lla_jw_kv_num(w, "threshold", r->threshold);
        lla_jw_kv_str(w, "then", a ? a->cfg.name : "?");
        lla_jw_kv_bool(w, "safety", r->safety);
        lla_jw_kv_bool(w, "active", r->active);
        if (r->faulted) lla_jw_kv_bool(w, "sensor_fault", true);
        lla_jw_obj_end(w);
    }
    lla_jw_arr_end(w);
    lla_jw_kv_int(w, "triggers", E.triggers.n);
    lla_jw_kv_bool(w, "proposal_pending", E.has_pending);
    lla_jw_obj_end(w);
    lla_unlock(&E.lock);
}

/* ------------------------------------------------------------ task */

static void handle_event(const event_t *e)
{
    bool dev_is_periph = e->type == EV_GPIO_EDGE || e->type == EV_SENSOR || e->type == EV_SENSOR_FAULT;
    const periph_dev_t *d = dev_is_periph && e->dev >= 0 ? periph_get(e->dev) : NULL;
    if (e->type == EV_BLE_SEEN) {
        ESP_LOGD(TAG, "event ble_seen #%d %.0f", e->dev, (double)e->value);
    } else if (e->type != EV_SENSOR) {
        ESP_LOGI(TAG, "event %s %s %.2f", ev_type_str(e->type),
                 e->type == EV_RULE_ON || e->type == EV_RULE_OFF || e->type == EV_RULE_FAULT
                     ? E.rules.r[e->dev].name
                     : (d ? d->cfg.name : ""),
                 (double)e->value);
    }
    trigger_t *t = triggers_match(&E.triggers, e, e->ts);
    if (t && E.submit) {
        char prompt[200];
        snprintf(prompt, sizeof(prompt), "[event %s%s%s value=%.2f] %s", ev_type_str(e->type), d ? " " : "",
                 d ? d->cfg.name : "", (double)e->value, t->prompt);
        E.submit(prompt, e);
    }
}

static void event_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    uint64_t next_second = 0;
    for (;;) {
        esp_task_wdt_reset();
        uint64_t now = lla_now_ms();
        int8_t idx;
        while (xQueueReceive(E.isr_q, &idx, 0) == pdTRUE) {
            if (idx >= 0 && idx < PERIPH_MAX_DEVICES) debounce_edge(&E.deb[idx], now);
        }
        event_t pe;
        while (xQueueReceive(E.post_q, &pe, 0) == pdTRUE) evq_push(&E.q, &pe);

        lla_lock(&E.lock);
        int n = periph_count();
        for (int i = 0; i < n; i++) {
            const periph_dev_t *d = periph_get(i);
            if (d->watched) {
                float v;
                int lvl;
                if (periph_read(i, &v, NULL) == PER_OK &&
                    debounce_poll(&E.deb[i], (int)v, now, d->cfg.debounce_ms, &lvl)) {
                    event_t e = {.type = EV_GPIO_EDGE, .dev = (int8_t)i, .value = (float)lvl, .ts = now};
                    evq_push(&E.q, &e);
                }
            }
            if (d->cfg.poll_ms && now >= E.next_sample[i]) {
                E.next_sample[i] = now + d->cfg.poll_ms;
                float v;
                int rc = periph_read(i, &v, NULL);
                E.samples++;
                if (rc == PER_OK) {
                    rules_eval(&E.rules, i, v, now);
                    event_t e = {.type = EV_SENSOR, .dev = (int8_t)i, .value = v, .ts = now};
                    evq_push(&E.q, &e);
                } else {
                    event_t e = {.type = EV_SENSOR_FAULT, .dev = (int8_t)i, .value = NAN, .ts = now};
                    evq_push(&E.q, &e);
                }
            }
        }
        check_long_press(now);
        if (now >= next_second) {
            next_second = now + 1000;
            rules_tick(&E.rules, now);
            int forced = periph_tick(now);
            if (forced) {
                event_t e = {.type = EV_WATCHDOG, .dev = -1, .value = (float)forced, .ts = now};
                evq_push(&E.q, &e);
            }
        }
        lla_unlock(&E.lock);

        event_t e;
        while (evq_pop(&E.q, &e)) handle_event(&e);
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

int event_engine_start(event_agent_submit_fn submit)
{
    lla_lock_init(&E.lock);
    E.submit = submit;
    evq_init(&E.q, 1000);
    rules_init(&E.rules, &k_env);
    for (int i = 0; i < PERIPH_MAX_DEVICES; i++) E.deb[i].stable = -1;
    E.isr_q = xQueueCreate(ISR_Q_LEN, sizeof(int8_t));
    E.post_q = xQueueCreate(POST_Q_LEN, sizeof(event_t));
    if (!E.isr_q || !E.post_q) return -1;
    periph_tools_set_watch_fn(event_engine_watch);
    /* Priority above the agent: local control must not wait for networking. */
    return xTaskCreate(event_task, "events", 4096, NULL, 6, &E.task) == pdPASS ? 0 : -1;
}

TaskHandle_t event_engine_task(void) { return E.task; }

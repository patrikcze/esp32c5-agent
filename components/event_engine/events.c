/* events.c — bounded event queue with dedup, debouncing and triggers. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "event_engine.h"

static const char *const k_ev_names[EV_TYPE_COUNT] = {"none",    "gpio_edge", "sensor",   "sensor_fault",
                                                      "rule_on", "rule_off",  "rule_fault", "net_up",
                                                      "net_down", "ble_seen", "watchdog"};

const char *ev_type_str(ev_type_t t) { return (unsigned)t < EV_TYPE_COUNT ? k_ev_names[t] : "?"; }

int ev_type_from_str(const char *s)
{
    for (int i = 1; i < EV_TYPE_COUNT; i++)
        if (strcmp(s, k_ev_names[i]) == 0) return i;
    return -1;
}

void evq_init(evq_t *q, uint32_t dedup_ms)
{
    memset(q, 0, sizeof(*q));
    q->dedup_ms = dedup_ms;
}

bool evq_push(evq_t *q, const event_t *e)
{
    /* sensor samples are periodic by design and never deduplicated */
    if (e->type != EV_SENSOR) {
        for (int i = 0; i < 4; i++) {
            const event_t *l = &q->last[i];
            if (l->type == e->type && l->dev == e->dev && l->value == e->value && e->ts - l->ts < q->dedup_ms &&
                l->ts != 0) {
                q->deduped++;
                return false;
            }
        }
        q->last[q->last_i] = *e;
        q->last_i = (uint8_t)((q->last_i + 1) % 4);
    }
    if (q->count == EVQ_LEN) {
        q->head = (uint8_t)((q->head + 1) % EVQ_LEN);
        q->count--;
        q->dropped++;
    }
    q->q[(q->head + q->count) % EVQ_LEN] = *e;
    q->count++;
    return true;
}

bool evq_pop(evq_t *q, event_t *e)
{
    if (!q->count) return false;
    *e = q->q[q->head];
    q->head = (uint8_t)((q->head + 1) % EVQ_LEN);
    q->count--;
    return true;
}

void debounce_edge(debounce_t *d, uint64_t now)
{
    d->pending = true;
    d->pending_since = now; /* restart the window on every bounce */
}

bool debounce_poll(debounce_t *d, int level_now, uint64_t now, uint16_t debounce_ms, int *level)
{
    if (d->stable < 0) {
        d->stable = (int8_t)level_now; /* first observation establishes the baseline */
        d->pending = false;
        return false;
    }
    if (!d->pending || now - d->pending_since < debounce_ms) return false;
    d->pending = false;
    if (level_now == d->stable) return false; /* glitch */
    d->stable = (int8_t)level_now;
    *level = level_now;
    return true;
}

int triggers_parse(triggers_t *ts, const char *js, const lla_jtok_t *t, int arr, int (*find_dev)(const char *),
                   char *err, size_t errcap)
{
    memset(ts, 0, sizeof(*ts));
    if (arr < 0) return 0;
    if (t[arr].type != LLA_JT_ARRAY || t[arr].size > TRIGGERS_MAX) {
        snprintf(err, errcap, "triggers must be an array of at most %d", TRIGGERS_MAX);
        return -1;
    }
    for (int i = 0; i < t[arr].size; i++) {
        int o = lla_json_arr_get(t, arr, i);
        trigger_t *tr = &ts->t[i];
        char buf[24];
        int on = lla_json_obj_get(js, t, o, "on");
        int ev = (on >= 0 && lla_json_str(js, &t[on], buf, sizeof(buf)) >= 0) ? ev_type_from_str(buf) : -1;
        if (ev < 0) {
            snprintf(err, errcap, "triggers[%d]: unknown 'on' event", i);
            return -1;
        }
        tr->ev_type = (uint8_t)ev;
        tr->dev = -1;
        int dv = lla_json_obj_get(js, t, o, "device");
        if (dv >= 0) {
            if (lla_json_str(js, &t[dv], buf, sizeof(buf)) < 0 || (tr->dev = (int8_t)find_dev(buf)) < 0) {
                snprintf(err, errcap, "triggers[%d]: unknown device", i);
                return -1;
            }
        }
        int vv = lla_json_obj_get(js, t, o, "value");
        double v;
        if (vv >= 0 && lla_json_num(js, &t[vv], &v)) {
            tr->match_value = true;
            tr->value = (float)v;
        }
        int mi = lla_json_obj_get(js, t, o, "min_interval_s");
        tr->min_interval_ms = 60000;
        if (mi >= 0 && lla_json_num(js, &t[mi], &v)) {
            if (v < 5 || v > 86400) {
                snprintf(err, errcap, "triggers[%d]: min_interval_s must be 5..86400", i);
                return -1;
            }
            tr->min_interval_ms = (uint32_t)(v * 1000);
        }
        int pr = lla_json_obj_get(js, t, o, "prompt");
        if (pr < 0 || lla_json_str(js, &t[pr], tr->prompt, sizeof(tr->prompt)) <= 0) {
            snprintf(err, errcap, "triggers[%d]: prompt missing or longer than %u", i, (unsigned)sizeof(tr->prompt) - 1);
            return -1;
        }
        ts->n++;
    }
    return 0;
}

trigger_t *triggers_match(triggers_t *ts, const event_t *e, uint64_t now)
{
    for (uint8_t i = 0; i < ts->n; i++) {
        trigger_t *t = &ts->t[i];
        if (t->ev_type != e->type) continue;
        if (t->dev >= 0 && t->dev != e->dev) continue;
        if (t->match_value && fabsf(t->value - e->value) > 1e-6f) continue;
        if (t->last_fired && now - t->last_fired < t->min_interval_ms) {
            t->suppressed++;
            continue;
        }
        t->last_fired = now ? now : 1;
        return t;
    }
    return NULL;
}

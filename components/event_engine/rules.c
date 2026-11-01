/* rules.c — deterministic threshold rules with hysteresis and fail-safe. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "event_engine.h"
#include "lla_port.h"

static const char *TAG = "rules";
static rules_env_t s_env;

void rules_init(rules_t *rs, const rules_env_t *env)
{
    memset(rs, 0, sizeof(*rs));
    if (env) s_env = *env;
}

const rules_env_t *rules_env(void) { return &s_env; }

static void emit(uint8_t type, int dev, float v, uint64_t now)
{
    if (!s_env.emit) return;
    event_t e = {.type = type, .dev = (int8_t)dev, .value = v, .ts = now};
    s_env.emit(&e);
}

static int jnum(const char *js, const lla_jtok_t *t, int o, const char *k, double *v)
{
    int i = lla_json_obj_get(js, t, o, k);
    return i >= 0 && lla_json_num(js, &t[i], v);
}

int rules_parse_one(rule_t *r, const char *js, const lla_jtok_t *t, int o, char *err, size_t errcap)
{
    memset(r, 0, sizeof(*r));
    char buf[24];
    int k;
    if (t[o].type != LLA_JT_OBJECT) {
        snprintf(err, errcap, "rule must be an object");
        return -1;
    }
    k = lla_json_obj_get(js, t, o, "name");
    if (k < 0 || lla_json_str(js, &t[k], r->name, sizeof(r->name)) <= 0) {
        snprintf(err, errcap, "rule name missing (max 15 chars)");
        return -1;
    }
    k = lla_json_obj_get(js, t, o, "sensor");
    if (k < 0 || lla_json_str(js, &t[k], buf, sizeof(buf)) < 0 || (r->sensor = (int8_t)s_env.find_dev(buf)) < 0) {
        snprintf(err, errcap, "%s: unknown sensor", r->name);
        return -1;
    }
    k = lla_json_obj_get(js, t, o, "actuator");
    if (k < 0 || lla_json_str(js, &t[k], buf, sizeof(buf)) < 0 || (r->actuator = (int8_t)s_env.find_dev(buf)) < 0) {
        snprintf(err, errcap, "%s: unknown actuator", r->name);
        return -1;
    }
    k = lla_json_obj_get(js, t, o, "op");
    if (k < 0 || lla_json_str(js, &t[k], buf, sizeof(buf)) < 0 || (strcmp(buf, ">") && strcmp(buf, "<"))) {
        snprintf(err, errcap, "%s: op must be \">\" or \"<\"", r->name);
        return -1;
    }
    r->op = buf[0] == '>' ? RULE_GT : RULE_LT;
    double v;
    if (!jnum(js, t, o, "threshold", &v)) {
        snprintf(err, errcap, "%s: threshold missing", r->name);
        return -1;
    }
    r->threshold = (float)v;
    r->hysteresis = jnum(js, t, o, "hysteresis", &v) ? (float)v : 1.0f;
    if (r->hysteresis < 0) {
        snprintf(err, errcap, "%s: hysteresis must be >= 0", r->name);
        return -1;
    }
    r->on_value = jnum(js, t, o, "on", &v) ? (float)v : 1.0f;
    r->off_value = jnum(js, t, o, "off", &v) ? (float)v : 0.0f;
    r->stale_ms = jnum(js, t, o, "stale_s", &v) ? (uint32_t)(v * 1000) : 0;
    if (jnum(js, t, o, "fault", &v)) {
        r->fault_value_set = 1;
        r->fault_value = (float)v;
    }
    k = lla_json_obj_get(js, t, o, "safety");
    r->safety = k >= 0 && t[k].type == LLA_JT_TRUE;
    k = lla_json_obj_get(js, t, o, "enabled");
    r->enabled = k < 0 || t[k].type == LLA_JT_TRUE;
    return 0;
}

int rules_parse(rules_t *rs, const char *js, const lla_jtok_t *t, int arr, char *err, size_t errcap)
{
    rs->n = 0;
    if (arr < 0) return 0;
    if (t[arr].type != LLA_JT_ARRAY || t[arr].size > RULES_MAX) {
        snprintf(err, errcap, "rules must be an array of at most %d", RULES_MAX);
        return -1;
    }
    for (int i = 0; i < t[arr].size; i++) {
        if (rules_parse_one(&rs->r[i], js, t, lla_json_arr_get(t, arr, i), err, errcap) < 0) {
            rs->n = 0;
            return -1;
        }
        for (int j = 0; j < i; j++) {
            if (rs->r[j].safety && rs->r[i].safety && rs->r[j].actuator == rs->r[i].actuator) {
                snprintf(err, errcap, "%s: actuator already owned by safety rule %s", rs->r[i].name, rs->r[j].name);
                rs->n = 0;
                return -1;
            }
        }
        rs->n++;
    }
    return 0;
}

static void apply(rule_t *r, int idx, float v, uint64_t now)
{
    int rc = s_env.actuate ? s_env.actuate(r->actuator, v, idx) : -1;
    if (rc != 0) LLA_LOGE(TAG, "rule %s: actuate failed (%d)", r->name, rc);
    (void)now;
}

void rules_eval(rules_t *rs, int sensor, float value, uint64_t now)
{
    for (uint8_t i = 0; i < rs->n; i++) {
        rule_t *r = &rs->r[i];
        if (!r->enabled || r->sensor != sensor || isnan(value)) continue;
        r->last_ok_ms = now;
        if (r->faulted) {
            /* sensor recovered: re-evaluate from scratch */
            r->faulted = false;
            r->active = false;
            if (r->safety && s_env.lock) s_env.lock(r->actuator, i, false);
            LLA_LOGI(TAG, "rule %s: sensor recovered", r->name);
        }
        bool trip = r->op == RULE_GT ? value > r->threshold : value < r->threshold;
        bool release = r->op == RULE_GT ? value < r->threshold - r->hysteresis : value > r->threshold + r->hysteresis;
        if (!r->active && trip) {
            r->active = true;
            r->fired++;
            apply(r, i, r->on_value, now);
            if (r->safety && s_env.lock) s_env.lock(r->actuator, i, true);
            LLA_LOGI(TAG, "rule %s ON (%.2f)", r->name, (double)value);
            emit(EV_RULE_ON, i, value, now);
        } else if (r->active && release) {
            r->active = false;
            if (r->safety && s_env.lock) s_env.lock(r->actuator, i, false);
            apply(r, i, r->off_value, now);
            LLA_LOGI(TAG, "rule %s OFF (%.2f)", r->name, (double)value);
            emit(EV_RULE_OFF, i, value, now);
        }
    }
}

void rules_tick(rules_t *rs, uint64_t now)
{
    for (uint8_t i = 0; i < rs->n; i++) {
        rule_t *r = &rs->r[i];
        if (!r->enabled || !r->stale_ms || r->faulted) continue;
        uint64_t since = r->last_ok_ms ? r->last_ok_ms : 0;
        if (since == 0) {
            r->last_ok_ms = now; /* start the stale window at first tick */
            continue;
        }
        if (now - since >= r->stale_ms) {
            r->faulted = true;
            LLA_LOGW(TAG, "rule %s: sensor stale for %u s", r->name, (unsigned)((now - since) / 1000));
            if (r->fault_value_set) {
                apply(r, i, r->fault_value, now);
                if (r->safety && s_env.lock) s_env.lock(r->actuator, i, true);
            }
            emit(EV_RULE_FAULT, i, NAN, now);
        }
    }
}

void rules_describe_one(const rule_t *r, lla_jw_t *w)
{
    lla_jw_obj_begin(w);
    lla_jw_kv_str(w, "name", r->name);
    lla_jw_kv_int(w, "sensor", r->sensor);
    lla_jw_kv_str(w, "op", r->op == RULE_GT ? ">" : "<");
    lla_jw_kv_num(w, "threshold", r->threshold);
    lla_jw_kv_num(w, "hysteresis", r->hysteresis);
    lla_jw_kv_int(w, "actuator", r->actuator);
    lla_jw_kv_num(w, "on", r->on_value);
    lla_jw_kv_num(w, "off", r->off_value);
    lla_jw_kv_bool(w, "safety", r->safety);
    lla_jw_kv_bool(w, "enabled", r->enabled);
    lla_jw_kv_bool(w, "active", r->active);
    if (r->faulted) lla_jw_kv_bool(w, "sensor_fault", true);
    lla_jw_obj_end(w);
}

void rules_describe(const rules_t *rs, lla_jw_t *w)
{
    lla_jw_arr_begin(w);
    for (uint8_t i = 0; i < rs->n; i++) rules_describe_one(&rs->r[i], w);
    lla_jw_arr_end(w);
}

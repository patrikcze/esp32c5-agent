/*
 * event_engine.h — events, deterministic rules and agent triggers.
 *
 * Portable logic (events.c, rules.c) + ESP task wrapper (event_task_esp.c).
 *
 * Deterministic rules run locally on every sensor sample and do not need the
 * network or the LLM:  IF sensor <op> threshold THEN set actuator, with
 * hysteresis, optional safety lock (agent cannot override while active) and
 * a fail-safe action when the sensor stops delivering valid readings.
 *
 * Triggers turn selected events into bounded agent runs (rate limited).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lla_json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV_NONE = 0,
    EV_GPIO_EDGE, /* value = new logical level (debounced) */
    EV_SENSOR,    /* value = sample */
    EV_SENSOR_FAULT,
    EV_RULE_ON,
    EV_RULE_OFF,
    EV_RULE_FAULT,
    EV_NET_UP,
    EV_NET_DOWN,
    EV_BLE_SEEN,  /* dev = ble table index */
    EV_WATCHDOG,  /* actuator forced safe by max-on watchdog */
    EV_TYPE_COUNT
} ev_type_t;

const char *ev_type_str(ev_type_t t);
int ev_type_from_str(const char *s);

typedef struct {
    uint8_t type;
    int8_t dev;
    float value;
    uint64_t ts;
} event_t;

/* ------------------------------------------------------------ queue */

#define EVQ_LEN 16
typedef struct {
    event_t q[EVQ_LEN];
    uint8_t head, count;
    uint32_t dropped;
    uint32_t deduped;
    event_t last[4]; /* recent events for dedup */
    uint8_t last_i;
    uint32_t dedup_ms;
} evq_t;

void evq_init(evq_t *q, uint32_t dedup_ms);
/** Push with dedup; when full the oldest event is dropped (counted). */
bool evq_push(evq_t *q, const event_t *e);
bool evq_pop(evq_t *q, event_t *e);

/* ------------------------------------------------------------ debounce */

typedef struct {
    int8_t stable;          /* last stable level, -1 unknown */
    bool pending;
    uint64_t pending_since;
} debounce_t;

/** Feed a raw edge notification (level sampled now). */
void debounce_edge(debounce_t *d, uint64_t now);
/** Returns true and sets *level when a new stable level is confirmed. */
bool debounce_poll(debounce_t *d, int level_now, uint64_t now, uint16_t debounce_ms, int *level);

/* ------------------------------------------------------------ rules */

#define RULES_MAX 8
typedef enum { RULE_GT = 0, RULE_LT } rule_op_t;

typedef struct {
    char name[16];
    bool enabled;
    bool safety;        /* hold a lock on the actuator while active/faulted */
    int8_t sensor;      /* periph device index */
    int8_t actuator;    /* periph device index */
    uint8_t op;         /* rule_op_t */
    float threshold;
    float hysteresis;
    float on_value;
    float off_value;
    uint32_t stale_ms;  /* sensor fault after this long without a valid sample (0 = off) */
    int8_t fault_value_set; /* 1 if fault_value is defined */
    float fault_value;
    /* runtime */
    bool active;
    bool faulted;
    uint64_t last_ok_ms;
    uint32_t fired;
} rule_t;

typedef struct {
    rule_t r[RULES_MAX];
    uint8_t n;
} rules_t;

/** Actuation callback (periph_set with by_rule). */
typedef int (*rule_actuate_fn)(int actuator, float value, int rule);
typedef int (*rule_lock_fn)(int actuator, int rule, bool lock);
typedef void (*rule_event_fn)(const event_t *e);

typedef struct {
    rule_actuate_fn actuate;
    rule_lock_fn lock;
    rule_event_fn emit;
    int (*find_dev)(const char *name);
} rules_env_t;

void rules_init(rules_t *rs, const rules_env_t *env);
/** Parse "rules":[...] from a profile JSON object token. Validates devices. */
int rules_parse(rules_t *rs, const char *js, const lla_jtok_t *t, int arr, char *err, size_t errcap);
/** Parse one rule object (used for agent proposals). */
int rules_parse_one(rule_t *r, const char *js, const lla_jtok_t *t, int obj, char *err, size_t errcap);
void rules_eval(rules_t *rs, int sensor, float value, uint64_t now);
void rules_tick(rules_t *rs, uint64_t now);
void rules_describe(const rules_t *rs, lla_jw_t *w);
void rules_describe_one(const rule_t *r, lla_jw_t *w);
const rules_env_t *rules_env(void);

/* ------------------------------------------------------------ triggers */

#define TRIGGERS_MAX 6
typedef struct {
    uint8_t ev_type;
    int8_t dev;           /* -1 any */
    bool match_value;
    float value;
    uint32_t min_interval_ms;
    char prompt[120];
    uint64_t last_fired;
    uint32_t suppressed;
} trigger_t;

typedef struct {
    trigger_t t[TRIGGERS_MAX];
    uint8_t n;
} triggers_t;

int triggers_parse(triggers_t *ts, const char *js, const lla_jtok_t *t, int arr, int (*find_dev)(const char *),
                   char *err, size_t errcap);
/** Returns matching trigger (rate limit applied) or NULL. */
trigger_t *triggers_match(triggers_t *ts, const event_t *e, uint64_t now);

#ifdef ESP_PLATFORM
/* ------------------------------------------------------------ ESP task */
typedef void (*event_agent_submit_fn)(const char *prompt, const event_t *e);
/** Start the event task: sensor sampling, debouncing, rules, watchdog. */
int event_engine_start(event_agent_submit_fn submit);
/** Load rules/triggers from profile JSON (same document as devices). */
int event_engine_load_profile(const char *json, size_t n, char *err, size_t errcap);
void event_engine_post(const event_t *e); /* task context */
void event_engine_describe(lla_jw_t *w);
rules_t *event_engine_rules(void);
void event_engine_stats(uint32_t *dropped, uint32_t *deduped, uint32_t *samples);
int event_engine_watch(int dev_idx, bool enable);
/** Pending agent-proposed rule awaiting local approval. */
bool event_engine_propose_rule(const rule_t *r);
bool event_engine_pending_rule(rule_t *out);
int event_engine_approve_rule(void);
void event_engine_reject_rule(void);
#endif

#ifdef __cplusplus
}
#endif

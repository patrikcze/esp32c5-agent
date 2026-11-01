/*
 * tool_registry.h — typed tool definitions, compact argument schema,
 * validation, permission policy and execution with structured results.
 *
 * Portable: depends only on lla_common.
 *
 * Schema subset (deliberately small, see docs/AGENT-DESIGN.md):
 *   - top-level arguments must be a JSON object
 *   - each argument is int | number | bool | string | enum(string)
 *   - int/number have inclusive [min,max]; strings have max_len
 *   - required flag; unknown keys are rejected (additionalProperties=false)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "lla_json.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TOOL_MAX_TOOLS
#define TOOL_MAX_TOOLS 40
#endif
#define TOOL_MAX_ARGS 6
#define TOOL_NAME_MAX 32
#define TOOL_STR_POOL 256      /* bytes for all decoded string args of one call */
#define TOOL_RESULT_MAX 768    /* JSON result payload returned to the model */

typedef enum { TARG_INT, TARG_NUM, TARG_BOOL, TARG_STR, TARG_ENUM } tool_arg_type_t;

typedef struct {
    const char *name;
    const char *desc;
    tool_arg_type_t type;
    bool required;
    double min;        /* TARG_INT/TARG_NUM */
    double max;
    uint16_t max_len;  /* TARG_STR (excluding NUL) */
    const char *const *enum_vals; /* TARG_ENUM, NULL-terminated */
} tool_arg_def_t;

/** Permission class, ordered by increasing risk. */
typedef enum {
    TOOL_PERM_READ = 0,      /* observes state only */
    TOOL_PERM_ACTUATE = 1,   /* changes a registered actuator within its limits */
    TOOL_PERM_CONFIG = 2,    /* changes runtime configuration (pins, watches, rules) */
    TOOL_PERM_DANGEROUS = 3, /* bus writes, raw device commands */
} tool_perm_t;

/**
 * Side-effect classification. Drives the duplicate-execution policy in the
 * agent: NONE may always repeat; IDEMPOTENT may repeat with identical args
 * (setting the same state again is harmless); NON_IDEMPOTENT is never
 * auto-repeated with identical args in one run.
 */
typedef enum { TOOL_EFFECT_NONE = 0, TOOL_EFFECT_IDEMPOTENT, TOOL_EFFECT_NON_IDEMPOTENT } tool_effect_t;

/** Who initiated the agent run (policy ceiling is per source). */
typedef enum { TOOL_SRC_CONSOLE = 0, TOOL_SRC_AUTONOMOUS, TOOL_SRC_BLE, TOOL_SRC_REMOTE, TOOL_SRC_COUNT } tool_source_t;

typedef enum {
    TOOL_ST_OK = 0,
    TOOL_ST_ERROR,     /* executed (or tried) and definitively failed; no side effect happened */
    TOOL_ST_REJECTED,  /* refused before execution: validation or policy */
    TOOL_ST_TIMEOUT,   /* deadline exceeded before the operation started; no side effect */
    TOOL_ST_UNKNOWN,   /* operation may or may not have taken effect */
} tool_status_t;

typedef struct {
    bool present;
    tool_arg_type_t type;
    union {
        int64_t i;
        double d;
        bool b;
        const char *s; /* points into tool_args_t.pool */
    } v;
} tool_arg_val_t;

typedef struct {
    tool_arg_val_t a[TOOL_MAX_ARGS];
    char pool[TOOL_STR_POOL];
    uint16_t pool_used;
} tool_args_t;

typedef struct {
    tool_status_t status;
    bool verified;          /* postcondition checked and held */
    lla_jw_t w;             /* writer over json[]; tools write ONE JSON object */
    char json[TOOL_RESULT_MAX];
} tool_result_t;

struct tool_def;
typedef void (*tool_exec_fn)(const struct tool_def *def, const tool_args_t *args, uint64_t deadline_ms,
                             tool_result_t *res);
/** Optional postcondition check; returns true if the intended state holds. */
typedef bool (*tool_verify_fn)(const struct tool_def *def, const tool_args_t *args, tool_result_t *res);

typedef struct tool_def {
    const char *name;
    const char *desc;
    const tool_arg_def_t *args;
    uint8_t n_args;
    tool_perm_t perm;
    tool_effect_t effect;
    uint32_t timeout_ms;
    tool_exec_fn exec;
    tool_verify_fn verify;
    void *ctx;
} tool_def_t;

/* --------------------------------------------------------------- policy */

typedef struct {
    tool_perm_t ceiling[TOOL_SRC_COUNT];     /* max permission allowed per source */
    uint64_t deny_mask;                      /* bit i → tool index i denied always */
    uint64_t allow_autonomous_mask;          /* tools explicitly allowed for AUTONOMOUS above ceiling */
    uint16_t min_interval_ms;                /* min interval between side-effecting calls of same tool */
} tool_policy_t;

void tool_policy_default(tool_policy_t *p);

/* -------------------------------------------------------------- registry */

typedef struct {
    const tool_def_t *tools[TOOL_MAX_TOOLS];
    uint8_t count;
    tool_policy_t policy;
    uint64_t last_side_effect_ms[TOOL_MAX_TOOLS];
} tool_registry_t;

void tool_registry_init(tool_registry_t *r);
/** @return index, or -1 if full / duplicate name / invalid definition. */
int tool_registry_add(tool_registry_t *r, const tool_def_t *def);
int tool_registry_find(const tool_registry_t *r, const char *name, size_t name_len);

typedef enum { TOOL_FMT_OPENAI = 0, TOOL_FMT_ANTHROPIC } tool_schema_fmt_t;

/** Is tool @p idx usable by @p src under the current policy? */
bool tool_registry_permitted(const tool_registry_t *r, int idx, tool_source_t src);

/** Emit the JSON array of tool schemas permitted for @p src. */
void tool_registry_write_schemas(const tool_registry_t *r, tool_source_t src, tool_schema_fmt_t fmt, lla_jw_t *w);

/**
 * Validate a raw JSON argument object against the tool's schema.
 * On failure writes a short reason (for the model) into @p why.
 */
bool tool_args_parse(const tool_def_t *def, const char *json, size_t len, tool_args_t *out, char *why,
                     size_t why_cap);

/** Accessors (return defaults when arg absent). */
int tool_arg_index(const tool_def_t *def, const char *name);
int64_t tool_arg_int(const tool_def_t *def, const tool_args_t *a, const char *name, int64_t dflt);
double tool_arg_num(const tool_def_t *def, const tool_args_t *a, const char *name, double dflt);
bool tool_arg_bool(const tool_def_t *def, const tool_args_t *a, const char *name, bool dflt);
const char *tool_arg_str(const tool_def_t *def, const tool_args_t *a, const char *name, const char *dflt);
bool tool_arg_present(const tool_def_t *def, const tool_args_t *a, const char *name);

/* ------------------------------------------------------------ results */

void tool_result_begin(tool_result_t *res);
/** Finish result: closes the object. On overflow replaces payload with an error. */
void tool_result_end(tool_result_t *res);
/** Shortcut: {"error":"<code>","detail":"<detail>"} with given status. */
void tool_result_fail(tool_result_t *res, tool_status_t st, const char *code, const char *detail);

const char *tool_status_str(tool_status_t s);
const char *tool_perm_str(tool_perm_t p);
const char *tool_source_str(tool_source_t s);

/**
 * Full validated invocation: policy → parse → exec → verify.
 * Never calls exec if validation or policy fails.
 */
void tool_registry_invoke(tool_registry_t *r, int idx, tool_source_t src, const char *args_json, size_t args_len,
                          uint64_t now_ms, tool_result_t *res, void (*before_verify)(void *u), void *u);

#ifdef __cplusplus
}
#endif

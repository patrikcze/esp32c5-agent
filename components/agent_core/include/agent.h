/*
 * agent.h — bounded agent execution loop (portable).
 *
 * One agent_t owns all buffers for a run; nothing is allocated while running.
 * Transitions (see docs/AGENT-DESIGN.md for the full table):
 *
 *   IDLE → CONNECTING → REQUESTING_MODEL → PARSING_RESPONSE
 *        ├─ no tool calls ───────────────────────────────→ VERIFYING → COMPLETED
 *        └─ tool calls → VALIDATING_TOOL → EXECUTING_TOOL [→ VERIFYING]
 *                        → (next call …) → SUBMITTING_RESULT → REQUESTING_MODEL
 *   REQUESTING_MODEL/PARSING_RESPONSE --retryable--> RECOVERING → REQUESTING_MODEL
 *   any --fatal--> ERROR      any --cancel flag--> CANCELED
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "llm_provider.h"
#include "lla_port.h"
#include "tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AG_IDLE = 0,
    AG_CONNECTING,
    AG_REQUESTING_MODEL,
    AG_PARSING_RESPONSE,
    AG_VALIDATING_TOOL,
    AG_EXECUTING_TOOL,
    AG_VERIFYING,
    AG_SUBMITTING_RESULT,
    AG_RECOVERING,
    AG_COMPLETED,
    AG_ERROR,
    AG_CANCELED,
} agent_state_t;

const char *agent_state_str(agent_state_t s);

typedef struct {
    uint8_t max_model_turns;     /* model requests per run */
    uint8_t max_tool_calls;      /* executed tool calls per run */
    uint8_t max_identical_calls; /* identical (tool,args) executions per run */
    uint8_t max_retries;         /* per model request */
    uint8_t max_loop_strikes;    /* repeated-call rejections before abort */
    uint32_t deadline_ms;        /* whole run */
    uint32_t backoff_base_ms;
    uint32_t backoff_max_ms;
    uint32_t net_wait_ms;        /* CONNECTING wait */
} agent_budget_t;

void agent_budget_default(agent_budget_t *b);

typedef struct {
    char *req_buf;
    size_t req_cap;
    char *resp_buf;      /* SSE line / non-stream body */
    size_t resp_cap;
    lla_jtok_t *toks;
    uint16_t max_toks;
    uint8_t *conv_buf;
    uint16_t conv_cap;
} agent_mem_t;

typedef struct {
    /** Wait up to wait_ms for network+time; false if unavailable. May be NULL. */
    bool (*net_ready)(void *u, uint32_t wait_ms);
    void (*on_state)(void *u, agent_state_t st, const char *detail);
    void (*on_tool)(void *u, const char *name, const char *args, const tool_result_t *res);
    void *u;
} agent_hooks_t;

typedef struct {
    agent_state_t final_state;
    char error[112];
    uint8_t model_turns;
    uint8_t tool_calls;
    uint8_t retries;
    uint8_t rejected_calls;
    uint8_t unknown_outcomes;
    uint32_t elapsed_ms;
    uint32_t model_ms;
    uint32_t tool_ms;
    uint32_t in_tokens, out_tokens;
    uint32_t diag;          /* OR of LLM_DIAG_* over the run */
    uint32_t req_bytes_max; /* largest request body */
} agent_run_result_t;

#define AGENT_LEDGER_MAX 24
#define AGENT_SYSTEM_MAX 640

typedef struct {
    uint32_t hash;
    uint8_t tool;
    uint8_t status; /* tool_status_t of the last execution */
    uint8_t count;
} agent_ledger_t;

typedef struct agent {
    llm_cfg_t cfg;
    const llm_provider_t *prov;
    llm_transport_t *tr;
    tool_registry_t *reg;
    agent_budget_t budget;
    agent_mem_t mem;
    agent_hooks_t hooks;
    char system_prompt[AGENT_SYSTEM_MAX];

    llm_conv_t conv;
    llm_response_t resp;
    llm_parser_t parser;
    tool_result_t results[LLM_MAX_CALLS];
    char hdr_scratch[224];

    volatile bool cancel;
    volatile agent_state_t state;
    uint16_t run_id;
    tool_source_t src;
    agent_ledger_t ledger[AGENT_LEDGER_MAX];
    volatile uint64_t run_started_ms; /* 0 while idle; read by the watchdog */
    uint8_t n_ledger;
    uint8_t loop_strikes;
    agent_run_result_t last;
} agent_t;

void agent_init(agent_t *a, const agent_mem_t *mem, llm_transport_t *tr, tool_registry_t *reg);
/** Replace provider config (copies, including the secret). */
bool agent_set_provider(agent_t *a, const llm_cfg_t *cfg);
void agent_set_system_prompt(agent_t *a, const char *s);
void agent_set_hooks(agent_t *a, const agent_hooks_t *h);

/**
 * Run one request to completion (blocking, in the caller's task).
 * @return final state: AG_COMPLETED, AG_ERROR or AG_CANCELED.
 * The final answer text is agent_last_text().
 */
agent_state_t agent_run(agent_t *a, tool_source_t src, const char *request, agent_run_result_t *out);

/** Request cancellation of the current run (safe from any task). */
void agent_cancel(agent_t *a);
/** Clear the working context (user-requested). */
void agent_clear_context(agent_t *a);
const char *agent_last_text(const agent_t *a);

/**
 * Run supervision. A run must finish within its deadline; transports and
 * tools enforce it cooperatively. If a run is still active `grace_ms` after
 * its deadline it is cancelled; after a second grace period the task is
 * considered hung and the caller should restart the device (outputs are
 * driven safe by the shutdown handler).
 */
typedef enum { AGENT_WD_OK = 0, AGENT_WD_CANCEL, AGENT_WD_RESTART } agent_wd_action_t;
agent_wd_action_t agent_watchdog_check(const agent_t *a, uint64_t now_ms, uint32_t grace_ms);

/** Exposed for tests: normalized hash of (tool, args). */
uint32_t agent_call_hash(const char *name, const char *args, size_t n);

#ifdef __cplusplus
}
#endif

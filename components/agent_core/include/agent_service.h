/*
 * agent_service.h — ESP-IDF runtime around agent_t: one agent task, a
 * bounded job queue, provider configuration persistence and budgets.
 */
#pragma once

#include <stdbool.h>

#include "agent.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool (*net_ready)(uint32_t wait_ms);
} agent_service_env_t;

int agent_service_start(tool_registry_t *reg, const agent_service_env_t *env);
/** Queue a request (copied). Returns false if the queue is full. */
bool agent_service_submit(tool_source_t src, const char *text);
void agent_service_cancel(void);
bool agent_service_busy(void);
agent_t *agent_service_agent(void);
void agent_service_mem_report(void);

/* Provider configuration (persisted; API keys in the secret namespace). */
int provider_load(llm_kind_t kind, llm_cfg_t *out);
int provider_save(const llm_cfg_t *cfg);
llm_kind_t provider_active_kind(void);
int provider_set_active(llm_kind_t kind);
/** Re-read persisted provider config into the agent (between runs). */
void agent_service_reload_provider(void);

/* Tool policy persistence */
int policy_load(tool_registry_t *reg);
int policy_save(const tool_registry_t *reg);

#ifdef __cplusplus
}
#endif

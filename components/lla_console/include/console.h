#pragma once
#include "tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *default_profile; /* shown by /devices profile when none stored */
} console_env_t;

/** Start the UART REPL (non-blocking; runs in its own task). */
int console_start(tool_registry_t *reg, const console_env_t *env);

#ifdef __cplusplus
}
#endif

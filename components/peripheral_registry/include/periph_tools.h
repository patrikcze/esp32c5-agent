#pragma once

#include "periph.h"
#include "tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Hook installed by the event engine to start/stop debounced edge watching. */
typedef int (*periph_watch_fn)(int dev_idx, bool enable);
void periph_tools_set_watch_fn(periph_watch_fn fn);

/** Register hardware tools. Returns number registered or -1. */
int periph_tools_register(tool_registry_t *reg);

#ifdef __cplusplus
}
#endif

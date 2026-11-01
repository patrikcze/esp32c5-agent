#pragma once
#include "tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register system introspection tools (system_info, get_uptime, ...). */
int system_tools_register(tool_registry_t *reg);

#ifdef __cplusplus
}
#endif

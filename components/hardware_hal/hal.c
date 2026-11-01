/* hal.c — active HAL pointer and error names (portable). */
#include "hal.h"

static const hal_ops_t *s_hal;

void hal_set(const hal_ops_t *ops) { s_hal = ops; }
const hal_ops_t *hal(void) { return s_hal; }

const char *hal_err_str(int e)
{
    switch (e) {
    case HAL_OK: return "ok";
    case HAL_ERR_IO: return "io";
    case HAL_ERR_TIMEOUT: return "timeout";
    case HAL_ERR_UNSUPPORTED: return "unsupported";
    case HAL_ERR_ARG: return "invalid_arg";
    case HAL_ERR_UNCERTAIN: return "uncertain";
    case HAL_ERR_NOMEM: return "no_memory";
    default: return "?";
    }
}

/*
 * lla_port.h — minimal platform abstraction used by the portable llmagent core.
 *
 * Everything under components/ that is listed as "portable" in
 * docs/ARCHITECTURE.md depends only on this header (plus the C standard
 * library). The ESP-IDF implementation lives in port_esp.c; the host test
 * implementation (with a controllable mock clock) lives in tests/host/port_host.c.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Monotonic milliseconds since boot. */
uint64_t lla_now_ms(void);

/** Block the calling task. On host this advances the mock clock instantly. */
void lla_sleep_ms(uint32_t ms);

/** Pseudo-random 32-bit value (used for backoff jitter only, not crypto). */
uint32_t lla_rand32(void);

typedef enum { LLA_LOG_ERROR = 1, LLA_LOG_WARN, LLA_LOG_INFO, LLA_LOG_DEBUG } lla_log_level_t;

void lla_log(lla_log_level_t lvl, const char *tag, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#define LLA_LOGE(tag, ...) lla_log(LLA_LOG_ERROR, tag, __VA_ARGS__)
#define LLA_LOGW(tag, ...) lla_log(LLA_LOG_WARN, tag, __VA_ARGS__)
#define LLA_LOGI(tag, ...) lla_log(LLA_LOG_INFO, tag, __VA_ARGS__)
#define LLA_LOGD(tag, ...) lla_log(LLA_LOG_DEBUG, tag, __VA_ARGS__)

/** Opaque recursive lock. Statically sized so no allocation is needed. */
typedef struct {
    void *impl;
} lla_lock_t;

void lla_lock_init(lla_lock_t *l);
void lla_lock(lla_lock_t *l);
void lla_unlock(lla_lock_t *l);

#ifdef __cplusplus
}
#endif

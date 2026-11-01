/*
 * config.h — persistent configuration with wear protection and secret
 * separation.
 *
 * Two namespaces: CFG_NS (normal settings, exportable) and CFG_NS_SECRET
 * (Wi-Fi password, API keys; never exported or logged). Writes are skipped
 * when the value is unchanged and are limited to CFG_MAX_WRITES_PER_HOUR to
 * bound flash wear even if a bug or a remote caller loops.
 *
 * Portable core (config_core.c) over a key/value backend: NVS on ESP
 * (config_nvs_esp.c), in-memory map in host tests.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_NS "llacfg"
#define CFG_NS_SECRET "llasec"
#define CFG_MAX_WRITES_PER_HOUR 60

typedef struct {
    /** Read blob; *len in: capacity, out: size. Returns 0, -1 not found, -2 too small, -3 error. */
    int (*get)(const char *ns, const char *key, void *buf, size_t *len);
    int (*set)(const char *ns, const char *key, const void *buf, size_t len);
    int (*erase)(const char *ns, const char *key);
    int (*erase_ns)(const char *ns);
} cfg_backend_t;

enum { CFG_OK = 0, CFG_NOT_FOUND = -1, CFG_TOO_SMALL = -2, CFG_IO = -3, CFG_RATE_LIMITED = -4, CFG_UNCHANGED = 1 };

void cfg_init(const cfg_backend_t *be);
int cfg_get(const char *ns, const char *key, void *buf, size_t *len);
/** NUL-terminated string get; returns CFG_* */
int cfg_get_str(const char *ns, const char *key, char *buf, size_t cap);
int cfg_set(const char *ns, const char *key, const void *buf, size_t len);
int cfg_set_str(const char *ns, const char *key, const char *s);
int cfg_erase(const char *ns, const char *key);
/** Factory reset: erase both namespaces (not rate-limited). */
int cfg_factory_reset(void);

typedef struct {
    uint32_t writes;
    uint32_t skipped_unchanged;
    uint32_t rate_limited;
    uint32_t window_writes;
} cfg_stats_t;
void cfg_stats(cfg_stats_t *s);

/** "<unset>" or "<set, N chars>" — never reveals any secret characters. */
void cfg_redact(const char *secret, char *out, size_t cap);

#ifdef ESP_PLATFORM
/** Initialise NVS (encrypted when CONFIG_NVS_ENCRYPTION) and bind the core. */
int cfg_nvs_init(void);
#endif

#ifdef __cplusplus
}
#endif

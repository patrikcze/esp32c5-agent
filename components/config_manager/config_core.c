/* config_core.c — unchanged-skip and hourly write budget over a KV backend. */
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "lla_port.h"

static const char *TAG = "cfg";

static const cfg_backend_t *s_be;
static cfg_stats_t s_st;
static uint64_t s_window_start;
static lla_lock_t s_lock;

void cfg_init(const cfg_backend_t *be)
{
    s_be = be;
    memset(&s_st, 0, sizeof(s_st));
    s_window_start = lla_now_ms();
    lla_lock_init(&s_lock);
}

int cfg_get(const char *ns, const char *key, void *buf, size_t *len)
{
    if (!s_be) return CFG_IO;
    return s_be->get(ns, key, buf, len);
}

int cfg_get_str(const char *ns, const char *key, char *buf, size_t cap)
{
    if (cap == 0) return CFG_TOO_SMALL;
    size_t len = cap - 1;
    int rc = cfg_get(ns, key, buf, &len);
    if (rc != CFG_OK) {
        buf[0] = '\0';
        return rc;
    }
    buf[len] = '\0';
    return CFG_OK;
}

int cfg_set(const char *ns, const char *key, const void *buf, size_t len)
{
    if (!s_be) return CFG_IO;
    lla_lock(&s_lock);
    /* compare with stored value: identical writes cost no flash wear */
    static uint8_t cur[512];
    size_t cl = sizeof(cur);
    if (len <= sizeof(cur) && s_be->get(ns, key, cur, &cl) == CFG_OK && cl == len && memcmp(cur, buf, len) == 0) {
        s_st.skipped_unchanged++;
        lla_unlock(&s_lock);
        return CFG_UNCHANGED;
    }
    uint64_t now = lla_now_ms();
    if (now - s_window_start >= 3600000ULL) {
        s_window_start = now;
        s_st.window_writes = 0;
    }
    if (s_st.window_writes >= CFG_MAX_WRITES_PER_HOUR) {
        s_st.rate_limited++;
        lla_unlock(&s_lock);
        LLA_LOGW(TAG, "write budget exhausted, %s/%s not written", ns, key);
        return CFG_RATE_LIMITED;
    }
    int rc = s_be->set(ns, key, buf, len);
    if (rc == CFG_OK) {
        s_st.writes++;
        s_st.window_writes++;
    }
    lla_unlock(&s_lock);
    return rc;
}

int cfg_set_str(const char *ns, const char *key, const char *s) { return cfg_set(ns, key, s, strlen(s)); }

int cfg_erase(const char *ns, const char *key) { return s_be ? s_be->erase(ns, key) : CFG_IO; }

int cfg_factory_reset(void)
{
    if (!s_be) return CFG_IO;
    int a = s_be->erase_ns(CFG_NS);
    int b = s_be->erase_ns(CFG_NS_SECRET);
    return a ? a : b;
}

void cfg_stats(cfg_stats_t *s) { *s = s_st; }

void cfg_redact(const char *secret, char *out, size_t cap)
{
    size_t n = secret ? strlen(secret) : 0;
    if (n == 0) snprintf(out, cap, "<unset>");
    else snprintf(out, cap, "<set, %u chars>", (unsigned)n);
}

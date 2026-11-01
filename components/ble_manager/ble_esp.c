/*
 * ble_esp.c — NimBLE central + peripheral.
 *
 * All GAP/GATT callbacks run in the NimBLE host task; blocking API calls
 * from other tasks wait on a semaphore with a timeout. One operation at a
 * time (s_op mutex); one central connection.
 */
#include "sdkconfig.h"
#if CONFIG_BT_NIMBLE_ENABLED
#include <stdio.h>
#include <string.h>

#include "agent_service.h"
#include "ble_mgr.h"
#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "event_engine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "net.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "periph.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void); /* NimBLE store, not exported by a header in IDF */

static const char *TAG = "ble";

#define MAX_SVCS 12
#define MAX_CHRS 32

typedef struct {
    ble_uuid_any_t uuid;
    uint16_t start, end;
} svc_t;

typedef struct {
    ble_uuid_any_t uuid;
    uint16_t def, val, end;
    uint8_t props;
    uint16_t cccd;
} chr_t;

static struct {
    bool synced;
    uint8_t own_addr_type;
    SemaphoreHandle_t op;   /* serialises public operations */
    SemaphoreHandle_t done; /* signalled by host-task callbacks */
    volatile int status;
    ble_seen_t seen[BLE_MAX_SEEN];
    int n_seen;
    char filter[20];
    /* allowlist */
    uint8_t allow[BLE_MAX_ALLOW][6];
    bool allow_rw[BLE_MAX_ALLOW];
    int n_allow;
    /* central link */
    uint16_t conn;
    int peer;
    svc_t svcs[MAX_SVCS];
    int n_svcs;
    chr_t chrs[MAX_CHRS];
    int n_chrs;
    int disc_svc_i;
    uint8_t *rd_buf;
    size_t rd_cap, rd_len;
    /* peripheral */
    uint16_t periph_conn;
    uint32_t scans, notifies;
    /* central auto-reconnect: after an unexpected link loss to the device the
     * user connected, reconnect with bounded backoff and restore
     * notification subscriptions. A user disconnect ends it. */
    bool want_link;
    ble_addr_t peer_addr;
    uint8_t rc_attempt;
    esp_timer_handle_t rc_timer;
    uint16_t sub_cccd[8];
    uint8_t sub_val[8];
    uint8_t n_sub;
    uint32_t reconnects;
} B = {.conn = BLE_HS_CONN_HANDLE_NONE, .periph_conn = BLE_HS_CONN_HANDLE_NONE, .peer = -1};

const char *ble_err_str(int e)
{
    switch (e) {
    case BLE_ERR_NOT_READY: return "ble_not_ready";
    case BLE_ERR_BUSY: return "busy";
    case BLE_ERR_TIMEOUT: return "timeout";
    case BLE_ERR_NOT_ALLOWED: return "device_not_in_allowlist";
    case BLE_ERR_NOT_CONNECTED: return "not_connected";
    case BLE_ERR_NOT_FOUND: return "characteristic_not_found";
    case BLE_ERR_GATT: return "gatt_error";
    case BLE_ERR_ARG: return "invalid_argument";
    case BLE_ERR_NOT_WRITABLE: return "write_not_authorized";
    case BLE_ERR_UNCERTAIN: return "outcome_uncertain";
    default: return e >= 0 ? "ok" : "error";
    }
}

void ble_addr_str(const uint8_t a[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
}

bool ble_addr_parse(const char *s, uint8_t a[6])
{
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[5], &v[4], &v[3], &v[2], &v[1], &v[0]) != 6) return false;
    for (int i = 0; i < 6; i++) a[i] = (uint8_t)v[i];
    return true;
}

/* ------------------------------------------------------------ sync helpers */

static bool begin_op(void)
{
    if (!B.synced) return false;
    if (xSemaphoreTake(B.op, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    xSemaphoreTake(B.done, 0); /* clear stale signal */
    return true;
}

static void end_op(void) { xSemaphoreGive(B.op); }

static int wait_done(uint32_t ms)
{
    if (xSemaphoreTake(B.done, pdMS_TO_TICKS(ms)) != pdTRUE) return BLE_ERR_TIMEOUT;
    return B.status;
}

static void op_signal(int st)
{
    B.status = st;
    xSemaphoreGive(B.done);
}

/* ------------------------------------------------------------ allowlist */

static void allow_persist(void)
{
    char buf[BLE_MAX_ALLOW * 22 + 1] = "";
    for (int i = 0; i < B.n_allow; i++) {
        char a[18];
        ble_addr_str(B.allow[i], a);
        strlcat(buf, a, sizeof(buf));
        strlcat(buf, B.allow_rw[i] ? "/rw;" : "/r;", sizeof(buf));
    }
    cfg_set_str(CFG_NS, "ble_allow", buf);
}

static void allow_load(void)
{
    char buf[BLE_MAX_ALLOW * 22 + 1];
    if (cfg_get_str(CFG_NS, "ble_allow", buf, sizeof(buf)) != CFG_OK) return;
    char *save = NULL;
    for (char *t = strtok_r(buf, ";", &save); t && B.n_allow < BLE_MAX_ALLOW; t = strtok_r(NULL, ";", &save)) {
        if (ble_addr_parse(t, B.allow[B.n_allow])) {
            B.allow_rw[B.n_allow] = strstr(t, "/rw") != NULL;
            B.n_allow++;
        }
    }
}

bool ble_mgr_allowed(const uint8_t addr[6], bool *writable)
{
    for (int i = 0; i < B.n_allow; i++) {
        if (memcmp(B.allow[i], addr, 6) == 0) {
            if (writable) *writable = B.allow_rw[i];
            return true;
        }
    }
    return false;
}

int ble_mgr_allow(const uint8_t addr[6], bool writable)
{
    for (int i = 0; i < B.n_allow; i++) {
        if (memcmp(B.allow[i], addr, 6) == 0) {
            B.allow_rw[i] = writable;
            allow_persist();
            return 0;
        }
    }
    if (B.n_allow >= BLE_MAX_ALLOW) return BLE_ERR_ARG;
    memcpy(B.allow[B.n_allow], addr, 6);
    B.allow_rw[B.n_allow++] = writable;
    allow_persist();
    return 0;
}

int ble_mgr_deny(const uint8_t addr[6])
{
    for (int i = 0; i < B.n_allow; i++) {
        if (memcmp(B.allow[i], addr, 6) == 0) {
            memmove(&B.allow[i], &B.allow[i + 1], (size_t)(B.n_allow - i - 1) * 6);
            memmove(&B.allow_rw[i], &B.allow_rw[i + 1], (size_t)(B.n_allow - i - 1));
            B.n_allow--;
            allow_persist();
            return 0;
        }
    }
    return BLE_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------ scanning */

static int gap_event(struct ble_gap_event *ev, void *arg);

static void record_adv(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return;
    char name[20] = "";
    if (f.name && f.name_len) snprintf(name, sizeof(name), "%.*s", f.name_len < 19 ? f.name_len : 19, (const char *)f.name);
    if (B.filter[0] && strncmp(name, B.filter, strlen(B.filter)) != 0) return;
    int idx = -1;
    for (int i = 0; i < B.n_seen; i++) {
        if (memcmp(B.seen[i].addr, d->addr.val, 6) == 0) idx = i;
    }
    if (idx < 0) {
        if (B.n_seen >= BLE_MAX_SEEN) return; /* bounded table */
        idx = B.n_seen++;
        memset(&B.seen[idx], 0, sizeof(B.seen[idx]));
        memcpy(B.seen[idx].addr, d->addr.val, 6);
        B.seen[idx].addr_type = d->addr.type;
        event_t e = {.type = EV_BLE_SEEN, .dev = (int8_t)idx, .value = d->rssi, .ts = esp_timer_get_time() / 1000};
        event_engine_post(&e);
    }
    ble_seen_t *s = &B.seen[idx];
    if (name[0]) strlcpy(s->name, name, sizeof(s->name));
    s->rssi = d->rssi;
    s->last_seen_ms = esp_timer_get_time() / 1000;
    for (int i = 0; i < f.num_uuids16; i++) {
        uint16_t u = ble_uuid_u16(&f.uuids16[i].u);
        if (u == 0x181A) s->ess = true;
        if (u == 0x180F) s->battery = true;
    }
}

int ble_mgr_scan(uint32_t ms, const char *prefix)
{
    if (ms < 500) ms = 500;
    if (ms > 10000) ms = 10000;
    if (B.conn != BLE_HS_CONN_HANDLE_NONE) return BLE_ERR_BUSY; /* keep the peer's table entry stable */
    if (!begin_op()) return B.synced ? BLE_ERR_BUSY : BLE_ERR_NOT_READY;
    B.n_seen = 0;
    strlcpy(B.filter, prefix ? prefix : "", sizeof(B.filter));
    struct ble_gap_disc_params p = {.itvl = 0x50, .window = 0x30, .passive = 0, .filter_duplicates = 1};
    int64_t t0 = esp_timer_get_time();
    int rc = ble_gap_disc(B.own_addr_type, (int32_t)ms, &p, gap_event, NULL);
    if (rc != 0) {
        end_op();
        ESP_LOGW(TAG, "ble_gap_disc rc=%d", rc);
        return BLE_ERR_BUSY;
    }
    rc = wait_done(ms + 1000);
    if (rc == BLE_ERR_TIMEOUT) ble_gap_disc_cancel();
    B.scans++;
    ESP_LOGI(TAG, "scan %u ms: %d devices (%lld ms wall)", (unsigned)ms, B.n_seen, (esp_timer_get_time() - t0) / 1000);
    end_op();
    return rc < 0 ? rc : B.n_seen;
}

int ble_mgr_seen_count(void) { return B.n_seen; }
const ble_seen_t *ble_mgr_seen(int i) { return i >= 0 && i < B.n_seen ? &B.seen[i] : NULL; }

/* ------------------------------------------------------------ central GATT */

static int on_dsc(uint16_t ch, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc, void *arg)
{
    chr_t *c = arg;
    if (err->status == 0 && dsc && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) c->cccd = dsc->handle;
    if (err->status == BLE_HS_EDONE) op_signal(0);
    else if (err->status != 0) op_signal(BLE_ERR_GATT);
    return 0;
}

static int on_chr(uint16_t ch, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr && B.n_chrs < MAX_CHRS) {
        chr_t *c = &B.chrs[B.n_chrs++];
        memset(c, 0, sizeof(*c));
        c->uuid = chr->uuid;
        c->def = chr->def_handle;
        c->val = chr->val_handle;
        c->props = chr->properties;
        c->end = B.svcs[B.disc_svc_i].end;
        if (B.n_chrs > 1 && B.chrs[B.n_chrs - 2].end >= c->def) B.chrs[B.n_chrs - 2].end = c->def - 1;
    }
    if (err->status == BLE_HS_EDONE) op_signal(0);
    else if (err->status != 0) op_signal(BLE_ERR_GATT);
    return 0;
}

static int on_svc(uint16_t ch, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0 && svc && B.n_svcs < MAX_SVCS) {
        B.svcs[B.n_svcs].uuid = svc->uuid;
        B.svcs[B.n_svcs].start = svc->start_handle;
        B.svcs[B.n_svcs].end = svc->end_handle;
        B.n_svcs++;
    }
    if (err->status == BLE_HS_EDONE) op_signal(0);
    else if (err->status != 0) op_signal(BLE_ERR_GATT);
    return 0;
}

int ble_mgr_connect(int idx, uint32_t timeout_ms)
{
    const ble_seen_t *s = ble_mgr_seen(idx);
    if (!s) return BLE_ERR_ARG;
    if (!ble_mgr_allowed(s->addr, NULL)) return BLE_ERR_NOT_ALLOWED;
    if (B.conn != BLE_HS_CONN_HANDLE_NONE) return BLE_ERR_BUSY;
    if (!begin_op()) return B.synced ? BLE_ERR_BUSY : BLE_ERR_NOT_READY;
    ble_addr_t peer = {.type = s->addr_type};
    memcpy(peer.val, s->addr, 6);
    B.peer = idx;
    B.n_svcs = B.n_chrs = 0;
    int rc = ble_gap_connect(B.own_addr_type, &peer, (int32_t)timeout_ms, NULL, gap_event, NULL);
    if (rc != 0) {
        end_op();
        return BLE_ERR_GATT;
    }
    rc = wait_done(timeout_ms + 1000);
    if (rc == BLE_ERR_TIMEOUT) ble_gap_conn_cancel();
    end_op();
    return rc;
}

#define RC_MAX_ATTEMPTS 6
#define RC_MAX_DELAY_MS 30000

static void rc_schedule(void)
{
    if (!B.want_link || B.rc_attempt >= RC_MAX_ATTEMPTS) {
        if (B.want_link) ESP_LOGW(TAG, "auto-reconnect gave up after %d attempts", RC_MAX_ATTEMPTS);
        B.want_link = false;
        B.n_svcs = B.n_chrs = 0;
        B.n_sub = 0;
        return;
    }
    uint32_t d = 1000u << B.rc_attempt;
    if (d > RC_MAX_DELAY_MS) d = RC_MAX_DELAY_MS;
    B.rc_attempt++;
    esp_timer_stop(B.rc_timer);
    esp_timer_start_once(B.rc_timer, (uint64_t)d * 1000);
    ESP_LOGI(TAG, "auto-reconnect attempt %u in %u ms", B.rc_attempt, (unsigned)d);
}

static void rc_timer_cb(void *arg)
{
    (void)arg;
    if (!B.want_link || !B.synced || B.conn != BLE_HS_CONN_HANDLE_NONE) return;
    int rc = ble_gap_connect(B.own_addr_type, &B.peer_addr, 5000, NULL, gap_event, (void *)2);
    if (rc != 0) {
        /* busy (e.g. a scan is running): try again later, same attempt budget */
        rc_schedule();
    }
}

static void rc_restore(void)
{
    for (uint8_t i = 0; i < B.n_sub; i++) {
        uint8_t v[2] = {B.sub_val[i], 0};
        ble_gattc_write_flat(B.conn, B.sub_cccd[i], v, 2, NULL, NULL);
    }
}

int ble_mgr_disconnect(void)
{
    B.want_link = false; /* user-initiated: never auto-reconnect */
    esp_timer_stop(B.rc_timer);
    B.n_sub = 0;
    if (B.conn == BLE_HS_CONN_HANDLE_NONE) {
        B.n_svcs = B.n_chrs = 0;
        return BLE_ERR_NOT_CONNECTED;
    }
    if (!begin_op()) return BLE_ERR_BUSY;
    int rc = ble_gap_terminate(B.conn, BLE_ERR_REM_USER_CONN_TERM);
    if (rc == 0) rc = wait_done(3000);
    end_op();
    return rc < 0 ? rc : 0;
}

bool ble_mgr_connected(void) { return B.conn != BLE_HS_CONN_HANDLE_NONE; }

int ble_mgr_discover(uint32_t timeout_ms)
{
    if (B.conn == BLE_HS_CONN_HANDLE_NONE) return BLE_ERR_NOT_CONNECTED;
    if (!begin_op()) return BLE_ERR_BUSY;
    int64_t dl = esp_timer_get_time() / 1000 + timeout_ms;
    B.n_svcs = B.n_chrs = 0;
    int rc = ble_gattc_disc_all_svcs(B.conn, on_svc, NULL);
    rc = rc ? BLE_ERR_GATT : wait_done(timeout_ms);
    for (int i = 0; rc == 0 && i < B.n_svcs; i++) {
        int64_t left = dl - esp_timer_get_time() / 1000;
        if (left <= 0) {
            rc = BLE_ERR_TIMEOUT;
            break;
        }
        B.disc_svc_i = i;
        rc = ble_gattc_disc_all_chrs(B.conn, B.svcs[i].start, B.svcs[i].end, on_chr, NULL);
        rc = rc ? BLE_ERR_GATT : wait_done((uint32_t)left);
    }
    end_op();
    return rc < 0 ? rc : B.n_chrs;
}

static chr_t *find_chr(const char *uuid_s)
{
    ble_uuid_any_t u;
    char norm[40];
    /* accept "2a6e" as shorthand for 16-bit UUIDs */
    if (strlen(uuid_s) == 4) snprintf(norm, sizeof(norm), "0x%s", uuid_s);
    else strlcpy(norm, uuid_s, sizeof(norm));
    if (ble_uuid_from_str(&u, norm) != 0) return NULL;
    for (int i = 0; i < B.n_chrs; i++) {
        if (ble_uuid_cmp(&B.chrs[i].uuid.u, &u.u) == 0) return &B.chrs[i];
    }
    return NULL;
}

static int on_read(uint16_t ch, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status == 0 && attr) {
        uint16_t l = OS_MBUF_PKTLEN(attr->om);
        if (l > B.rd_cap) l = (uint16_t)B.rd_cap;
        ble_hs_mbuf_to_flat(attr->om, B.rd_buf, l, NULL);
        B.rd_len = l;
        op_signal(0);
    } else {
        op_signal(BLE_ERR_GATT);
    }
    return 0;
}

int ble_mgr_read(const char *uuid, uint8_t *out, size_t cap, uint32_t timeout_ms)
{
    if (B.conn == BLE_HS_CONN_HANDLE_NONE) return BLE_ERR_NOT_CONNECTED;
    if (!begin_op()) return BLE_ERR_BUSY;
    chr_t *c = find_chr(uuid);
    int rc = BLE_ERR_NOT_FOUND;
    if (c && (c->props & BLE_GATT_CHR_PROP_READ)) {
        B.rd_buf = out;
        B.rd_cap = cap;
        B.rd_len = 0;
        rc = ble_gattc_read(B.conn, c->val, on_read, NULL) ? BLE_ERR_GATT : wait_done(timeout_ms);
        if (rc == 0) rc = (int)B.rd_len;
    }
    end_op();
    return rc;
}

static int on_write(uint16_t ch, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    op_signal(err->status == 0 ? 0 : BLE_ERR_GATT);
    return 0;
}

int ble_mgr_write(const char *uuid, const uint8_t *d, size_t n, uint32_t timeout_ms)
{
    if (B.conn == BLE_HS_CONN_HANDLE_NONE) return BLE_ERR_NOT_CONNECTED;
    /* Authorise against the live link's identity address, never the scan
     * table, which a rescan may have rewritten since connect. */
    bool rw = false;
    struct ble_gap_conn_desc cd;
    if (ble_gap_conn_find(B.conn, &cd) != 0 || !ble_mgr_allowed(cd.peer_id_addr.val, &rw) || !rw)
        return BLE_ERR_NOT_WRITABLE;
    if (!begin_op()) return BLE_ERR_BUSY;
    chr_t *c = find_chr(uuid);
    int rc = BLE_ERR_NOT_FOUND;
    if (c && (c->props & BLE_GATT_CHR_PROP_WRITE)) {
        rc = ble_gattc_write_flat(B.conn, c->val, d, (uint16_t)n, on_write, NULL) ? BLE_ERR_GATT : wait_done(timeout_ms);
        /* a write whose response never arrived may still have been applied */
        if (rc == BLE_ERR_TIMEOUT) rc = BLE_ERR_UNCERTAIN;
    }
    end_op();
    return rc;
}

int ble_mgr_subscribe(const char *uuid, uint32_t timeout_ms)
{
    if (B.conn == BLE_HS_CONN_HANDLE_NONE) return BLE_ERR_NOT_CONNECTED;
    if (!begin_op()) return BLE_ERR_BUSY;
    chr_t *c = find_chr(uuid);
    int rc = BLE_ERR_NOT_FOUND;
    if (c && (c->props & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE))) {
        if (!c->cccd) {
            rc = ble_gattc_disc_all_dscs(B.conn, c->val, c->end, on_dsc, c) ? BLE_ERR_GATT : wait_done(timeout_ms);
        } else {
            rc = 0;
        }
        if (rc == 0 && c->cccd) {
            uint8_t v[2] = {(c->props & BLE_GATT_CHR_PROP_NOTIFY) ? 1 : 2, 0};
            rc = ble_gattc_write_flat(B.conn, c->cccd, v, 2, on_write, NULL) ? BLE_ERR_GATT : wait_done(timeout_ms);
            if (rc == 0) {
                bool known = false;
                for (uint8_t i = 0; i < B.n_sub; i++) known |= B.sub_cccd[i] == c->cccd;
                if (!known && B.n_sub < 8) {
                    B.sub_cccd[B.n_sub] = c->cccd;
                    B.sub_val[B.n_sub++] = v[0];
                }
            }
        } else if (rc == 0) {
            rc = BLE_ERR_NOT_FOUND;
        }
    }
    end_op();
    return rc;
}

/* ------------------------------------------------------------ peripheral GATT service */

/* 6c6c6167-656e-7400-0000-00000000000x  ("llagent") */
static const ble_uuid128_t k_svc = BLE_UUID128_INIT(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x74, 0x6e,
                                                    0x65, 0x67, 0x61, 0x6c, 0x6c);
static const ble_uuid128_t k_chr_status = BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x74,
                                                           0x6e, 0x65, 0x67, 0x61, 0x6c, 0x6c);
static const ble_uuid128_t k_chr_caps = BLE_UUID128_INIT(0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x74,
                                                         0x6e, 0x65, 0x67, 0x61, 0x6c, 0x6c);
static const ble_uuid128_t k_chr_req = BLE_UUID128_INIT(0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x74,
                                                        0x6e, 0x65, 0x67, 0x61, 0x6c, 0x6c);

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    static char buf[480];
    int which = (int)(intptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        lla_jw_t w;
        lla_jw_init(&w, buf, sizeof(buf));
        lla_jw_obj_begin(&w);
        if (which == 1) {
            net_status_t ns;
            net_get_status(&ns);
            lla_jw_kv_int(&w, "uptime_s", esp_timer_get_time() / 1000000);
            lla_jw_kv_str(&w, "wifi", net_state_str(ns.state));
            lla_jw_kv_bool(&w, "agent_busy", agent_service_busy());
        } else {
            lla_jw_key(&w, "devices");
            int om = periph_describe(&w, 64);
            if (om) lla_jw_kv_int(&w, "omitted", om);
        }
        lla_jw_obj_end(&w);
        if (!lla_jw_ok(&w)) strcpy(buf, "{\"truncated\":true}");
        return os_mbuf_append(ctxt->om, buf, strlen(buf)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && which == 3) {
        uint16_t l = OS_MBUF_PKTLEN(ctxt->om);
        if (l == 0 || l >= 300) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        static char req[300];
        ble_hs_mbuf_to_flat(ctxt->om, req, l, NULL);
        req[l] = '\0';
        for (uint16_t i = 0; i < l; i++) {
            if ((unsigned char)req[i] < 0x20) req[i] = ' ';
        }
        return agent_service_submit(TOOL_SRC_BLE, req) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def k_gatt[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_svc.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {.uuid = &k_chr_status.u, .access_cb = gatt_access, .arg = (void *)1, .flags = BLE_GATT_CHR_F_READ},
                {.uuid = &k_chr_caps.u, .access_cb = gatt_access, .arg = (void *)2,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
                {.uuid = &k_chr_req.u, .access_cb = gatt_access, .arg = (void *)3,
                 .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN},
                {0},
            },
    },
    {0},
};

static void advertise(void)
{
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    f.name = (const uint8_t *)name;
    f.name_len = (uint8_t)strlen(name);
    f.name_is_complete = 1;
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&k_svc;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0 || ble_gap_adv_rsp_set_fields(&rsp) != 0) {
        ESP_LOGE(TAG, "advertising data rejected");
        return;
    }
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    int rc = ble_gap_adv_start(B.own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, (void *)1);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "adv start rc=%d", rc);
}

/* ------------------------------------------------------------ GAP events */

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    bool from_adv = arg == (void *)1;
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        record_adv(&ev->disc);
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        op_signal(0);
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (from_adv) {
            if (ev->connect.status == 0) {
                B.periph_conn = ev->connect.conn_handle;
                ESP_LOGI(TAG, "peripheral: central connected");
            } else {
                advertise();
            }
            return 0;
        }
        bool auto_rc = arg == (void *)2;
        if (ev->connect.status == 0) {
            B.conn = ev->connect.conn_handle;
            struct ble_gap_conn_desc cd;
            if (ble_gap_conn_find(B.conn, &cd) != 0 || !ble_mgr_allowed(cd.peer_id_addr.val, NULL)) {
                B.want_link = false;
                ble_gap_terminate(B.conn, BLE_ERR_REM_USER_CONN_TERM); /* identity not allowlisted */
                if (!auto_rc) op_signal(BLE_ERR_NOT_ALLOWED);
                return 0;
            }
            if (auto_rc) {
                B.reconnects++;
                B.rc_attempt = 0;
                ESP_LOGI(TAG, "central link restored; re-enabling %u subscription(s)", B.n_sub);
                rc_restore();
                return 0;
            }
            B.peer_addr = cd.peer_ota_addr;
            B.want_link = true;
            B.rc_attempt = 0;
            B.n_sub = 0;
            op_signal(0);
        } else if (auto_rc) {
            rc_schedule();
        } else {
            op_signal(BLE_ERR_GATT);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        if (ev->disconnect.conn.conn_handle == B.periph_conn) {
            B.periph_conn = BLE_HS_CONN_HANDLE_NONE;
            advertise();
        } else if (ev->disconnect.conn.conn_handle == B.conn) {
            B.conn = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGI(TAG, "central link closed (reason 0x%x)", ev->disconnect.reason);
            if (B.want_link) {
                /* unexpected loss: keep the GATT table (same device, same
                 * handles) and reconnect in the background */
                rc_schedule();
            } else {
                B.n_svcs = B.n_chrs = 0;
            }
            op_signal(0);
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (ev->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io pk = {.action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000};
            printf("\n[ble] PAIRING REQUEST — passkey: %06u (enter it on the phone; ignore if unexpected)\n",
                   (unsigned)pk.passkey);
            ble_sm_inject_io(ev->passkey.conn_handle, &pk);
        }
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change status=%d", ev->enc_change.status);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
        B.notifies++;
        uint8_t v[8];
        uint16_t l = OS_MBUF_PKTLEN(ev->notify_rx.om);
        if (l > sizeof(v)) l = sizeof(v);
        ble_hs_mbuf_to_flat(ev->notify_rx.om, v, l, NULL);
        double val = l ? v[0] : 0;
        const char *unit;
        for (int i = 0; i < B.n_chrs; i++) {
            if (B.chrs[i].val == ev->notify_rx.attr_handle && B.chrs[i].uuid.u.type == BLE_UUID_TYPE_16)
                ble_decode_known(ble_uuid_u16(&B.chrs[i].uuid.u), v, l, &val, &unit);
        }
        event_t e = {.type = EV_BLE_SEEN, .dev = (int8_t)B.peer, .value = (float)val, .ts = esp_timer_get_time() / 1000};
        event_engine_post(&e);
        return 0;
    }
    default:
        return 0;
    }
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &B.own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    B.synced = true;
    advertise();
    ESP_LOGI(TAG, "BLE ready (central + peripheral '%s')", ble_svc_gap_device_name());
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset, reason %d", reason); B.synced = false; }

static void host_task(void *p)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

int ble_mgr_init(void)
{
    B.op = xSemaphoreCreateMutex();
    B.done = xSemaphoreCreateBinary();
    if (!B.op || !B.done) return -1;
    esp_timer_create_args_t ta = {.callback = rc_timer_cb, .name = "ble_rc"};
    if (esp_timer_create(&ta, &B.rc_timer) != ESP_OK) return -1;
    allow_load();
    if (nimble_port_init() != ESP_OK) return -1;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(k_gatt) != 0 || ble_gatts_add_svcs(k_gatt) != 0) return -1;
    uint8_t mac[6];
    char name[20];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(name, sizeof(name), "llmagent-%02x%02x", mac[4], mac[5]);
    ble_svc_gap_device_name_set(name);
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return 0;
}

void ble_mgr_print_status(void)
{
    char a[18] = "-";
    if (B.conn != BLE_HS_CONN_HANDLE_NONE && B.peer >= 0) ble_addr_str(B.seen[B.peer].addr, a);
    printf("ble         : %s, %d seen, %d allowlisted, central link %s (%d svcs, %d chrs), peripheral %s, %u notifications\n",
           B.synced ? "ready" : "not ready", B.n_seen, B.n_allow, a, B.n_svcs, B.n_chrs,
           B.periph_conn != BLE_HS_CONN_HANDLE_NONE ? "connected" : "advertising", (unsigned)B.notifies);
    printf("              auto-reconnect %s, %u reconnects, %u subscriptions remembered\n",
           B.want_link ? (B.conn == BLE_HS_CONN_HANDLE_NONE ? "PENDING" : "armed") : "off", (unsigned)B.reconnects,
           B.n_sub);
}

/* used by console listing */
int ble_mgr_chr_count(void) { return B.n_chrs; }

void ble_mgr_chr_describe(int i, char *uuid, size_t cap, uint8_t *props)
{
    char tmp[BLE_UUID_STR_LEN];
    ble_uuid_to_str(&B.chrs[i].uuid.u, tmp);
    strlcpy(uuid, tmp, cap);
    *props = B.chrs[i].props;
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */

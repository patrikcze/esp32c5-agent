/* ble_tools.c — BLE agent tools and the /ble console command. */
#include "sdkconfig.h"
#if CONFIG_BT_NIMBLE_ENABLED
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_mgr.h"
#include "esp_timer.h"

int ble_mgr_chr_count(void);
void ble_mgr_chr_describe(int i, char *uuid, size_t cap, uint8_t *props);

static uint32_t left_ms(uint64_t dl)
{
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    return now >= dl ? 0 : (uint32_t)(dl - now);
}

static void fail(tool_result_t *r, int rc)
{
    tool_status_t st = rc == BLE_ERR_UNCERTAIN ? TOOL_ST_UNKNOWN
                       : (rc == BLE_ERR_NOT_ALLOWED || rc == BLE_ERR_NOT_WRITABLE || rc == BLE_ERR_ARG ||
                          rc == BLE_ERR_NOT_FOUND || rc == BLE_ERR_NOT_CONNECTED)
                           ? TOOL_ST_REJECTED
                       : rc == BLE_ERR_TIMEOUT ? TOOL_ST_TIMEOUT
                                               : TOOL_ST_ERROR;
    tool_result_fail(r, st, ble_err_str(rc), NULL);
}

/* Devices strongest-signal first, as many as fit; "shown"/"total" tell the
 * model when the list is partial (use a name_prefix filter to narrow it). */
static void write_seen(lla_jw_t *w)
{
    int n = ble_mgr_seen_count();
    int8_t order[BLE_MAX_SEEN];
    for (int i = 0; i < n; i++) order[i] = (int8_t)i;
    for (int i = 1; i < n; i++) { /* insertion sort by RSSI, n <= 16 */
        int8_t k = order[i];
        int j = i - 1;
        while (j >= 0 && ble_mgr_seen(order[j])->rssi < ble_mgr_seen(k)->rssi) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = k;
    }
    int shown = 0;
    lla_jw_key(w, "devices");
    lla_jw_arr_begin(w);
    for (int o = 0; o < n; o++) {
        int i = order[o];
        const ble_seen_t *s = ble_mgr_seen(i);
        char a[18];
        ble_addr_str(s->addr, a);
        lla_jw_t snap = *w;
        lla_jw_obj_begin(w);
        lla_jw_kv_int(w, "index", i);
        lla_jw_kv_str(w, "addr", a);
        if (s->name[0]) lla_jw_kv_str(w, "name", s->name);
        lla_jw_kv_int(w, "rssi", s->rssi);
        bool rw = false;
        if (ble_mgr_allowed(s->addr, &rw)) lla_jw_kv_bool(w, "authorized", true);
        if (s->ess) lla_jw_kv_bool(w, "env_sensing", true);
        lla_jw_obj_end(w);
        if (!lla_jw_room(w, 64)) {
            lla_jw_rollback(w, &snap);
            break;
        }
        shown++;
    }
    lla_jw_arr_end(w);
    lla_jw_kv_int(w, "shown", shown);
    lla_jw_kv_int(w, "total", n);
}

static const tool_arg_def_t A_SCAN[] = {
    {.name = "seconds", .type = TARG_INT, .required = false, .min = 1, .max = 10},
    {.name = "name_prefix", .type = TARG_STR, .required = false, .max_len = 16}};

static void x_scan(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    uint32_t ms = (uint32_t)tool_arg_int(d, a, "seconds", 3) * 1000;
    if (ms + 500 > left_ms(dl)) ms = left_ms(dl) > 1500 ? left_ms(dl) - 1000 : 500;
    int n = ble_mgr_scan(ms, tool_arg_str(d, a, "name_prefix", ""));
    if (n < 0) {
        fail(r, n);
        return;
    }
    write_seen(&r->w);
}

static void x_list(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)a; (void)dl;
    lla_jw_kv_bool(&r->w, "connected", ble_mgr_connected());
    write_seen(&r->w);
}

static const tool_arg_def_t A_IDX[] = {{.name = "index", .desc = "index from ble_list_devices", .type = TARG_INT,
                                        .required = true, .min = 0, .max = BLE_MAX_SEEN - 1}};

static void x_connect(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    uint32_t left = left_ms(dl);
    int rc = ble_mgr_connect((int)tool_arg_int(d, a, "index", -1), left > 1000 ? left - 1000 : left);
    if (rc != 0) {
        fail(r, rc);
        return;
    }
    lla_jw_kv_bool(&r->w, "connected", true);
}

static void x_disconnect(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)a; (void)dl;
    int rc = ble_mgr_disconnect();
    if (rc != 0 && rc != BLE_ERR_NOT_CONNECTED) {
        fail(r, rc);
        return;
    }
    lla_jw_kv_bool(&r->w, "connected", false);
}

static void x_discover(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)a;
    int n = ble_mgr_discover(left_ms(dl));
    if (n < 0) {
        fail(r, n);
        return;
    }
    lla_jw_key(&r->w, "characteristics");
    lla_jw_arr_begin(&r->w);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        char u[40];
        uint8_t p;
        ble_mgr_chr_describe(i, u, sizeof(u), &p);
        lla_jw_t snap = r->w;
        lla_jw_obj_begin(&r->w);
        lla_jw_kv_str(&r->w, "uuid", u);
        char props[8] = "";
        if (p & 0x02) strcat(props, "r");
        if (p & 0x08) strcat(props, "w");
        if (p & 0x10) strcat(props, "n");
        if (p & 0x20) strcat(props, "i");
        lla_jw_kv_str(&r->w, "props", props);
        lla_jw_obj_end(&r->w);
        if (!lla_jw_room(&r->w, 64)) {
            lla_jw_rollback(&r->w, &snap);
            break;
        }
        shown++;
    }
    lla_jw_arr_end(&r->w);
    lla_jw_kv_int(&r->w, "shown", shown);
    lla_jw_kv_int(&r->w, "total", n);
}

static const tool_arg_def_t A_UUID[] = {{.name = "uuid", .desc = "e.g. \"2a6e\" or 128-bit", .type = TARG_STR,
                                         .required = true, .max_len = 36}};

static void x_read(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    uint8_t buf[64];
    const char *u = tool_arg_str(d, a, "uuid", "");
    int n = ble_mgr_read(u, buf, sizeof(buf), left_ms(dl));
    if (n < 0) {
        fail(r, n);
        return;
    }
    char hex[129];
    for (int i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", buf[i]);
    hex[2 * n] = '\0';
    lla_jw_kv_str(&r->w, "hex", hex);
    double v;
    const char *unit;
    if (strlen(u) == 4 && ble_decode_known((uint16_t)strtol(u, NULL, 16), buf, (size_t)n, &v, &unit)) {
        lla_jw_kv_num(&r->w, "value", v);
        lla_jw_kv_str(&r->w, "unit", unit);
    }
}

static const tool_arg_def_t A_WRITE[] = {
    {.name = "uuid", .type = TARG_STR, .required = true, .max_len = 36},
    {.name = "hex", .type = TARG_STR, .required = true, .max_len = 40}};

static void x_write(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    const char *hex = tool_arg_str(d, a, "hex", "");
    size_t hl = strlen(hex);
    uint8_t buf[20];
    if (hl == 0 || hl % 2) {
        tool_result_fail(r, TOOL_ST_REJECTED, "invalid_arguments", "even number of hex digits required");
        return;
    }
    for (size_t i = 0; i < hl / 2; i++) {
        if (!isxdigit((unsigned char)hex[2 * i]) || !isxdigit((unsigned char)hex[2 * i + 1])) {
            tool_result_fail(r, TOOL_ST_REJECTED, "invalid_arguments", "non-hex digit");
            return;
        }
        char t[3] = {hex[2 * i], hex[2 * i + 1], 0};
        buf[i] = (uint8_t)strtol(t, NULL, 16);
    }
    int rc = ble_mgr_write(tool_arg_str(d, a, "uuid", ""), buf, hl / 2, left_ms(dl));
    if (rc != 0) {
        fail(r, rc);
        return;
    }
    lla_jw_kv_int(&r->w, "written", (int64_t)(hl / 2));
}

static void x_subscribe(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    int rc = ble_mgr_subscribe(tool_arg_str(d, a, "uuid", ""), left_ms(dl));
    if (rc != 0) {
        fail(r, rc);
        return;
    }
    lla_jw_kv_bool(&r->w, "subscribed", true);
    lla_jw_kv_str(&r->w, "note", "notifications arrive as ble_seen events");
}

static const tool_def_t k_tools[] = {
    {.name = "ble_scan", .desc = "Scan for BLE advertisers (bounded, max 10 s).", .args = A_SCAN, .n_args = 2,
     .perm = TOOL_PERM_READ, .timeout_ms = 11500, .exec = x_scan},
    {.name = "ble_list_devices", .desc = "Devices from the last scan with authorization state.", .perm = TOOL_PERM_READ,
     .timeout_ms = 100, .exec = x_list},
    {.name = "ble_connect", .desc = "Connect to an allowlisted device by scan index.", .args = A_IDX, .n_args = 1,
     .perm = TOOL_PERM_ACTUATE, .effect = TOOL_EFFECT_IDEMPOTENT, .timeout_ms = 10000, .exec = x_connect},
    {.name = "ble_disconnect", .desc = "Close the BLE central connection.", .perm = TOOL_PERM_ACTUATE,
     .effect = TOOL_EFFECT_IDEMPOTENT, .timeout_ms = 4000, .exec = x_disconnect},
    {.name = "ble_discover_services", .desc = "Discover GATT characteristics of the connected device.",
     .perm = TOOL_PERM_READ, .timeout_ms = 8000, .exec = x_discover},
    {.name = "ble_read_characteristic", .desc = "Read a characteristic; standard sensor UUIDs are decoded.",
     .args = A_UUID, .n_args = 1, .perm = TOOL_PERM_READ, .timeout_ms = 3000, .exec = x_read},
    {.name = "ble_write_characteristic", .desc = "Write to a device allowlisted as writable.", .args = A_WRITE,
     .n_args = 2, .perm = TOOL_PERM_ACTUATE, .effect = TOOL_EFFECT_NON_IDEMPOTENT, .timeout_ms = 3000, .exec = x_write},
    {.name = "ble_subscribe_notifications", .desc = "Enable notifications for a characteristic.", .args = A_UUID,
     .n_args = 1, .perm = TOOL_PERM_ACTUATE, .effect = TOOL_EFFECT_IDEMPOTENT, .timeout_ms = 3000, .exec = x_subscribe},
};

int ble_tools_register(tool_registry_t *reg)
{
    for (size_t i = 0; i < sizeof(k_tools) / sizeof(k_tools[0]); i++) {
        if (tool_registry_add(reg, &k_tools[i]) < 0) return -1;
    }
    return (int)(sizeof(k_tools) / sizeof(k_tools[0]));
}

/* ------------------------------------------------------------ console */

int console_ble_cmd(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "status";
    if (!strcmp(c, "scan")) {
        int secs = argc > 2 ? atoi(argv[2]) : 3;
        int n = ble_mgr_scan((uint32_t)(secs > 0 ? secs : 3) * 1000, argc > 3 ? argv[3] : "");
        if (n < 0) printf("scan failed: %s\n", ble_err_str(n));
        c = "list";
    }
    if (!strcmp(c, "list")) {
        for (int i = 0; i < ble_mgr_seen_count(); i++) {
            const ble_seen_t *s = ble_mgr_seen(i);
            char a[18];
            bool rw = false;
            ble_addr_str(s->addr, a);
            bool al = ble_mgr_allowed(s->addr, &rw);
            printf("  [%2d] %s rssi %4d %-20s %s%s\n", i, a, s->rssi, s->name, al ? (rw ? "allowed-rw " : "allowed ") : "",
                   s->ess ? "ESS" : "");
        }
        return 0;
    }
    if ((!strcmp(c, "allow") || !strcmp(c, "deny")) && argc > 2) {
        uint8_t a[6];
        if (!ble_addr_parse(argv[2], a)) {
            printf("address format aa:bb:cc:dd:ee:ff\n");
            return 1;
        }
        int rc = !strcmp(c, "allow") ? ble_mgr_allow(a, argc > 3 && !strcmp(argv[3], "rw")) : ble_mgr_deny(a);
        printf("%s\n", rc == 0 ? "ok (stored)" : ble_err_str(rc));
        return rc != 0;
    }
    if (!strcmp(c, "connect") && argc > 2) {
        int rc = ble_mgr_connect(atoi(argv[2]), 8000);
        printf("%s\n", ble_err_str(rc));
        return rc != 0;
    }
    if (!strcmp(c, "disconnect")) {
        printf("%s\n", ble_err_str(ble_mgr_disconnect()));
        return 0;
    }
    if (!strcmp(c, "services")) {
        int n = ble_mgr_discover(8000);
        if (n < 0) {
            printf("%s\n", ble_err_str(n));
            return 1;
        }
        for (int i = 0; i < n; i++) {
            char u[40];
            uint8_t p;
            ble_mgr_chr_describe(i, u, sizeof(u), &p);
            printf("  %-38s props 0x%02x\n", u, p);
        }
        return 0;
    }
    if ((!strcmp(c, "read") || !strcmp(c, "sub")) && argc > 2) {
        if (!strcmp(c, "sub")) {
            printf("%s\n", ble_err_str(ble_mgr_subscribe(argv[2], 3000)));
            return 0;
        }
        uint8_t b[64];
        int n = ble_mgr_read(argv[2], b, sizeof(b), 3000);
        if (n < 0) {
            printf("%s\n", ble_err_str(n));
            return 1;
        }
        printf("  ");
        for (int i = 0; i < n; i++) printf("%02x", b[i]);
        double v;
        const char *unit;
        if (strlen(argv[2]) == 4 && ble_decode_known((uint16_t)strtol(argv[2], NULL, 16), b, (size_t)n, &v, &unit))
            printf("  = %.2f %s", v, unit);
        printf("\n");
        return 0;
    }
    ble_mgr_print_status();
    return 0;
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */

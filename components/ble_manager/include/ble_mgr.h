/*
 * ble_mgr.h — Bluetooth LE (NimBLE) central + peripheral for llmagent.
 *
 * ESP32-C5 supports Bluetooth LE only; there is no Bluetooth Classic.
 *
 * Central: bounded scans, an address allowlist (persisted) that gates
 * connect, and per-device write authorization. GATT reads/writes go by
 * characteristic UUID on the single active central connection.
 *
 * Peripheral: one custom GATT service with
 *   status        read (no encryption)          short JSON status
 *   capabilities  read, encrypted               device list
 *   request       write, encrypted+MITM         queues an agent request
 *                                               (source "ble", READ ceiling)
 * Pairing uses passkey display: the 6-digit key is printed on the console.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_MAX_SEEN 16
#define BLE_MAX_ALLOW 4

typedef struct {
    uint8_t addr[6];   /* little-endian as NimBLE */
    uint8_t addr_type;
    char name[20];
    int8_t rssi;
    uint64_t last_seen_ms;
    bool ess;          /* advertises Environmental Sensing 0x181A */
    bool battery;      /* advertises Battery 0x180F */
} ble_seen_t;

int ble_mgr_init(void);
/** Blocking scan for @p ms (<= 10000). Returns devices found or <0. */
int ble_mgr_scan(uint32_t ms, const char *name_prefix);
int ble_mgr_seen_count(void);
const ble_seen_t *ble_mgr_seen(int i);
void ble_addr_str(const uint8_t a[6], char out[18]);
bool ble_addr_parse(const char *s, uint8_t a[6]);

int ble_mgr_allow(const uint8_t addr[6], bool writable);
int ble_mgr_deny(const uint8_t addr[6]);
bool ble_mgr_allowed(const uint8_t addr[6], bool *writable);

int ble_mgr_connect(int seen_idx, uint32_t timeout_ms);
int ble_mgr_disconnect(void);
bool ble_mgr_connected(void);
int ble_mgr_discover(uint32_t timeout_ms);
/** Read by UUID string ("2a6e" or 128-bit). Returns length or <0. */
int ble_mgr_read(const char *uuid, uint8_t *out, size_t cap, uint32_t timeout_ms);
int ble_mgr_write(const char *uuid, const uint8_t *d, size_t n, uint32_t timeout_ms);
int ble_mgr_subscribe(const char *uuid, uint32_t timeout_ms);

/** Decode well-known characteristics (device profile). Returns false if unknown. */
bool ble_decode_known(uint16_t uuid16, const uint8_t *d, size_t n, double *value, const char **unit);

const char *ble_err_str(int e);
void ble_mgr_print_status(void);
int ble_tools_register(tool_registry_t *reg);

enum {
    BLE_ERR_NOT_READY = -200,
    BLE_ERR_BUSY,
    BLE_ERR_TIMEOUT,
    BLE_ERR_NOT_ALLOWED,
    BLE_ERR_NOT_CONNECTED,
    BLE_ERR_NOT_FOUND,
    BLE_ERR_GATT,
    BLE_ERR_ARG,
    BLE_ERR_NOT_WRITABLE,
    BLE_ERR_UNCERTAIN,
};

#ifdef __cplusplus
}
#endif

/*
 * net.h — Wi-Fi station state machine, SNTP, HTTPS transport and OTA.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "llm_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NET_NO_CREDENTIALS = 0, NET_CONNECTING, NET_CONNECTED, NET_BACKOFF, NET_STOPPED } net_state_t;

typedef struct {
    net_state_t state;
    char ssid[33];
    char ip[16];
    int8_t rssi;
    uint8_t channel;
    bool time_synced;
    uint32_t connects;
    uint32_t disconnects;
    uint32_t last_reconnect_ms;   /* disconnect → got IP, last occurrence */
    uint32_t backoff_ms;          /* current backoff when in NET_BACKOFF */
    uint8_t last_reason;          /* wifi_err_reason_t of the last disconnect */
    /* transport statistics (measured) */
    uint32_t http_requests;
    uint32_t http_conn_reused;
    uint32_t last_connect_ms;     /* TCP+TLS establishment time of the last new connection */
    uint32_t max_connect_ms;
    uint32_t tls_failures;
} net_status_t;

typedef void (*net_event_cb_t)(bool up);

/** Initialise netif/event loop/Wi-Fi and connect with stored credentials. */
int net_init(net_event_cb_t cb);
/** Store credentials (secret namespace) and reconnect. */
int net_set_credentials(const char *ssid, const char *pass);
/** Wait until associated with IP and, if @p need_time, SNTP synced. */
bool net_wait_ready(uint32_t wait_ms, bool need_time);
void net_get_status(net_status_t *st);
const char *net_state_str(net_state_t s);
/** Force a disconnect (used by the hardware recovery test). */
void net_force_disconnect(void);
/** Scan visible APs and print them (console helper). */
int net_scan_print(void);

/** Shared HTTP(S) transport used by all LLM providers. */
llm_transport_t *net_transport(void);
/** Load a PEM trust anchor (private CA for local servers). NULL clears. */
int net_set_ca_pem(const char *pem);
bool net_has_custom_ca(void);

/** OTA from an https URL; reboots on success. */
int net_ota(const char *url);

#ifdef __cplusplus
}
#endif

/*
 * wifi_esp.c — Wi-Fi station with bounded exponential backoff and SNTP.
 *
 * States: NO_CREDENTIALS → CONNECTING → CONNECTED
 *                              ↑   ↓ (disconnect / auth fail)
 *                              BACKOFF (1 s doubling to 60 s, ±25 % jitter)
 */
#include <string.h>
#include <time.h>

#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "net.h"

static const char *TAG = "net";

#define BIT_UP BIT0
#define BIT_TIME BIT1
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 60000

static EventGroupHandle_t s_eg;
static esp_timer_handle_t s_retry_timer;
static net_status_t s_st;
static net_event_cb_t s_cb;
static uint64_t s_down_since_us;
static uint32_t s_backoff = BACKOFF_MIN_MS;
static bool s_inited;
static volatile bool s_prov; /* provisioning manager owns connection attempts */

void net_internal_set_prov(bool active)
{
    s_prov = active;
    if (active) esp_timer_stop(s_retry_timer);
}

const char *net_state_str(net_state_t s)
{
    static const char *const n[] = {"no_credentials", "connecting", "connected", "backoff", "stopped"};
    return (unsigned)s < 5 ? n[s] : "?";
}

static void retry_cb(void *arg)
{
    (void)arg;
    s_st.state = NET_CONNECTING;
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK) ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(e));
}

static void schedule_retry(void)
{
    uint32_t jitter = esp_random() % (s_backoff / 2 + 1);
    uint32_t d = s_backoff - s_backoff / 4 + jitter; /* ±25 % */
    s_st.state = NET_BACKOFF;
    s_st.backoff_ms = d;
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)d * 1000);
    s_backoff = s_backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff * 2;
}

static void time_synced(struct timeval *tv)
{
    (void)tv;
    s_st.time_synced = true;
    xEventGroupSetBits(s_eg, BIT_TIME);
    ESP_LOGI(TAG, "SNTP time synchronised");
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_st.ssid[0] && !s_prov) {
            s_st.state = NET_CONNECTING;
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        s_st.last_reason = d->reason;
        bool was_up = xEventGroupGetBits(s_eg) & BIT_UP;
        xEventGroupClearBits(s_eg, BIT_UP);
        s_st.ip[0] = '\0';
        if (was_up) {
            s_st.disconnects++;
            s_down_since_us = esp_timer_get_time();
            if (s_cb) s_cb(false);
        }
        if (s_prov) {
            s_st.state = NET_CONNECTING; /* provisioning manager owns the link */
            return;
        }
        if (s_st.state == NET_STOPPED || !s_st.ssid[0]) return;
        ESP_LOGW(TAG, "disconnected (reason %d), retry in ~%u ms", d->reason, (unsigned)s_backoff);
        schedule_retry();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        esp_ip4addr_ntoa(&ev->ip_info.ip, s_st.ip, sizeof(s_st.ip));
        s_st.state = NET_CONNECTED;
        s_st.connects++;
        s_backoff = BACKOFF_MIN_MS;
        if (s_down_since_us) {
            s_st.last_reconnect_ms = (uint32_t)((esp_timer_get_time() - s_down_since_us) / 1000);
            ESP_LOGI(TAG, "reconnected in %u ms", (unsigned)s_st.last_reconnect_ms);
            s_down_since_us = 0;
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_st.rssi = ap.rssi;
            s_st.channel = ap.primary;
        }
        ESP_LOGI(TAG, "connected: ip %s ch %u rssi %d", s_st.ip, s_st.channel, s_st.rssi);
        xEventGroupSetBits(s_eg, BIT_UP);
        if (s_cb) s_cb(true);
    }
}

static esp_err_t apply_config(const char *ssid, const char *pass)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof(wc)); /* do not leave the password on the stack */
    return e;
}

int net_init(net_event_cb_t cb)
{
    if (s_inited) return 0;
    s_cb = cb;
    s_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return -1;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); /* credentials live in our secret namespace */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    esp_timer_create_args_t ta = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_retry_timer));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    char pass[65];
    cfg_get_str(CFG_NS, "wifi_ssid", s_st.ssid, sizeof(s_st.ssid));
    cfg_get_str(CFG_NS_SECRET, "wifi_pass", pass, sizeof(pass));
    if (s_st.ssid[0]) apply_config(s_st.ssid, pass);
    memset(pass, 0, sizeof(pass));
    s_st.state = s_st.ssid[0] ? NET_CONNECTING : NET_NO_CREDENTIALS;
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Modem sleep is mandatory when Wi-Fi and BLE coexist. */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    /* two servers: pool.ntp.org alone took ~30 s to answer on some networks */
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("time.google.com", "pool.ntp.org"));
    sc.sync_cb = time_synced;
    esp_netif_sntp_init(&sc);
    s_inited = true;
    if (!s_st.ssid[0]) ESP_LOGW(TAG, "no Wi-Fi credentials: use /wifi set <ssid> <password>");
    return 0;
}

/* used by provisioning: persist and connect without the extra validation */
int net_internal_apply_credentials(const char *ssid, const char *pass)
{
    if (cfg_set_str(CFG_NS, "wifi_ssid", ssid) < 0 || cfg_set_str(CFG_NS_SECRET, "wifi_pass", pass) < 0) return -2;
    strlcpy(s_st.ssid, ssid, sizeof(s_st.ssid));
    esp_timer_stop(s_retry_timer);
    s_backoff = BACKOFF_MIN_MS;
    if (apply_config(ssid, pass) != ESP_OK) return -3;
    s_st.state = NET_CONNECTING;
    return esp_wifi_connect() == ESP_OK ? 0 : -3;
}

int net_set_credentials(const char *ssid, const char *pass)
{
    if (strlen(ssid) == 0 || strlen(ssid) > 32 || strlen(pass) > 64 || (pass[0] && strlen(pass) < 8)) return -1;
    if (cfg_set_str(CFG_NS, "wifi_ssid", ssid) < 0) return -2;
    if (cfg_set_str(CFG_NS_SECRET, "wifi_pass", pass) < 0) return -2;
    strlcpy(s_st.ssid, ssid, sizeof(s_st.ssid));
    esp_timer_stop(s_retry_timer);
    s_backoff = BACKOFF_MIN_MS;
    esp_wifi_disconnect();
    if (apply_config(ssid, pass) != ESP_OK) return -3;
    s_st.state = NET_CONNECTING;
    return esp_wifi_connect() == ESP_OK ? 0 : -3;
}

bool net_wait_ready(uint32_t wait_ms, bool need_time)
{
    if (!s_inited) return false;
    EventBits_t want = BIT_UP | (need_time ? BIT_TIME : 0);
    EventBits_t b = xEventGroupWaitBits(s_eg, want, pdFALSE, pdTRUE, pdMS_TO_TICKS(wait_ms));
    if ((b & BIT_TIME) == 0 && time(NULL) > 1735689600) { /* RTC already valid (after 2025-01-01) */
        s_st.time_synced = true;
        xEventGroupSetBits(s_eg, BIT_TIME);
        b |= BIT_TIME;
    }
    return (b & want) == want;
}

void net_get_status(net_status_t *st)
{
    if (s_st.state == NET_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_st.rssi = ap.rssi;
    }
    *st = s_st;
}

/* exported for the transport (same component) */
net_status_t *net_stats_mut(void) { return &s_st; }

void net_force_disconnect(void) { esp_wifi_disconnect(); }

int net_scan_print(void)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return -1;
    uint16_t n = 12;
    static wifi_ap_record_t recs[12];
    esp_wifi_scan_get_ap_records(&n, recs);
    for (uint16_t i = 0; i < n; i++) {
        printf("  %-32s ch%-3u rssi %-4d %s\n", (char *)recs[i].ssid, recs[i].primary, recs[i].rssi,
               recs[i].authmode == WIFI_AUTH_OPEN ? "open" : "secured");
    }
    return n;
}

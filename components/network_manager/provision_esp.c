/*
 * provision_esp.c — headless Wi-Fi provisioning (ESP-IDF wifi_provisioning,
 * SoftAP scheme, protocomm security 2 = SRP6a with a proof-of-possession).
 *
 * The device opens an AP "PROV_xxxxxx"; the Espressif "ESP SoftAP
 * Provisioning" app (or tools/esp_prov/esp_prov.py) connects, proves the PoP
 * with SRP6a (username "wifiprov", password = PoP; AES-GCM session) and
 * sends SSID/password. SRP6a means a recorded session does not allow
 * offline guessing of the PoP, unlike security 1.
 *
 * The SoftAP scheme is used instead of the BLE scheme because the BLE scheme
 * initialises NimBLE itself, which would conflict with ble_manager.
 *
 * Secrets: the manager stores received credentials in the Wi-Fi driver's
 * own NVS area. After success we move them to our secret namespace and
 * overwrite the driver's flash copy with an empty config, so the password
 * exists in exactly one place.
 *
 * Started when no credentials are stored, by /wifi prov, or by holding the
 * BOOT button for 5 s. Stops itself after 10 minutes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#if CONFIG_LLA_WIFI_PROV
#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_srp.h"
#include "esp_wifi.h"
#include "net.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"

static const char *TAG = "prov";

#define PROV_TIMEOUT_US (10ULL * 60 * 1000 * 1000)
#define PROV_USER "wifiprov" /* default username of Espressif's provisioning apps */
#define POP_LEN 12           /* 31-symbol alphabet: ~59 bits */

void net_internal_set_prov(bool active);
int net_internal_apply_credentials(const char *ssid, const char *pass);

static volatile bool s_active;
static bool s_handler;
static esp_timer_handle_t s_timeout;
static char s_ssid[33], s_pass[65];
static bool s_got_creds;
static char s_service[16];
static char *s_salt, *s_verifier;
static wifi_prov_security2_params_t s_sec2;

static void pop_get(char *pop, size_t cap)
{
    if (cfg_get_str(CFG_NS_SECRET, "prov_pop", pop, cap) == CFG_OK && strlen(pop) >= POP_LEN) return;
    static const char cs[] = "abcdefghjkmnpqrstuvwxyz23456789"; /* no look-alike characters */
    for (int i = 0; i < POP_LEN; i++) pop[i] = cs[esp_random() % (sizeof(cs) - 1)];
    pop[POP_LEN] = '\0';
    cfg_set_str(CFG_NS_SECRET, "prov_pop", pop);
}

static void service_name(char *out, size_t cap)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static void free_sec2(void)
{
    free(s_salt);
    free(s_verifier);
    s_salt = s_verifier = NULL;
}

static void finish_with_credentials(void)
{
    /* Scrub the copy the manager wrote to the driver's NVS area. */
    wifi_config_t empty = {0};
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    esp_wifi_set_config(WIFI_IF_STA, &empty);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    int rc = net_internal_apply_credentials(s_ssid, s_pass);
    ESP_LOGI(TAG, "credentials for '%s' stored in the secret namespace (%s)", s_ssid, rc == 0 ? "ok" : "FAILED");
    memset(s_pass, 0, sizeof(s_pass));
}

static void on_prov(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_PROV_START:
        ESP_LOGI(TAG, "provisioning started");
        break;
    case WIFI_PROV_CRED_RECV: {
        wifi_sta_config_t *c = data;
        strlcpy(s_ssid, (const char *)c->ssid, sizeof(s_ssid));
        strlcpy(s_pass, (const char *)c->password, sizeof(s_pass));
        s_got_creds = false;
        ESP_LOGI(TAG, "received credentials for '%s' (password hidden)", s_ssid);
        break;
    }
    case WIFI_PROV_CRED_FAIL: {
        wifi_prov_sta_fail_reason_t *r = data;
        ESP_LOGW(TAG, "connection with received credentials failed: %s",
                 *r == WIFI_PROV_STA_AUTH_ERROR ? "authentication error" : "access point not found");
        memset(s_pass, 0, sizeof(s_pass));
        wifi_prov_mgr_reset_sm_state_on_failure(); /* allow the app to retry */
        break;
    }
    case WIFI_PROV_CRED_SUCCESS:
        s_got_creds = true;
        ESP_LOGI(TAG, "provisioning successful");
        break;
    case WIFI_PROV_END:
        esp_timer_stop(s_timeout);
        wifi_prov_mgr_deinit();
        break;
    case WIFI_PROV_DEINIT:
        if (s_got_creds) {
            finish_with_credentials();
        } else {
            /* The manager dropped the station link when it started; go back
             * to the previously stored network, if any. */
            memset(s_pass, 0, sizeof(s_pass));
            esp_wifi_set_storage(WIFI_STORAGE_RAM);
            esp_wifi_set_mode(WIFI_MODE_STA);
            char ssid[33], pass[65];
            if (cfg_get_str(CFG_NS, "wifi_ssid", ssid, sizeof(ssid)) == CFG_OK && ssid[0]) {
                cfg_get_str(CFG_NS_SECRET, "wifi_pass", pass, sizeof(pass));
                net_internal_set_prov(false);
                net_internal_apply_credentials(ssid, pass);
                memset(pass, 0, sizeof(pass));
                ESP_LOGI(TAG, "restored previous network '%s'", ssid);
            }
        }
        free_sec2();
        s_active = false;
        net_internal_set_prov(false);
        ESP_LOGI(TAG, "provisioning service stopped");
        break;
    default:
        break;
    }
}

static void on_timeout(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "no provisioning within 10 minutes; stopping the access point");
    wifi_prov_mgr_stop_provisioning();
}

bool net_prov_active(void) { return s_active; }

void net_prov_print_info(void)
{
    char pop[16];
    pop_get(pop, sizeof(pop));
    char svc[16];
    service_name(svc, sizeof(svc));
    printf("provisioning %s\n", s_active ? "ACTIVE" : "inactive");
    printf("  access point : %s (open; the session is SRP6a-authenticated and encrypted)\n", svc);
    printf("  username     : %s\n", PROV_USER);
    printf("  PoP          : %s\n", pop);
    printf("  app          : Espressif \"ESP SoftAP Provisioning\" (iOS/Android)\n");
    printf("  QR payload   : {\"ver\":\"v1\",\"name\":\"%s\",\"username\":\"%s\",\"pop\":\"%s\",\"transport\":\"softap\"}\n",
           svc, PROV_USER, pop);
    memset(pop, 0, sizeof(pop));
}

int net_prov_start(void)
{
    if (s_active) return 0;
    if (!s_handler) {
        if (esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, on_prov, NULL) != ESP_OK) return -1;
        esp_timer_create_args_t ta = {.callback = on_timeout, .name = "prov_to"};
        if (esp_timer_create(&ta, &s_timeout) != ESP_OK) return -1;
        s_handler = true;
    }
    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    if (wifi_prov_mgr_init(cfg) != ESP_OK) return -2;
    net_internal_set_prov(true);
    s_active = true;
    s_got_creds = false;
    char pop[16];
    pop_get(pop, sizeof(pop));
    service_name(s_service, sizeof(s_service));
    /* fresh salt + verifier per session; the PoP itself never goes on the air */
    int vlen = 0;
    esp_err_t e = esp_srp_gen_salt_verifier(PROV_USER, strlen(PROV_USER), pop, strlen(pop), &s_salt, 16, &s_verifier,
                                            &vlen);
    memset(pop, 0, sizeof(pop));
    if (e == ESP_OK) {
        s_sec2 = (wifi_prov_security2_params_t){.salt = s_salt, .salt_len = 16, .verifier = s_verifier,
                                                .verifier_len = (uint16_t)vlen};
        e = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_2, &s_sec2, s_service, NULL);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(e));
        wifi_prov_mgr_deinit();
        free_sec2();
        s_active = false;
        net_internal_set_prov(false);
        return -3;
    }
    esp_timer_start_once(s_timeout, PROV_TIMEOUT_US);
    net_prov_print_info();
    return 0;
}

void net_prov_stop(void)
{
    if (s_active) wifi_prov_mgr_stop_provisioning();
}

#else /* !CONFIG_LLA_WIFI_PROV */
#include "esp_log.h"
#include "net.h"

/* Provisioning compiled out: report it instead of pretending to provision. */
int net_prov_start(void)
{
    ESP_LOGW("prov", "provisioning not included in this build (CONFIG_LLA_WIFI_PROV); use /wifi set");
    return -1;
}
void net_prov_stop(void) {}
bool net_prov_active(void) { return false; }
void net_prov_print_info(void) { printf("provisioning not included in this build\n"); }
#endif

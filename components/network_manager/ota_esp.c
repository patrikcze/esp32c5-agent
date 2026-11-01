/*
 * ota_esp.c — HTTPS OTA. The server certificate is verified with the same
 * trust anchors as the LLM transport (private CA via /provider ca, else the
 * x509 bundle). With Secure Boot V2 enabled the new
 * image's signature is verified before it can boot; with
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the image must be marked valid by
 * app_main after a health check or the bootloader rolls back.
 */
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "net.h"

static const char *TAG = "ota";

const char *net_custom_ca(void);

int net_ota(const char *url)
{
    if (strncmp(url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "OTA requires https");
        return -1;
    }
    if (!net_wait_ready(10000, true)) {
        ESP_LOGE(TAG, "network/time not ready");
        return -2;
    }
    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    /* same trust anchors as the LLM transport: private CA if configured */
    if (net_custom_ca()) hc.cert_pem = net_custom_ca();
    else hc.crt_bundle_attach = esp_crt_bundle_attach;
    esp_https_ota_config_t oc = {.http_config = &hc};
    ESP_LOGI(TAG, "downloading %s", url);
    esp_err_t e = esp_https_ota(&oc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(e));
        return -3;
    }
    ESP_LOGI(TAG, "OTA written; rebooting into new image (pending verification)");
    esp_restart();
    return 0;
}

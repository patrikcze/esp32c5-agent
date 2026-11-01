/*
 * app_main.c — llmagent boot sequence.
 *
 * Order matters for safety: outputs reach their safe state (profile load)
 * before networking, BLE or the agent start, and a shutdown handler drives
 * them safe again on any software restart.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_service.h"
#include "config.h"
#include "console.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "event_engine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "periph.h"
#include "periph_tools.h"
#include "sdkconfig.h"
#include "system_tools.h"
#if CONFIG_SECURE_BOOT
#include "esp_secure_boot.h"
#endif
#include "esp_flash_encrypt.h"
#if CONFIG_LLA_ENABLE_BLE
#include "ble_mgr.h"
#endif

static const char *TAG = "main";

extern const char default_profile_start[] asm("_binary_default_profile_json_start");
extern const char default_profile_end[] asm("_binary_default_profile_json_end");

static tool_registry_t s_reg;

static void shutdown_safe(void) { periph_all_safe(); }

static void load_profile(void)
{
    char *js = malloc(4096); /* boot-only scratch; freed below */
    size_t n = 0;
    bool stored = js && cfg_get_str(CFG_NS, "profile", js, 4096) == CFG_OK;
    if (stored) n = strlen(js);
    char err[128];
    if (stored && periph_load_profile(js, n, err, sizeof(err)) == PER_OK &&
        event_engine_load_profile(js, n, err, sizeof(err)) == 0) {
        ESP_LOGI(TAG, "stored device profile applied");
        free(js);
        return;
    }
    free(js);
    if (stored) ESP_LOGE(TAG, "stored profile rejected (%s); using built-in default", err);
    n = strnlen(default_profile_start, (size_t)(default_profile_end - default_profile_start)); /* EMBED_TXTFILES adds a NUL */
    if (periph_load_profile(default_profile_start, n, err, sizeof(err)) != PER_OK ||
        event_engine_load_profile(default_profile_start, n, err, sizeof(err)) != 0) {
        ESP_LOGE(TAG, "built-in profile invalid: %s", err); /* caught by host tests; never expected */
    }
}

static void net_event(bool up)
{
    event_t e = {.type = up ? EV_NET_UP : EV_NET_DOWN, .dev = -1, .value = up, .ts = esp_timer_get_time() / 1000};
    event_engine_post(&e);
}

static bool net_ready(uint32_t wait_ms) { return net_wait_ready(wait_ms, false); }

static void boot_long_press(void)
{
    ESP_LOGI(TAG, "BOOT long press: starting Wi-Fi provisioning");
    net_prov_start();
}

static void agent_submit_from_event(const char *prompt, const event_t *e)
{
    (void)e;
    if (agent_service_busy()) {
        ESP_LOGW(TAG, "event trigger skipped: agent busy");
        return;
    }
    agent_service_submit(TOOL_SRC_AUTONOMOUS, prompt);
}

static void report_platform(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0, phys = 0;
    esp_flash_get_size(NULL, &flash);
    esp_flash_get_physical_size(NULL, &phys);
    ESP_LOGI(TAG, "ESP32-C5 rev %d.%d, %d core(s), flash %u MB physical (%u MB image), PSRAM %s %u KB", ci.revision / 100,
             ci.revision % 100, ci.cores, (unsigned)(phys >> 20), (unsigned)(flash >> 20), esp_psram_is_initialized() ? "present" : "absent",
             esp_psram_is_initialized() ? (unsigned)(esp_psram_get_size() >> 10) : 0);
    ESP_LOGI(TAG, "secure boot: %s, flash encryption: %s",
#if CONFIG_SECURE_BOOT
             esp_secure_boot_enabled() ? "ENABLED" : "configured, not active",
#else
             "disabled (development)",
#endif
             esp_flash_encryption_enabled() ? "ENABLED" : "disabled (development)");
    ESP_LOGI(TAG, "baseline heap: internal free %u, largest %u, PSRAM free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void ota_health_check(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
    /* Health criterion: control loop and console are up and Wi-Fi associates
     * within 60 s (or no credentials are configured). */
    net_status_t ns;
    net_get_status(&ns);
    bool ok = ns.state == NET_NO_CREDENTIALS || net_wait_ready(60000, false);
#if CONFIG_LLA_OTA_TEST_FAIL_HEALTH
    ESP_LOGW(TAG, "TEST BUILD: forcing health-check failure");
    ok = false;
#endif
    if (ok) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware marked valid");
    } else {
        ESP_LOGE(TAG, "health check failed: rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "llmagent starting");
    if (cfg_nvs_init() != CFG_OK) ESP_LOGE(TAG, "NVS unavailable: configuration will not persist");

    /* 1. hardware to safe state */
    hal_set(hal_esp_ops());
    periph_init(&board_esp32c5_devkitc1);
    esp_register_shutdown_handler(shutdown_safe);
    if (event_engine_start(agent_submit_from_event) != 0) ESP_LOGE(TAG, "event engine failed to start");
    load_profile();

#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = {.max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false};
    esp_err_t pe = esp_pm_configure(&pm);
    if (pe != ESP_OK) ESP_LOGW(TAG, "power management not configured: %s", esp_err_to_name(pe));
#endif
    report_platform();

    /* 2. tools + policy */
    tool_registry_init(&s_reg);
    system_tools_register(&s_reg);
    periph_tools_register(&s_reg);
#if CONFIG_LLA_ENABLE_BLE
    ble_tools_register(&s_reg);
#endif
    if (policy_load(&s_reg) == 0) ESP_LOGI(TAG, "stored tool policy applied");
    ESP_LOGI(TAG, "%u tools registered", s_reg.count);

    /* 3. networking */
    char *ca = malloc(3072);
    if (ca && cfg_get_str(CFG_NS, "ca_pem", ca, 3072) == CFG_OK) net_set_ca_pem(ca);
    free(ca);
    net_init(net_event);
#if CONFIG_LLA_ENABLE_BLE
    if (ble_mgr_init() != 0) ESP_LOGE(TAG, "BLE init failed");
#endif

    /* 4. agent + console */
    agent_service_env_t env = {.net_ready = net_ready};
    if (agent_service_start(&s_reg, &env) != 0) ESP_LOGE(TAG, "agent service failed to start");
    console_env_t cenv = {.default_profile = default_profile_start};
    console_start(&s_reg, &cenv);

    event_engine_set_long_press_cb(boot_long_press);
    net_status_t ns;
    net_get_status(&ns);
    if (ns.state == NET_NO_CREDENTIALS) {
        ESP_LOGI(TAG, "no Wi-Fi credentials: starting provisioning (or use /wifi set)");
        net_prov_start();
    }

    ota_health_check();
    ESP_LOGI(TAG, "ready: heap internal free %u, min %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

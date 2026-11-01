/* config_nvs_esp.c — NVS backend for config_core. */
#include "config.h"
#include "nvs.h"
#include "nvs_flash.h"

static int map(esp_err_t e)
{
    if (e == ESP_OK) return CFG_OK;
    if (e == ESP_ERR_NVS_NOT_FOUND) return CFG_NOT_FOUND;
    if (e == ESP_ERR_NVS_INVALID_LENGTH) return CFG_TOO_SMALL;
    return CFG_IO;
}

static int nv_get(const char *ns, const char *key, void *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READONLY, &h);
    if (e != ESP_OK) return map(e);
    e = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    return map(e);
}

static int nv_set(const char *ns, const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
    if (e != ESP_OK) return map(e);
    e = nvs_set_blob(h, key, buf, len);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return map(e);
}

static int nv_erase(const char *ns, const char *key)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
    if (e != ESP_OK) return map(e);
    e = nvs_erase_key(h, key);
    if (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) e = nvs_commit(h);
    nvs_close(h);
    return map(e);
}

static int nv_erase_ns(const char *ns)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
    if (e != ESP_OK) return e == ESP_ERR_NVS_NOT_FOUND ? CFG_OK : map(e);
    e = nvs_erase_all(h);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return map(e);
}

static const cfg_backend_t k_nvs = {nv_get, nv_set, nv_erase, nv_erase_ns};

const cfg_backend_t *cfg_nvs_backend(void) { return &k_nvs; }

int cfg_nvs_init(void)
{
    /* With CONFIG_NVS_ENCRYPTION + flash encryption, nvs_flash_init() uses
     * the encrypted NVS keys partition automatically (see docs/SECURITY.md). */
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) return CFG_IO;
    cfg_init(&k_nvs);
    return CFG_OK;
}

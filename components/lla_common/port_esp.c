/* port_esp.c — lla_port.h on ESP-IDF / FreeRTOS. */
#include <stdarg.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lla_port.h"

uint64_t lla_now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

void lla_sleep_ms(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

uint32_t lla_rand32(void) { return esp_random(); }

void lla_log(lla_log_level_t lvl, const char *tag, const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    switch (lvl) {
    case LLA_LOG_ERROR: ESP_LOGE(tag, "%s", buf); break;
    case LLA_LOG_WARN: ESP_LOGW(tag, "%s", buf); break;
    case LLA_LOG_INFO: ESP_LOGI(tag, "%s", buf); break;
    default: ESP_LOGD(tag, "%s", buf); break;
    }
}

void lla_lock_init(lla_lock_t *l)
{
    if (!l->impl) l->impl = xSemaphoreCreateRecursiveMutex();
    configASSERT(l->impl);
}

void lla_lock(lla_lock_t *l)
{
    if (!l->impl) lla_lock_init(l);
    xSemaphoreTakeRecursive((SemaphoreHandle_t)l->impl, portMAX_DELAY);
}

void lla_unlock(lla_lock_t *l) { xSemaphoreGiveRecursive((SemaphoreHandle_t)l->impl); }

/*
 * hal.h — hardware access vtable.
 *
 * All hardware access in llmagent goes through this table. The ESP-IDF
 * implementation is hal_esp.c (ESP32-C5 drivers: gpio, adc_oneshot+cali,
 * ledc, i2c_master, uart, rmt, temperature_sensor). Host tests install a
 * mock. Callers above this layer never see pin numbers chosen by the LLM:
 * the peripheral registry resolves logical device names to validated pins.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HAL_OK = 0,
    HAL_ERR_IO = -1,          /* operation failed, no effect */
    HAL_ERR_TIMEOUT = -2,     /* timed out before any effect */
    HAL_ERR_UNSUPPORTED = -3,
    HAL_ERR_ARG = -4,
    HAL_ERR_UNCERTAIN = -5,   /* failed part-way: effect unknown (e.g. bus write NACK/timeout) */
    HAL_ERR_NOMEM = -6,
};

typedef enum { HAL_PULL_NONE = 0, HAL_PULL_UP, HAL_PULL_DOWN } hal_pull_t;

typedef void (*hal_edge_cb_t)(int pin, void *u); /* called from ISR context */

typedef struct hal_ops {
    int (*gpio_out)(int pin, int level, bool open_drain); /* level applied before enabling output */
    int (*gpio_in)(int pin, hal_pull_t pull);
    int (*gpio_set)(int pin, int level);
    int (*gpio_get)(int pin, int *level);
    int (*gpio_release)(int pin);                         /* back to disabled/high-Z */
    int (*gpio_watch)(int pin, bool enable, hal_edge_cb_t cb, void *u);
    int (*adc_mv)(int pin, int *mv);
    int (*pwm_init)(int pin, int channel, uint32_t freq_hz, float duty_pct);
    int (*pwm_duty)(int channel, float duty_pct);
    int (*i2c_init)(int port, int sda, int scl, uint32_t hz);
    int (*i2c_xfer)(int port, uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn, uint32_t timeout_ms);
    int (*uart_init)(int port, int tx, int rx, uint32_t baud);
    int (*uart_write)(int port, const uint8_t *d, size_t n, uint32_t timeout_ms);
    int (*rgb_init)(int pin);
    int (*rgb_set)(int pin, uint8_t r, uint8_t g, uint8_t b);
    int (*chip_temp)(float *celsius);
} hal_ops_t;

/** Install the active HAL (ESP: hal_esp_ops(); tests: mock). */
void hal_set(const hal_ops_t *ops);
const hal_ops_t *hal(void);
const char *hal_err_str(int e);

#ifdef ESP_PLATFORM
const hal_ops_t *hal_esp_ops(void);
#endif

#ifdef __cplusplus
}
#endif

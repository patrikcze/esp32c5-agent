/*
 * hal_esp.c — ESP32-C5 implementation of hal.h using ESP-IDF v5.5 drivers:
 * gpio, adc_oneshot + curve-fitting calibration, ledc, i2c_master, uart,
 * rmt (WS2812 bytes encoder) and the internal temperature sensor.
 *
 * Driver objects are created once on first use and reused; no allocation
 * happens on the steady-state paths.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/rmt_tx.h"
#include "driver/temperature_sensor.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal.h"
#include "soc/soc_caps.h"

static const char *TAG = "hal";

static int map_err(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return HAL_OK;
    case ESP_ERR_TIMEOUT: return HAL_ERR_TIMEOUT;
    case ESP_ERR_INVALID_ARG: return HAL_ERR_ARG;
    case ESP_ERR_NO_MEM: return HAL_ERR_NOMEM;
    case ESP_ERR_NOT_SUPPORTED: return HAL_ERR_UNSUPPORTED;
    default: return HAL_ERR_IO;
    }
}

/* ------------------------------------------------------------ GPIO */

static int e_gpio_out(int pin, int level, bool od)
{
    /* Latch the level first so enabling the driver never glitches. */
    esp_err_t e = gpio_set_level(pin, level);
    if (e != ESP_OK) return map_err(e);
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode = od ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT_OUTPUT, /* input enabled for readback */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    e = gpio_config(&c);
    if (e == ESP_OK) e = gpio_set_level(pin, level);
    return map_err(e);
}

static int e_gpio_in(int pin, hal_pull_t pull)
{
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull == HAL_PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull == HAL_PULL_DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return map_err(gpio_config(&c));
}

static int e_gpio_set(int pin, int level) { return map_err(gpio_set_level(pin, level)); }

static int e_gpio_get(int pin, int *level)
{
    *level = gpio_get_level(pin);
    return HAL_OK;
}

static int e_gpio_release(int pin) { return map_err(gpio_reset_pin(pin)); }

typedef struct {
    hal_edge_cb_t cb;
    void *u;
} edge_slot_t;
static edge_slot_t s_edge[SOC_GPIO_PIN_COUNT];
static bool s_isr_installed;

static void IRAM_ATTR edge_isr(void *arg)
{
    int pin = (int)(intptr_t)arg;
    if (s_edge[pin].cb) s_edge[pin].cb(pin, s_edge[pin].u);
}

static int e_gpio_watch(int pin, bool en, hal_edge_cb_t cb, void *u)
{
    if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) return HAL_ERR_ARG;
    if (!s_isr_installed) {
        esp_err_t e = gpio_install_isr_service(0);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return map_err(e);
        s_isr_installed = true;
    }
    if (en) {
        s_edge[pin].cb = cb;
        s_edge[pin].u = u;
        esp_err_t e = gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE);
        if (e == ESP_OK) e = gpio_isr_handler_add(pin, edge_isr, (void *)(intptr_t)pin);
        if (e == ESP_OK) e = gpio_intr_enable(pin);
        return map_err(e);
    }
    gpio_intr_disable(pin);
    gpio_isr_handler_remove(pin);
    s_edge[pin].cb = NULL;
    return HAL_OK;
}

/* ------------------------------------------------------------ ADC */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali[SOC_ADC_MAX_CHANNEL_NUM];
static uint32_t s_adc_cfg_mask;

static int e_adc_mv(int pin, int *mv)
{
    adc_unit_t unit;
    adc_channel_t ch;
    esp_err_t e = adc_oneshot_io_to_channel(pin, &unit, &ch);
    if (e != ESP_OK || unit != ADC_UNIT_1) return HAL_ERR_ARG;
    if (!s_adc) {
        adc_oneshot_unit_init_cfg_t uc = {.unit_id = ADC_UNIT_1, .ulp_mode = ADC_ULP_MODE_DISABLE};
        e = adc_oneshot_new_unit(&uc, &s_adc);
        if (e != ESP_OK) return map_err(e);
    }
    if (!(s_adc_cfg_mask & (1u << ch))) {
        adc_oneshot_chan_cfg_t cc = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        e = adc_oneshot_config_channel(s_adc, ch, &cc);
        if (e != ESP_OK) return map_err(e);
        adc_cali_curve_fitting_config_t kc = {
            .unit_id = ADC_UNIT_1, .chan = ch, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        e = adc_cali_create_scheme_curve_fitting(&kc, &s_cali[ch]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ADC calibration unavailable on ch%d (%s)", ch, esp_err_to_name(e));
            return map_err(e); /* never report uncalibrated values as millivolts */
        }
        s_adc_cfg_mask |= 1u << ch;
    }
    int acc = 0;
    for (int i = 0; i < 8; i++) {
        int v;
        e = adc_oneshot_get_calibrated_result(s_adc, s_cali[ch], ch, &v);
        if (e != ESP_OK) return map_err(e);
        acc += v;
    }
    *mv = acc / 8;
    return HAL_OK;
}

/* ------------------------------------------------------------ LEDC PWM */

static uint32_t s_timer_freq[SOC_LEDC_TIMER_NUM];
static int8_t s_chan_timer[SOC_LEDC_CHANNEL_NUM];
#define PWM_RES LEDC_TIMER_10_BIT
#define PWM_MAX ((1u << 10) - 1)

static uint32_t duty_ticks(float pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint32_t)(pct * PWM_MAX / 100.0f + 0.5f);
}

static int e_pwm_init(int pin, int ch, uint32_t freq, float duty)
{
    if (ch < 0 || ch >= SOC_LEDC_CHANNEL_NUM) return HAL_ERR_ARG;
    int t = -1;
    for (int i = 0; i < SOC_LEDC_TIMER_NUM; i++) {
        if (s_timer_freq[i] == freq) t = i;
    }
    for (int i = 0; t < 0 && i < SOC_LEDC_TIMER_NUM; i++) {
        if (s_timer_freq[i] == 0) t = i;
    }
    if (t < 0) return HAL_ERR_NOMEM;
    if (s_timer_freq[t] == 0) {
        ledc_timer_config_t tc = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = PWM_RES, .timer_num = t,
                                  .freq_hz = freq, .clk_cfg = LEDC_AUTO_CLK};
        esp_err_t e = ledc_timer_config(&tc);
        if (e != ESP_OK) return map_err(e);
        s_timer_freq[t] = freq;
    }
    ledc_channel_config_t cc = {.gpio_num = pin, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = ch,
                                .intr_type = LEDC_INTR_DISABLE, .timer_sel = t, .duty = duty_ticks(duty), .hpoint = 0};
    s_chan_timer[ch] = (int8_t)t;
    return map_err(ledc_channel_config(&cc));
}

static int e_pwm_duty(int ch, float duty)
{
    esp_err_t e = ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty_ticks(duty));
    if (e == ESP_OK) e = ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
    return map_err(e);
}

/* ------------------------------------------------------------ I2C */

#define I2C_DEV_CACHE 8
static i2c_master_bus_handle_t s_i2c_bus;
static uint32_t s_i2c_hz;
static struct {
    uint8_t addr;
    i2c_master_dev_handle_t h;
} s_i2c_dev[I2C_DEV_CACHE];

static int e_i2c_init(int port, int sda, int scl, uint32_t hz)
{
    if (s_i2c_bus) return HAL_OK;
    i2c_master_bus_config_t bc = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, /* weak; use external 4.7k for real buses */
    };
    s_i2c_hz = hz;
    return map_err(i2c_new_master_bus(&bc, &s_i2c_bus));
}

static i2c_master_dev_handle_t i2c_dev(uint8_t addr)
{
    for (int i = 0; i < I2C_DEV_CACHE; i++) {
        if (s_i2c_dev[i].h && s_i2c_dev[i].addr == addr) return s_i2c_dev[i].h;
    }
    for (int i = 0; i < I2C_DEV_CACHE; i++) {
        if (!s_i2c_dev[i].h) {
            i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr,
                                      .scl_speed_hz = s_i2c_hz};
            if (i2c_master_bus_add_device(s_i2c_bus, &dc, &s_i2c_dev[i].h) != ESP_OK) return NULL;
            s_i2c_dev[i].addr = addr;
            return s_i2c_dev[i].h;
        }
    }
    return NULL;
}

static int e_i2c_xfer(int port, uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn, uint32_t to)
{
    (void)port;
    if (!s_i2c_bus) return HAL_ERR_IO;
    i2c_master_dev_handle_t d = i2c_dev(addr);
    if (!d) return HAL_ERR_NOMEM;
    esp_err_t e;
    if (wn && rn) e = i2c_master_transmit_receive(d, w, wn, r, rn, (int)to);
    else if (wn) e = i2c_master_transmit(d, w, wn, (int)to);
    else e = i2c_master_receive(d, r, rn, (int)to);
    return map_err(e);
}

/* ------------------------------------------------------------ UART */

static bool s_uart_inst[SOC_UART_HP_NUM];

static int e_uart_init(int port, int tx, int rx, uint32_t baud)
{
    if (port <= 0 || port >= SOC_UART_HP_NUM) return HAL_ERR_ARG; /* UART0 = console */
    if (!s_uart_inst[port]) {
        esp_err_t e = uart_driver_install(port, 256, 0, 0, NULL, 0);
        if (e != ESP_OK) return map_err(e);
        s_uart_inst[port] = true;
    }
    uart_config_t c = {.baud_rate = (int)baud, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                       .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                       .source_clk = UART_SCLK_DEFAULT};
    esp_err_t e = uart_param_config(port, &c);
    if (e == ESP_OK) e = uart_set_pin(port, tx, rx < 0 ? UART_PIN_NO_CHANGE : rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    return map_err(e);
}

static int e_uart_write(int port, const uint8_t *d, size_t n, uint32_t to)
{
    if (port <= 0 || port >= SOC_UART_HP_NUM || !s_uart_inst[port]) return HAL_ERR_ARG;
    int w = uart_write_bytes(port, d, n);
    if (w < 0) return HAL_ERR_IO;
    return map_err(uart_wait_tx_done(port, pdMS_TO_TICKS(to ? to : 1)));
}

/* ------------------------------------------------------------ WS2812 via RMT */

static rmt_channel_handle_t s_rmt;
static rmt_encoder_handle_t s_rmt_enc;
static int s_rgb_pin = -1;

static int e_rgb_init(int pin)
{
    if (s_rmt) return pin == s_rgb_pin ? HAL_OK : HAL_ERR_UNSUPPORTED; /* one strip supported */
    rmt_tx_channel_config_t tc = {
        .gpio_num = pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 0.1 us per tick */
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
        .trans_queue_depth = 2,
    };
    esp_err_t e = rmt_new_tx_channel(&tc, &s_rmt);
    if (e != ESP_OK) return map_err(e);
    rmt_bytes_encoder_config_t bc = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9}, /* T0H 0.3us, T0L 0.9us */
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3}, /* T1H 0.9us, T1L 0.3us */
        .flags.msb_first = 1,
    };
    e = rmt_new_bytes_encoder(&bc, &s_rmt_enc);
    if (e == ESP_OK) e = rmt_enable(s_rmt);
    if (e == ESP_OK) s_rgb_pin = pin;
    return map_err(e);
}

static int e_rgb_set(int pin, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_rmt || pin != s_rgb_pin) return HAL_ERR_ARG;
    static uint8_t grb[3];
    grb[0] = g;
    grb[1] = r;
    grb[2] = b;
    rmt_transmit_config_t tx = {.loop_count = 0};
    esp_err_t e = rmt_transmit(s_rmt, s_rmt_enc, grb, sizeof(grb), &tx);
    if (e == ESP_OK) e = rmt_tx_wait_all_done(s_rmt, 50);
    esp_rom_delay_us(80); /* WS2812 latch (>50 us low) */
    return map_err(e);
}

/* ------------------------------------------------------------ internal temperature */

static temperature_sensor_handle_t s_tsens;

static int e_chip_temp(float *c)
{
    if (!s_tsens) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        esp_err_t e = temperature_sensor_install(&cfg, &s_tsens);
        if (e == ESP_OK) e = temperature_sensor_enable(s_tsens);
        if (e != ESP_OK) {
            s_tsens = NULL;
            return map_err(e);
        }
    }
    return map_err(temperature_sensor_get_celsius(s_tsens, c));
}

static const hal_ops_t k_esp_hal = {
    .gpio_out = e_gpio_out,
    .gpio_in = e_gpio_in,
    .gpio_set = e_gpio_set,
    .gpio_get = e_gpio_get,
    .gpio_release = e_gpio_release,
    .gpio_watch = e_gpio_watch,
    .adc_mv = e_adc_mv,
    .pwm_init = e_pwm_init,
    .pwm_duty = e_pwm_duty,
    .i2c_init = e_i2c_init,
    .i2c_xfer = e_i2c_xfer,
    .uart_init = e_uart_init,
    .uart_write = e_uart_write,
    .rgb_init = e_rgb_init,
    .rgb_set = e_rgb_set,
    .chip_temp = e_chip_temp,
};

const hal_ops_t *hal_esp_ops(void) { return &k_esp_hal; }

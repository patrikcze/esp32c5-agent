/*
 * periph.h — board pin registry + logical device registry with local safety
 * enforcement (portable; hardware through hal.h).
 *
 * Safety properties enforced here, independent of any LLM:
 *   - only pins validated against the board table can be used
 *   - reserved pins (flash/PSRAM, USB, console UART) can never be assigned
 *   - strapping pins require explicit allow_strapping in the local profile
 *   - every output has a safe state applied at boot, on profile load,
 *     on shutdown and on fault
 *   - per-actuator max on-time watchdog and minimum toggle interval
 *   - actuators can be locked by deterministic safety rules; writes from the
 *     agent are then rejected
 *   - devices default to llm_write=false: actuation must be enabled locally
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal.h"
#include "lla_json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_GPIO_COUNT 29
#define PERIPH_MAX_DEVICES 16
#define PERIPH_NAME_MAX 16
#define PERIPH_I2C_WREGS 8

/* ------------------------------------------------------------- board */

enum { PCAP_IN = 1, PCAP_OUT = 2, PCAP_ADC = 4 };
typedef enum { PIN_FREE = 0, PIN_CAUTION, PIN_STRAP, PIN_RESERVED } pin_class_t;

typedef struct {
    uint8_t caps;
    uint8_t cls; /* pin_class_t */
    const char *note;
} board_pin_t;

typedef struct {
    const char *name;
    const board_pin_t *pins;
    uint8_t n_pins;
    int8_t rgb_led_pin;  /* on-board addressable LED, -1 if none */
    int8_t boot_btn_pin; /* boot button (input only), -1 if none */
} board_t;

extern const board_t board_esp32c5_devkitc1;

/* ------------------------------------------------------------- devices */

typedef enum {
    DEV_GPIO_OUT = 0,
    DEV_GPIO_IN,
    DEV_ADC,
    DEV_PWM,
    DEV_RGB,
    DEV_CHIP_TEMP,
    DEV_I2C_SHT3X,
    DEV_I2C_RAW,
    DEV_UART,
    DEV_TYPE_COUNT
} dev_type_t;

typedef enum { ADC_KIND_LINEAR = 0, ADC_KIND_NTC } adc_kind_t;

typedef struct {
    char name[PERIPH_NAME_MAX];
    dev_type_t type;
    int8_t pin;          /* gpio/adc/pwm/rgb; -1 otherwise */
    int8_t pin2;         /* i2c scl / uart rx */
    bool active_low;
    bool llm_write;      /* agent may actuate (local authorization) */
    bool open_drain;
    uint8_t pull;        /* hal_pull_t */
    float safe_value;    /* logical safe state: 0/1, duty %, or brightness % */
    float min_value, max_value;
    uint32_t max_on_ms;  /* 0 = unlimited */
    uint32_t min_toggle_ms;
    uint16_t debounce_ms;
    uint32_t poll_ms;    /* sampler period for sensors, 0 = not sampled */
    /* adc */
    adc_kind_t adc_kind;
    float k_a, k_b;      /* linear: value = mv*k_a + k_b */
    float ntc_beta, ntc_r0, ntc_rs, ntc_vref_mv; /* NTC low side, series resistor to vref */
    char unit[8];
    /* pwm */
    uint32_t freq_hz;
    int8_t pwm_channel;
    /* i2c / uart */
    uint8_t bus;         /* i2c port / uart port */
    uint8_t addr;
    uint32_t bus_hz;     /* i2c clock / uart baud */
    bool readable;       /* i2c_read allowed */
    uint8_t n_wregs;
    uint8_t wregs[PERIPH_I2C_WREGS]; /* i2c_write allowlist */
    uint16_t max_len;    /* uart_send */
} dev_cfg_t;

typedef struct {
    dev_cfg_t cfg;
    float value;          /* last commanded (actuators) or last read (sensors) */
    float value2;         /* secondary reading (humidity) */
    bool valid;
    uint64_t last_change_ms;
    uint64_t on_since_ms; /* 0 when off */
    uint64_t last_read_ms;
    int8_t locked_by;     /* rule index holding a safety lock, -1 none */
    bool watched;
    uint8_t rgb[3];
    uint16_t faults;
} periph_dev_t;

typedef enum {
    PER_OK = 0,
    PER_ERR_NOT_FOUND = -100,
    PER_ERR_NOT_AUTHORIZED,
    PER_ERR_LOCKED,
    PER_ERR_RANGE,
    PER_ERR_RATE,
    PER_ERR_TYPE,
    PER_ERR_HW,
    PER_ERR_UNCERTAIN,
    PER_ERR_PROFILE,
} per_err_t;

const char *per_err_str(int e);

/** Initialise with board and drive nothing (no profile yet). */
void periph_init(const board_t *board);
const board_t *periph_board(void);

/**
 * Parse+validate a JSON profile and, on success, apply it: release old pins,
 * configure new ones and drive every output to its safe state.
 * On failure nothing changes and @p err explains why.
 */
int periph_load_profile(const char *json, size_t n, char *err, size_t errcap);

/** Drive all outputs to their safe state (boot, shutdown, fault). */
void periph_all_safe(void);

int periph_count(void);
const periph_dev_t *periph_get(int idx);
int periph_find(const char *name);
const char *dev_type_str(dev_type_t t);

/** Actuate. @p by_rule >= 0 when called by the rule engine (bypasses llm_write). */
int periph_set(int idx, float value, int by_rule);
int periph_set_rgb(int idx, uint8_t r, uint8_t g, uint8_t b, int by_rule);
/** Read sensor/input; returns calibrated value in device unit. */
int periph_read(int idx, float *value, float *value2);
/** Read back actual output state where hardware allows (gpio). */
int periph_readback(int idx, float *value);

int periph_lock(int idx, int rule);
int periph_unlock(int idx, int rule);

/** Bus helpers enforcing device authorization. */
int periph_i2c_read(int idx, uint8_t reg, uint8_t *out, size_t n, uint32_t timeout_ms);
int periph_i2c_write(int idx, uint8_t reg, const uint8_t *data, size_t n, uint32_t timeout_ms);
int periph_uart_send(int idx, const uint8_t *data, size_t n, uint32_t timeout_ms);

/** Enforce max-on watchdog; call periodically (>= 1 Hz). Returns count of forced-safe devices. */
int periph_tick(uint64_t now_ms);

/** Edge watch: cb runs in ISR context on ESP. */
int periph_watch(int idx, bool enable, hal_edge_cb_t cb, void *u);

/** Configure a profile-declared spare pin as a dynamic device "gpioN".
 *  Returns device index or per_err_t. */
int periph_add_spare(int pin, bool output, hal_pull_t pull, char *name_out, size_t cap);

#define PERIPH_BY_AGENT (-1)
#define PERIPH_BY_LOCAL (-2) /* local console operator: bypasses llm_write, not locks */
#define PERIPH_BY_WATCHDOG 0x7f

/** JSON description of devices for get_registered_capabilities. */
void periph_describe(lla_jw_t *w);
/** Validate a pin for a given use without assigning it. */
bool periph_pin_ok(int pin, uint8_t need_caps, bool allow_strap, char *why, size_t whycap);

#ifdef __cplusplus
}
#endif

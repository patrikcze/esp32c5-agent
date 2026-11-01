/*
 * periph.c — logical device registry with locally enforced safety.
 */
#include "periph.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "lla_port.h"

static const char *TAG = "periph";

#define PWM_CHANNELS 6 /* SOC_LEDC_CHANNEL_NUM on ESP32-C5 */
#define I2C_PORTS 1    /* HP I2C0 only; LP I2C not used */

static struct {
    const board_t *board;
    periph_dev_t dev[PERIPH_MAX_DEVICES];
    int n;
    uint32_t spare_mask; /* pins the agent may configure via gpio_configure */
    bool allow_strap;
    lla_lock_t lock;
    bool lock_init;
} S;

const char *per_err_str(int e)
{
    switch (e) {
    case PER_OK: return "ok";
    case PER_ERR_NOT_FOUND: return "device_not_found";
    case PER_ERR_NOT_AUTHORIZED: return "not_authorized_by_local_policy";
    case PER_ERR_LOCKED: return "locked_by_safety_rule";
    case PER_ERR_RANGE: return "value_out_of_range";
    case PER_ERR_RATE: return "min_toggle_interval";
    case PER_ERR_TYPE: return "wrong_device_type";
    case PER_ERR_HW: return "hardware_error";
    case PER_ERR_UNCERTAIN: return "outcome_uncertain";
    case PER_ERR_PROFILE: return "invalid_profile";
    default: return "error";
    }
}

static const char *const k_type_names[DEV_TYPE_COUNT] = {"gpio_out", "gpio_in", "adc", "pwm", "rgb",
                                                          "chip_temp", "sht3x", "i2c", "uart"};

const char *dev_type_str(dev_type_t t) { return (unsigned)t < DEV_TYPE_COUNT ? k_type_names[t] : "?"; }

void periph_init(const board_t *board)
{
    if (!S.lock_init) {
        lla_lock_init(&S.lock);
        S.lock_init = true;
    }
    lla_lock(&S.lock);
    S.board = board;
    S.n = 0;
    S.spare_mask = 0;
    lla_unlock(&S.lock);
}

const board_t *periph_board(void) { return S.board; }

bool periph_pin_ok(int pin, uint8_t need, bool allow_strap, char *why, size_t whycap)
{
    if (!S.board || pin < 0 || pin >= S.board->n_pins) {
        snprintf(why, whycap, "GPIO%d does not exist", pin);
        return false;
    }
    const board_pin_t *p = &S.board->pins[pin];
    if (p->cls == PIN_RESERVED) {
        snprintf(why, whycap, "GPIO%d reserved: %s", pin, p->note);
        return false;
    }
    if (p->cls == PIN_STRAP && !allow_strap) {
        snprintf(why, whycap, "GPIO%d is a strapping pin (%s); set allow_strapping", pin, p->note);
        return false;
    }
    if ((p->caps & need) != need) {
        snprintf(why, whycap, "GPIO%d lacks capability (%s)", pin, need & PCAP_ADC ? "ADC" : "IO");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------ profile */

typedef struct {
    const char *js;
    const lla_jtok_t *t;
    int obj;
} jo_t;

static int jget(const jo_t *o, const char *k) { return lla_json_obj_get(o->js, o->t, o->obj, k); }

static bool jnum(const jo_t *o, const char *k, double *v)
{
    int i = jget(o, k);
    return i >= 0 && lla_json_num(o->js, &o->t[i], v);
}

static double jnum_d(const jo_t *o, const char *k, double d)
{
    double v;
    return jnum(o, k, &v) ? v : d;
}

static bool jbool_d(const jo_t *o, const char *k, bool d)
{
    int i = jget(o, k);
    if (i < 0) return d;
    return o->t[i].type == LLA_JT_TRUE;
}

static bool jstr(const jo_t *o, const char *k, char *out, size_t cap)
{
    int i = jget(o, k);
    return i >= 0 && lla_json_str(o->js, &o->t[i], out, cap) >= 0;
}

static bool valid_name(const char *n)
{
    if (!n[0]) return false;
    for (const char *p = n; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    }
    return true;
}

#define PERR(...)                                  \
    do {                                           \
        snprintf(err, errcap, __VA_ARGS__);        \
        return PER_ERR_PROFILE;                    \
    } while (0)

static int parse_dev(const jo_t *o, dev_cfg_t *c, bool allow_strap, uint32_t *used, int8_t *i2c_sda, int8_t *i2c_scl,
                     int *pwm_next, char *err, size_t errcap)
{
    memset(c, 0, sizeof(*c));
    c->pin = c->pin2 = -1;
    c->pwm_channel = -1;
    char type[16];
    if (!jstr(o, "name", c->name, sizeof(c->name)) || !valid_name(c->name)) PERR("device name missing/invalid (a-z0-9_, <16)");
    if (!jstr(o, "type", type, sizeof(type))) PERR("%s: type missing", c->name);
    int ty = -1;
    for (int i = 0; i < DEV_TYPE_COUNT; i++)
        if (strcmp(type, k_type_names[i]) == 0) ty = i;
    if (ty < 0) PERR("%s: unknown type '%s'", c->name, type);
    c->type = (dev_type_t)ty;
    c->active_low = jbool_d(o, "active_low", false);
    c->llm_write = jbool_d(o, "llm_write", false);
    c->open_drain = jbool_d(o, "open_drain", false);
    c->safe_value = (float)jnum_d(o, "safe", 0);
    c->min_value = (float)jnum_d(o, "min", 0);
    c->max_value = (float)jnum_d(o, "max", c->type == DEV_GPIO_OUT ? 1 : 100);
    double mo = jnum_d(o, "max_on_s", 0);
    if (mo < 0 || mo > 86400) PERR("%s: max_on_s out of range", c->name);
    c->max_on_ms = (uint32_t)(mo * 1000);
    c->min_toggle_ms = (uint32_t)jnum_d(o, "min_toggle_ms", 0);
    c->debounce_ms = (uint16_t)jnum_d(o, "debounce_ms", 30);
    c->poll_ms = (uint32_t)jnum_d(o, "poll_ms", 0);
    if (c->poll_ms && c->poll_ms < 200) PERR("%s: poll_ms must be >= 200", c->name);
    char pull[8] = "none";
    jstr(o, "pull", pull, sizeof(pull));
    c->pull = strcmp(pull, "up") == 0 ? HAL_PULL_UP : strcmp(pull, "down") == 0 ? HAL_PULL_DOWN : HAL_PULL_NONE;
    snprintf(c->unit, sizeof(c->unit), "%s", "");
    jstr(o, "unit", c->unit, sizeof(c->unit));

    double pin = -1;
    jnum(o, "pin", &pin);
    c->pin = (int8_t)pin;
    char why[96];
    uint8_t need = 0;
    bool strap_ok = allow_strap;
    switch (c->type) {
    case DEV_GPIO_OUT:
        need = PCAP_OUT;
        if (c->safe_value != 0 && c->safe_value != 1) PERR("%s: safe must be 0 or 1", c->name);
        c->min_value = 0;
        c->max_value = 1;
        break;
    case DEV_GPIO_IN:
        need = PCAP_IN;
        if (S.board && c->pin == S.board->boot_btn_pin) strap_ok = true; /* reading BOOT is harmless */
        c->llm_write = false;
        break;
    case DEV_ADC: {
        need = PCAP_ADC;
        char kind[8] = "linear";
        jstr(o, "kind", kind, sizeof(kind));
        c->adc_kind = strcmp(kind, "ntc") == 0 ? ADC_KIND_NTC : ADC_KIND_LINEAR;
        c->k_a = (float)jnum_d(o, "a", 1.0);
        c->k_b = (float)jnum_d(o, "b", 0.0);
        c->ntc_beta = (float)jnum_d(o, "beta", 3950);
        c->ntc_r0 = (float)jnum_d(o, "r0", 10000);
        c->ntc_rs = (float)jnum_d(o, "rs", 10000);
        c->ntc_vref_mv = (float)jnum_d(o, "vref_mv", 3300);
        if (!c->unit[0]) snprintf(c->unit, sizeof(c->unit), "%s", c->adc_kind == ADC_KIND_NTC ? "C" : "mV");
        c->llm_write = false;
        break;
    }
    case DEV_PWM:
        need = PCAP_OUT;
        c->freq_hz = (uint32_t)jnum_d(o, "freq", 25000);
        if (c->freq_hz < 1 || c->freq_hz > 40000) PERR("%s: freq out of range 1..40000", c->name);
        if (c->min_value < 0 || c->max_value > 100 || c->min_value > c->max_value) PERR("%s: bad min/max", c->name);
        if (c->safe_value < 0 || c->safe_value > 100) PERR("%s: bad safe", c->name);
        if (*pwm_next >= PWM_CHANNELS) PERR("%s: no free PWM channel", c->name);
        c->pwm_channel = (int8_t)(*pwm_next)++;
        if (!c->unit[0]) snprintf(c->unit, sizeof(c->unit), "%%");
        break;
    case DEV_RGB:
        need = PCAP_OUT;
        if (S.board && c->pin == S.board->rgb_led_pin) strap_ok = true; /* on-board LED, fixed wiring */
        if (c->max_value > 100) c->max_value = 100;
        if (!c->unit[0]) snprintf(c->unit, sizeof(c->unit), "%%");
        break;
    case DEV_CHIP_TEMP:
        c->llm_write = false;
        snprintf(c->unit, sizeof(c->unit), "C");
        return PER_OK;
    case DEV_I2C_SHT3X:
    case DEV_I2C_RAW: {
        double sda = -1, scl = -1, addr = 0;
        jnum(o, "sda", &sda);
        jnum(o, "scl", &scl);
        jnum(o, "addr", &addr);
        if (addr < 0x08 || addr > 0x77) PERR("%s: i2c addr out of range", c->name);
        c->addr = (uint8_t)addr;
        c->bus = 0;
        c->bus_hz = (uint32_t)jnum_d(o, "hz", 100000);
        c->pin = (int8_t)sda;
        c->pin2 = (int8_t)scl;
        if (*i2c_sda >= 0 && (*i2c_sda != c->pin || *i2c_scl != c->pin2)) PERR("%s: only one I2C bus (pins must match)", c->name);
        bool first = *i2c_sda < 0;
        if (first) {
            if (!periph_pin_ok(c->pin, (PCAP_IN | PCAP_OUT), allow_strap, why, sizeof(why))) PERR("%s: %s", c->name, why);
            if (!periph_pin_ok(c->pin2, (PCAP_IN | PCAP_OUT), allow_strap, why, sizeof(why))) PERR("%s: %s", c->name, why);
            if (c->pin == c->pin2) PERR("%s: sda == scl", c->name);
            if ((*used >> c->pin) & 1 || (*used >> c->pin2) & 1) PERR("%s: i2c pin already in use", c->name);
            *used |= (1u << c->pin) | (1u << c->pin2);
            *i2c_sda = c->pin;
            *i2c_scl = c->pin2;
        }
        if (c->type == DEV_I2C_SHT3X) {
            snprintf(c->unit, sizeof(c->unit), "C");
            c->llm_write = false;
        } else {
            c->readable = jbool_d(o, "readable", false);
            int wr = jget(o, "write_regs");
            if (wr >= 0 && o->t[wr].type == LLA_JT_ARRAY) {
                if (o->t[wr].size > PERIPH_I2C_WREGS) PERR("%s: too many write_regs", c->name);
                for (int i = 0; i < o->t[wr].size; i++) {
                    int64_t r;
                    if (!lla_json_int(o->js, &o->t[lla_json_arr_get(o->t, wr, i)], &r) || r < 0 || r > 255)
                        PERR("%s: bad write_regs", c->name);
                    c->wregs[c->n_wregs++] = (uint8_t)r;
                }
            }
        }
        return PER_OK;
    }
    case DEV_UART: {
        double tx = -1, rx = -1;
        jnum(o, "tx", &tx);
        jnum(o, "rx", &rx);
        c->bus = 1; /* UART0 is the console */
        c->bus_hz = (uint32_t)jnum_d(o, "baud", 115200);
        c->max_len = (uint16_t)jnum_d(o, "max_len", 64);
        if (c->max_len > 128) c->max_len = 128;
        c->pin = (int8_t)tx;
        c->pin2 = (int8_t)rx;
        if (!periph_pin_ok(c->pin, PCAP_OUT, allow_strap, why, sizeof(why))) PERR("%s: %s", c->name, why);
        if ((*used >> c->pin) & 1) PERR("%s: GPIO%d already in use", c->name, c->pin);
        *used |= 1u << c->pin;
        if (c->pin2 >= 0) {
            if (!periph_pin_ok(c->pin2, PCAP_IN, allow_strap, why, sizeof(why))) PERR("%s: %s", c->name, why);
            if ((*used >> c->pin2) & 1) PERR("%s: GPIO%d already in use", c->name, c->pin2);
            *used |= 1u << c->pin2;
        }
        return PER_OK;
    }
    default:
        PERR("%s: unsupported type", c->name);
    }
    if (!periph_pin_ok(c->pin, need, strap_ok, why, sizeof(why))) PERR("%s: %s", c->name, why);
    if ((*used >> c->pin) & 1) PERR("%s: GPIO%d already in use", c->name, c->pin);
    *used |= 1u << c->pin;
    return PER_OK;
}

static int phys(const dev_cfg_t *c, float logical) { return ((logical != 0) ^ c->active_low) ? 1 : 0; }

static void release_all(void)
{
    const hal_ops_t *h = hal();
    for (int i = 0; i < S.n; i++) {
        periph_dev_t *d = &S.dev[i];
        if (d->watched && h && h->gpio_watch) h->gpio_watch(d->cfg.pin, false, NULL, NULL);
        if (d->cfg.type == DEV_GPIO_OUT || d->cfg.type == DEV_GPIO_IN) {
            if (h && h->gpio_release) h->gpio_release(d->cfg.pin);
        }
    }
    S.n = 0;
}

static void init_dev(periph_dev_t *d, bool *i2c_done)
{
    const hal_ops_t *h = hal();
    dev_cfg_t *c = &d->cfg;
    int rc = HAL_ERR_UNSUPPORTED;
    d->locked_by = -1;
    d->value = c->safe_value;
    d->valid = false;
    if (!h) return;
    switch (c->type) {
    case DEV_GPIO_OUT:
        rc = h->gpio_out ? h->gpio_out(c->pin, phys(c, c->safe_value), c->open_drain) : rc;
        d->valid = rc == HAL_OK;
        break;
    case DEV_GPIO_IN:
        rc = h->gpio_in ? h->gpio_in(c->pin, (hal_pull_t)c->pull) : rc;
        break;
    case DEV_PWM: {
        float duty = c->active_low ? 100 - c->safe_value : c->safe_value;
        rc = h->pwm_init ? h->pwm_init(c->pin, c->pwm_channel, c->freq_hz, duty) : rc;
        d->valid = rc == HAL_OK;
        break;
    }
    case DEV_RGB:
        rc = h->rgb_init ? h->rgb_init(c->pin) : rc;
        if (rc == HAL_OK && h->rgb_set) rc = h->rgb_set(c->pin, 0, 0, 0);
        d->valid = rc == HAL_OK;
        d->value = 0;
        break;
    case DEV_I2C_SHT3X:
    case DEV_I2C_RAW:
        if (!*i2c_done) {
            rc = h->i2c_init ? h->i2c_init(c->bus, c->pin, c->pin2, c->bus_hz) : rc;
            *i2c_done = rc == HAL_OK;
        } else {
            rc = HAL_OK;
        }
        break;
    case DEV_UART:
        rc = h->uart_init ? h->uart_init(c->bus, c->pin, c->pin2, c->bus_hz) : rc;
        break;
    case DEV_ADC:
    case DEV_CHIP_TEMP:
        rc = HAL_OK; /* lazily initialised by the HAL on first read */
        break;
    default:
        break;
    }
    if (rc != HAL_OK) {
        d->faults++;
        LLA_LOGE(TAG, "%s (%s) init failed: %s", c->name, dev_type_str(c->type), hal_err_str(rc));
    }
}

static int load_profile_locked(const char *json, size_t n, char *err, size_t errcap, lla_jtok_t *toks,
                               dev_cfg_t *staged);

/* Profile loads are rare: scratch memory is heap-allocated for the duration
 * of the call instead of permanently reserving ~5 KB of .bss. */
int periph_load_profile(const char *json, size_t n, char *err, size_t errcap)
{
    lla_jtok_t *toks = malloc(sizeof(lla_jtok_t) * 384);
    dev_cfg_t *staged = malloc(sizeof(dev_cfg_t) * PERIPH_MAX_DEVICES);
    int rc;
    if (!toks || !staged) {
        snprintf(err, errcap, "out of memory");
        rc = PER_ERR_PROFILE;
    } else {
        lla_lock(&S.lock);
        rc = load_profile_locked(json, n, err, errcap, toks, staged);
        lla_unlock(&S.lock);
    }
    free(toks);
    free(staged);
    if (rc == PER_OK) LLA_LOGI(TAG, "profile applied: %d devices, all outputs in safe state", S.n);
    return rc;
}

static int load_profile_locked(const char *json, size_t n, char *err, size_t errcap, lla_jtok_t *toks,
                               dev_cfg_t *staged)
{
    err[0] = '\0';
    int nt = lla_json_parse(json, n, toks, 384);
    if (nt < 0 || toks[0].type != LLA_JT_OBJECT) {
        snprintf(err, errcap, "profile is not a JSON object (err %d)", nt);
        return PER_ERR_PROFILE;
    }
    jo_t root = {json, toks, 0};
    char board[24] = "";
    if (jstr(&root, "board", board, sizeof(board)) && strcmp(board, S.board->name) != 0) {
        snprintf(err, errcap, "profile is for board '%s', running on '%s'", board, S.board->name);
        return PER_ERR_PROFILE;
    }
    bool allow_strap = jbool_d(&root, "allow_strapping", false);
    int devs = jget(&root, "devices");
    if (devs < 0 || toks[devs].type != LLA_JT_ARRAY || toks[devs].size > PERIPH_MAX_DEVICES) {
        snprintf(err, errcap, "devices must be an array of at most %d", PERIPH_MAX_DEVICES);
        return PER_ERR_PROFILE;
    }
    uint32_t used = 0, spare = 0;
    int8_t sda = -1, scl = -1;
    int pwm_next = 0;
    int count = toks[devs].size;
    for (int i = 0; i < count; i++) {
        jo_t o = {json, toks, lla_json_arr_get(toks, devs, i)};
        if (toks[o.obj].type != LLA_JT_OBJECT) {
            snprintf(err, errcap, "devices[%d] is not an object", i);
            return PER_ERR_PROFILE;
        }
        int rc = parse_dev(&o, &staged[i], allow_strap, &used, &sda, &scl, &pwm_next, err, errcap);
        if (rc != PER_OK) {
            return rc;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(staged[j].name, staged[i].name) == 0) {
                    snprintf(err, errcap, "duplicate device name %s", staged[i].name);
                return PER_ERR_PROFILE;
            }
        }
    }
    int sp = jget(&root, "spare_pins");
    if (sp >= 0 && toks[sp].type == LLA_JT_ARRAY) {
        for (int i = 0; i < toks[sp].size; i++) {
            int64_t p;
            char why[96];
            if (!lla_json_int(json, &toks[lla_json_arr_get(toks, sp, i)], &p) ||
                !periph_pin_ok((int)p, PCAP_IN | PCAP_OUT, allow_strap, why, sizeof(why)) || ((used >> p) & 1)) {
                    snprintf(err, errcap, "spare_pins[%d] invalid or in use", i);
                return PER_ERR_PROFILE;
            }
            spare |= 1u << p;
        }
    }
    /* Validated: apply atomically. */
    release_all();
    S.allow_strap = allow_strap;
    S.spare_mask = spare;
    bool i2c_done = false;
    for (int i = 0; i < count; i++) {
        memset(&S.dev[i], 0, sizeof(S.dev[i]));
        S.dev[i].cfg = staged[i];
        init_dev(&S.dev[i], &i2c_done);
    }
    S.n = count;
    return PER_OK;
}

void periph_all_safe(void)
{
    const hal_ops_t *h = hal();
    if (!h) return;
    lla_lock(&S.lock);
    for (int i = 0; i < S.n; i++) {
        periph_dev_t *d = &S.dev[i];
        dev_cfg_t *c = &d->cfg;
        int rc = HAL_OK;
        switch (c->type) {
        case DEV_GPIO_OUT: rc = h->gpio_set(c->pin, phys(c, c->safe_value)); break;
        case DEV_PWM: rc = h->pwm_duty(c->pwm_channel, c->active_low ? 100 - c->safe_value : c->safe_value); break;
        case DEV_RGB: rc = h->rgb_set(c->pin, 0, 0, 0); break;
        default: continue;
        }
        if (rc == HAL_OK) {
            d->value = c->type == DEV_RGB ? 0 : c->safe_value;
            d->on_since_ms = 0;
        } else {
            d->faults++;
        }
    }
    lla_unlock(&S.lock);
}

int periph_count(void) { return S.n; }
const periph_dev_t *periph_get(int idx) { return (idx >= 0 && idx < S.n) ? &S.dev[idx] : NULL; }

int periph_find(const char *name)
{
    for (int i = 0; i < S.n; i++)
        if (strcmp(S.dev[i].cfg.name, name) == 0) return i;
    return PER_ERR_NOT_FOUND;
}

static int map_hal(int rc) { return rc == HAL_OK ? PER_OK : rc == HAL_ERR_UNCERTAIN ? PER_ERR_UNCERTAIN : PER_ERR_HW; }

static int authorize(periph_dev_t *d, int by)
{
    if (by >= 0) {
        /* deterministic rule engine / watchdog: a safety lock is owned by one rule */
        if (d->locked_by >= 0 && d->locked_by != by && by != PERIPH_BY_WATCHDOG) return PER_ERR_LOCKED;
        return PER_OK;
    }
    if (d->locked_by >= 0) return PER_ERR_LOCKED;
    if (by == -1 && !d->cfg.llm_write) return PER_ERR_NOT_AUTHORIZED; /* -1 agent, -2 local console */
    return PER_OK;
}

static void track(periph_dev_t *d, float v)
{
    uint64_t now = lla_now_ms();
    if (v != d->value) d->last_change_ms = now;
    bool on = v != d->cfg.safe_value;
    if (on && !d->on_since_ms) d->on_since_ms = now ? now : 1;
    if (!on) d->on_since_ms = 0;
    d->value = v;
}

int periph_set(int idx, float value, int by)
{
    if (idx < 0 || idx >= S.n) return PER_ERR_NOT_FOUND;
    lla_lock(&S.lock);
    periph_dev_t *d = &S.dev[idx];
    dev_cfg_t *c = &d->cfg;
    int rc = authorize(d, by);
    if (rc == PER_OK && c->type != DEV_GPIO_OUT && c->type != DEV_PWM && c->type != DEV_RGB) rc = PER_ERR_TYPE;
    if (rc == PER_OK && (isnan(value) || value < c->min_value || value > c->max_value)) rc = PER_ERR_RANGE;
    if (rc == PER_OK && c->type == DEV_GPIO_OUT && value != 0 && value != 1) rc = PER_ERR_RANGE;
    if (rc == PER_OK && by < 0 && c->min_toggle_ms && value != d->value && d->last_change_ms &&
        lla_now_ms() - d->last_change_ms < c->min_toggle_ms) {
        rc = PER_ERR_RATE;
    }
    if (rc == PER_OK) {
        const hal_ops_t *h = hal();
        int hr;
        if (c->type == DEV_GPIO_OUT) {
            hr = h->gpio_set(c->pin, phys(c, value));
        } else if (c->type == DEV_PWM) {
            hr = h->pwm_duty(c->pwm_channel, c->active_low ? 100 - value : value);
        } else {
            /* rgb: value = brightness % of last colour (white if none) */
            uint8_t base[3] = {d->rgb[0], d->rgb[1], d->rgb[2]};
            if (!base[0] && !base[1] && !base[2]) base[0] = base[1] = base[2] = 255;
            hr = h->rgb_set(c->pin, (uint8_t)(base[0] * value / 100), (uint8_t)(base[1] * value / 100),
                            (uint8_t)(base[2] * value / 100));
        }
        rc = map_hal(hr);
        if (rc == PER_OK) track(d, value);
        else d->faults++;
    }
    lla_unlock(&S.lock);
    return rc;
}

int periph_set_rgb(int idx, uint8_t r, uint8_t g, uint8_t b, int by)
{
    if (idx < 0 || idx >= S.n) return PER_ERR_NOT_FOUND;
    lla_lock(&S.lock);
    periph_dev_t *d = &S.dev[idx];
    int rc = authorize(d, by);
    if (rc == PER_OK && d->cfg.type != DEV_RGB) rc = PER_ERR_TYPE;
    if (rc == PER_OK) {
        float lim = d->cfg.max_value / 100.0f; /* brightness cap */
        rc = map_hal(hal()->rgb_set(d->cfg.pin, (uint8_t)(r * lim), (uint8_t)(g * lim), (uint8_t)(b * lim)));
        if (rc == PER_OK) {
            d->rgb[0] = r;
            d->rgb[1] = g;
            d->rgb[2] = b;
            track(d, (r || g || b) ? d->cfg.max_value : 0);
        }
    }
    lla_unlock(&S.lock);
    return rc;
}

static uint8_t crc8_sht(const uint8_t *d, int n)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (uint8_t)(crc & 0x80 ? (crc << 1) ^ 0x31 : crc << 1);
    }
    return crc;
}

int periph_read(int idx, float *value, float *value2)
{
    if (idx < 0 || idx >= S.n) return PER_ERR_NOT_FOUND;
    lla_lock(&S.lock);
    periph_dev_t *d = &S.dev[idx];
    dev_cfg_t *c = &d->cfg;
    const hal_ops_t *h = hal();
    int rc = PER_OK;
    float v = NAN, v2 = NAN;
    switch (c->type) {
    case DEV_GPIO_IN:
    case DEV_GPIO_OUT: {
        int lvl;
        rc = map_hal(h->gpio_get(c->pin, &lvl));
        if (rc == PER_OK) v = (float)((lvl != 0) ^ c->active_low);
        break;
    }
    case DEV_ADC: {
        int mv;
        rc = map_hal(h->adc_mv(c->pin, &mv));
        if (rc != PER_OK) break;
        if (c->adc_kind == ADC_KIND_LINEAR) {
            v = (float)mv * c->k_a + c->k_b;
        } else {
            if (mv <= 0 || (float)mv >= c->ntc_vref_mv) {
                rc = PER_ERR_HW; /* open/short circuit */
                break;
            }
            float r = c->ntc_rs * (float)mv / (c->ntc_vref_mv - (float)mv);
            v = 1.0f / (1.0f / 298.15f + logf(r / c->ntc_r0) / c->ntc_beta) - 273.15f;
        }
        v2 = (float)mv;
        break;
    }
    case DEV_CHIP_TEMP:
        rc = map_hal(h->chip_temp(&v));
        break;
    case DEV_I2C_SHT3X: {
        static const uint8_t cmd[2] = {0x24, 0x00}; /* single shot, high repeatability, no stretching */
        uint8_t b[6];
        rc = map_hal(h->i2c_xfer(c->bus, c->addr, cmd, 2, NULL, 0, 50));
        if (rc != PER_OK) break;
        lla_sleep_ms(16); /* max measurement time 15.5 ms */
        rc = map_hal(h->i2c_xfer(c->bus, c->addr, NULL, 0, b, 6, 50));
        if (rc != PER_OK) break;
        if (crc8_sht(b, 2) != b[2] || crc8_sht(b + 3, 2) != b[5]) {
            rc = PER_ERR_HW;
            break;
        }
        v = -45.0f + 175.0f * (float)((b[0] << 8) | b[1]) / 65535.0f;
        v2 = 100.0f * (float)((b[3] << 8) | b[4]) / 65535.0f;
        break;
    }
    case DEV_PWM:
    case DEV_RGB:
        v = d->value;
        break;
    default:
        rc = PER_ERR_TYPE;
        break;
    }
    if (rc == PER_OK) {
        if (c->type != DEV_GPIO_OUT && c->type != DEV_PWM && c->type != DEV_RGB) d->value = v;
        d->value2 = v2;
        d->valid = true;
        d->last_read_ms = lla_now_ms();
        if (value) *value = v;
        if (value2) *value2 = v2;
    } else if (rc == PER_ERR_HW || rc == PER_ERR_UNCERTAIN) {
        d->faults++;
    }
    lla_unlock(&S.lock);
    return rc;
}

int periph_readback(int idx, float *value)
{
    if (idx < 0 || idx >= S.n) return PER_ERR_NOT_FOUND;
    return periph_read(idx, value, NULL);
}

int periph_lock(int idx, int rule)
{
    if (idx < 0 || idx >= S.n) return PER_ERR_NOT_FOUND;
    lla_lock(&S.lock);
    S.dev[idx].locked_by = (int8_t)rule;
    lla_unlock(&S.lock);
    return PER_OK;
}

int periph_unlock(int idx, int rule)
{
    if (idx < 0 || idx >= S.n) return PER_ERR_NOT_FOUND;
    lla_lock(&S.lock);
    if (S.dev[idx].locked_by == rule) S.dev[idx].locked_by = -1;
    lla_unlock(&S.lock);
    return PER_OK;
}

int periph_i2c_read(int idx, uint8_t reg, uint8_t *out, size_t n, uint32_t timeout_ms)
{
    if (idx < 0 || idx >= S.n) return PER_ERR_NOT_FOUND;
    periph_dev_t *d = &S.dev[idx];
    if (d->cfg.type != DEV_I2C_RAW) return PER_ERR_TYPE;
    if (!d->cfg.readable) return PER_ERR_NOT_AUTHORIZED;
    if (n == 0 || n > 32) return PER_ERR_RANGE;
    lla_lock(&S.lock);
    int rc = map_hal(hal()->i2c_xfer(d->cfg.bus, d->cfg.addr, &reg, 1, out, n, timeout_ms));
    lla_unlock(&S.lock);
    return rc;
}

int periph_i2c_write(int idx, uint8_t reg, const uint8_t *data, size_t n, uint32_t timeout_ms)
{
    if (idx < 0 || idx >= S.n) return PER_ERR_NOT_FOUND;
    periph_dev_t *d = &S.dev[idx];
    if (d->cfg.type != DEV_I2C_RAW) return PER_ERR_TYPE;
    bool ok = false;
    for (uint8_t i = 0; i < d->cfg.n_wregs; i++) ok |= d->cfg.wregs[i] == reg;
    if (!ok) return PER_ERR_NOT_AUTHORIZED;
    if (n == 0 || n > 16) return PER_ERR_RANGE;
    uint8_t buf[17];
    buf[0] = reg;
    memcpy(buf + 1, data, n);
    lla_lock(&S.lock);
    int hr = hal()->i2c_xfer(d->cfg.bus, d->cfg.addr, buf, n + 1, NULL, 0, timeout_ms);
    /* A failed write may have partially reached the device: never "no effect". */
    int rc = hr == HAL_OK ? PER_OK : (hr == HAL_ERR_ARG ? PER_ERR_RANGE : PER_ERR_UNCERTAIN);
    lla_unlock(&S.lock);
    return rc;
}

int periph_uart_send(int idx, const uint8_t *data, size_t n, uint32_t timeout_ms)
{
    if (idx < 0 || idx >= S.n) return PER_ERR_NOT_FOUND;
    periph_dev_t *d = &S.dev[idx];
    if (d->cfg.type != DEV_UART) return PER_ERR_TYPE;
    if (!d->cfg.llm_write) return PER_ERR_NOT_AUTHORIZED;
    if (n == 0 || n > d->cfg.max_len) return PER_ERR_RANGE;
    lla_lock(&S.lock);
    int hr = hal()->uart_write(d->cfg.bus, data, n, timeout_ms);
    int rc = hr == HAL_OK ? PER_OK : hr == HAL_ERR_TIMEOUT ? PER_ERR_UNCERTAIN : map_hal(hr);
    lla_unlock(&S.lock);
    return rc;
}

int periph_tick(uint64_t now)
{
    int forced = 0;
    lla_lock(&S.lock);
    for (int i = 0; i < S.n; i++) {
        periph_dev_t *d = &S.dev[i];
        if (!d->cfg.max_on_ms || !d->on_since_ms || d->locked_by >= 0) continue;
        if (now - d->on_since_ms >= d->cfg.max_on_ms) {
            LLA_LOGW(TAG, "%s exceeded max on-time (%u s): forcing safe state", d->cfg.name,
                     (unsigned)(d->cfg.max_on_ms / 1000));
            int rc = periph_set(i, d->cfg.safe_value, PERIPH_BY_WATCHDOG);
            if (rc != PER_OK) {
                d->faults++;
                LLA_LOGE(TAG, "%s: failed to force safe state: %s", d->cfg.name, per_err_str(rc));
            }
            forced++;
        }
    }
    lla_unlock(&S.lock);
    return forced;
}

int periph_watch(int idx, bool enable, hal_edge_cb_t cb, void *u)
{
    if (idx < 0 || idx >= S.n) return PER_ERR_NOT_FOUND;
    periph_dev_t *d = &S.dev[idx];
    if (d->cfg.type != DEV_GPIO_IN) return PER_ERR_TYPE;
    const hal_ops_t *h = hal();
    if (!h->gpio_watch) return PER_ERR_HW;
    lla_lock(&S.lock);
    int rc = map_hal(h->gpio_watch(d->cfg.pin, enable, cb, u));
    if (rc == PER_OK) d->watched = enable;
    lla_unlock(&S.lock);
    return rc;
}

int periph_add_spare(int pin, bool output, hal_pull_t pull, char *name_out, size_t cap)
{
    if (pin < 0 || pin >= BOARD_GPIO_COUNT || !((S.spare_mask >> pin) & 1)) return PER_ERR_NOT_AUTHORIZED;
    lla_lock(&S.lock);
    snprintf(name_out, cap, "gpio%d", pin);
    int idx = periph_find(name_out);
    if (idx < 0) {
        if (S.n >= PERIPH_MAX_DEVICES) {
            lla_unlock(&S.lock);
            return PER_ERR_RANGE;
        }
        idx = S.n++;
    } else if (S.dev[idx].watched) {
        hal()->gpio_watch(pin, false, NULL, NULL);
    }
    periph_dev_t *d = &S.dev[idx];
    memset(d, 0, sizeof(*d));
    snprintf(d->cfg.name, sizeof(d->cfg.name), "gpio%d", pin);
    d->cfg.type = output ? DEV_GPIO_OUT : DEV_GPIO_IN;
    d->cfg.pin = (int8_t)pin;
    d->cfg.pin2 = -1;
    d->cfg.pwm_channel = -1;
    d->cfg.pull = (uint8_t)pull;
    d->cfg.max_value = 1;
    d->cfg.llm_write = output; /* the local profile listed this pin as spare */
    d->cfg.debounce_ms = 30;
    bool dummy = false;
    init_dev(d, &dummy);
    lla_unlock(&S.lock);
    return d->faults ? PER_ERR_HW : idx;
}

int periph_describe(lla_jw_t *w, size_t reserve)
{
    int shown = 0;
    lla_lock(&S.lock);
    lla_jw_arr_begin(w);
    for (int i = 0; i < S.n; i++) {
        const periph_dev_t *d = &S.dev[i];
        lla_jw_t snap = *w;
        const dev_cfg_t *c = &d->cfg;
        lla_jw_obj_begin(w);
        lla_jw_kv_str(w, "name", c->name);
        lla_jw_kv_str(w, "type", dev_type_str(c->type));
        if (c->unit[0]) lla_jw_kv_str(w, "unit", c->unit);
        bool act = c->type == DEV_GPIO_OUT || c->type == DEV_PWM || c->type == DEV_RGB;
        if (act) {
            lla_jw_kv_bool(w, "agent_writable", c->llm_write);
            lla_jw_kv_num(w, "min", c->min_value);
            lla_jw_kv_num(w, "max", c->max_value);
            lla_jw_kv_num(w, "value", d->value);
            if (d->locked_by >= 0) lla_jw_kv_bool(w, "locked_by_safety_rule", true);
        } else if (d->valid) {
            lla_jw_kv_num(w, "last", d->value);
        }
        if (c->type == DEV_I2C_RAW) {
            lla_jw_kv_bool(w, "readable", c->readable);
            lla_jw_kv_int(w, "writable_regs", c->n_wregs);
        }
        if (c->type == DEV_UART) lla_jw_kv_bool(w, "agent_writable", c->llm_write);
        if (d->faults) lla_jw_kv_int(w, "faults", d->faults);
        lla_jw_obj_end(w);
        if (!lla_jw_room(w, reserve)) {
            lla_jw_rollback(w, &snap);
            break;
        }
        shown++;
    }
    lla_jw_arr_end(w);
    lla_unlock(&S.lock);
    return S.n - shown;
}

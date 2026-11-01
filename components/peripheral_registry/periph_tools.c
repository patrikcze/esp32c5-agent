/*
 * periph_tools.c — agent tools over the peripheral registry (portable).
 *
 * Every tool addresses hardware by logical device name. The LLM can never
 * name an arbitrary pin except through gpio_configure, which is restricted to
 * pins the local profile lists as "spare_pins" and needs CONFIG permission.
 */
#include "periph_tools.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lla_port.h"

static periph_watch_fn s_watch_fn;

void periph_tools_set_watch_fn(periph_watch_fn fn) { s_watch_fn = fn; }

static int dev_arg(const tool_def_t *d, const tool_args_t *a, tool_result_t *r)
{
    const char *name = tool_arg_str(d, a, "device", "");
    int idx = periph_find(name);
    if (idx < 0) tool_result_fail(r, TOOL_ST_REJECTED, "device_not_found", "call get_registered_capabilities");
    return idx;
}

static void fail_per(tool_result_t *r, int rc)
{
    tool_status_t st = rc == PER_ERR_UNCERTAIN ? TOOL_ST_UNKNOWN
                       : (rc == PER_ERR_HW)     ? TOOL_ST_ERROR
                                                : TOOL_ST_REJECTED;
    tool_result_fail(r, st, per_err_str(rc), NULL);
}

static const tool_arg_def_t A_DEV[] = {
    {.name = "device", .desc = "logical device name", .type = TARG_STR, .required = true, .max_len = 15}};

/* ----------------------------------------------------------- capability */

static void x_caps(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)a; (void)dl;
    lla_jw_kv_str(&r->w, "board", periph_board() ? periph_board()->name : "?");
    lla_jw_key(&r->w, "devices");
    periph_describe(&r->w);
}

/* ----------------------------------------------------------- sensors */

static void x_read_sensor(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)dl;
    int idx = dev_arg(d, a, r);
    if (idx < 0) return;
    const periph_dev_t *dv = periph_get(idx);
    float v, v2;
    int rc = periph_read(idx, &v, &v2);
    if (rc != PER_OK) {
        fail_per(r, rc);
        return;
    }
    lla_jw_kv_str(&r->w, "device", dv->cfg.name);
    lla_jw_kv_num(&r->w, "value", v);
    if (dv->cfg.unit[0]) lla_jw_kv_str(&r->w, "unit", dv->cfg.unit);
    if (dv->cfg.type == DEV_I2C_SHT3X) lla_jw_kv_num(&r->w, "humidity_pct", v2);
    if (dv->cfg.type == DEV_CHIP_TEMP) lla_jw_kv_str(&r->w, "note", "die temperature, not ambient");
}

static void x_adc_read(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)dl;
    int idx = dev_arg(d, a, r);
    if (idx < 0) return;
    const periph_dev_t *dv = periph_get(idx);
    if (dv->cfg.type != DEV_ADC) {
        tool_result_fail(r, TOOL_ST_REJECTED, "wrong_device_type", "adc device required");
        return;
    }
    float v, mv;
    int rc = periph_read(idx, &v, &mv);
    if (rc != PER_OK) {
        fail_per(r, rc);
        return;
    }
    lla_jw_kv_num(&r->w, "mv", mv);
    lla_jw_kv_num(&r->w, "value", v);
    lla_jw_kv_str(&r->w, "unit", dv->cfg.unit);
}

/* ----------------------------------------------------------- GPIO */

static void x_gpio_read(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)dl;
    int idx = dev_arg(d, a, r);
    if (idx < 0) return;
    const periph_dev_t *dv = periph_get(idx);
    if (dv->cfg.type != DEV_GPIO_IN && dv->cfg.type != DEV_GPIO_OUT) {
        tool_result_fail(r, TOOL_ST_REJECTED, "wrong_device_type", "gpio device required");
        return;
    }
    float v;
    int rc = periph_read(idx, &v, NULL);
    if (rc != PER_OK) {
        fail_per(r, rc);
        return;
    }
    lla_jw_kv_int(&r->w, "level", (int)v);
}

static const tool_arg_def_t A_GPIO_W[] = {
    {.name = "device", .type = TARG_STR, .required = true, .max_len = 15},
    {.name = "level", .desc = "logical level (1 = active)", .type = TARG_INT, .required = true, .min = 0, .max = 1}};

static void x_gpio_write(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)dl;
    int idx = dev_arg(d, a, r);
    if (idx < 0) return;
    if (periph_get(idx)->cfg.type != DEV_GPIO_OUT) {
        tool_result_fail(r, TOOL_ST_REJECTED, "wrong_device_type", "gpio_out device required");
        return;
    }
    int rc = periph_set(idx, (float)tool_arg_int(d, a, "level", 0), PERIPH_BY_AGENT);
    if (rc != PER_OK) {
        fail_per(r, rc);
        return;
    }
    lla_jw_kv_int(&r->w, "level", tool_arg_int(d, a, "level", 0));
}

static bool v_gpio_write(const tool_def_t *d, const tool_args_t *a, tool_result_t *r)
{
    float v;
    int idx = periph_find(tool_arg_str(d, a, "device", ""));
    if (idx < 0 || periph_readback(idx, &v) != PER_OK) return false;
    lla_jw_kv_int(&r->w, "readback", (int)v);
    return (int)v == tool_arg_int(d, a, "level", -1);
}

static const char *const E_MODE[] = {"input", "output", NULL};
static const char *const E_PULL[] = {"none", "up", "down", NULL};
static const tool_arg_def_t A_GPIO_CFG[] = {
    {.name = "pin", .desc = "GPIO number; must be a profile spare pin", .type = TARG_INT, .required = true, .min = 0, .max = 28},
    {.name = "mode", .type = TARG_ENUM, .required = true, .enum_vals = E_MODE},
    {.name = "pull", .type = TARG_ENUM, .required = false, .enum_vals = E_PULL}};

static void x_gpio_configure(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)dl;
    const char *pull = tool_arg_str(d, a, "pull", "none");
    hal_pull_t p = strcmp(pull, "up") == 0 ? HAL_PULL_UP : strcmp(pull, "down") == 0 ? HAL_PULL_DOWN : HAL_PULL_NONE;
    char name[PERIPH_NAME_MAX];
    int rc = periph_add_spare((int)tool_arg_int(d, a, "pin", -1), strcmp(tool_arg_str(d, a, "mode", ""), "output") == 0,
                              p, name, sizeof(name));
    if (rc < 0) {
        fail_per(r, rc);
        return;
    }
    lla_jw_kv_str(&r->w, "device", name);
}

static void x_gpio_watch(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)dl;
    int idx = dev_arg(d, a, r);
    if (idx < 0) return;
    if (!s_watch_fn) {
        tool_result_fail(r, TOOL_ST_ERROR, "event_engine_unavailable", NULL);
        return;
    }
    bool en = strcmp(d->name, "gpio_watch") == 0;
    int rc = s_watch_fn(idx, en);
    if (rc != PER_OK) {
        fail_per(r, rc);
        return;
    }
    lla_jw_kv_bool(&r->w, "watching", en);
}

/* ----------------------------------------------------------- actuators */

static const tool_arg_def_t A_ACT[] = {
    {.name = "device", .type = TARG_STR, .required = true, .max_len = 15},
    {.name = "value", .desc = "0/1 for switches, percent for pwm/rgb", .type = TARG_NUM, .required = true, .min = 0, .max = 100},
    {.name = "color", .desc = "rgb only, #RRGGBB", .type = TARG_STR, .required = false, .max_len = 7}};

static bool parse_color(const char *s, uint8_t rgb[3])
{
    if (strlen(s) != 7 || s[0] != '#') return false;
    for (int i = 0; i < 3; i++) {
        unsigned v;
        char tmp[3] = {s[1 + 2 * i], s[2 + 2 * i], 0};
        if (!isxdigit((unsigned char)tmp[0]) || !isxdigit((unsigned char)tmp[1]) || sscanf(tmp, "%2x", &v) != 1) return false;
        rgb[i] = (uint8_t)v;
    }
    return true;
}

static void x_set_actuator(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)dl;
    int idx = dev_arg(d, a, r);
    if (idx < 0) return;
    const periph_dev_t *dv = periph_get(idx);
    float v = (float)tool_arg_num(d, a, "value", NAN);
    int rc;
    if (dv->cfg.type == DEV_RGB && tool_arg_present(d, a, "color")) {
        uint8_t c[3];
        if (!parse_color(tool_arg_str(d, a, "color", ""), c)) {
            tool_result_fail(r, TOOL_ST_REJECTED, "invalid_arguments", "color must be #RRGGBB");
            return;
        }
        for (int i = 0; i < 3; i++) c[i] = (uint8_t)(c[i] * v / 100.0f);
        rc = periph_set_rgb(idx, c[0], c[1], c[2], PERIPH_BY_AGENT);
    } else {
        rc = periph_set(idx, v, PERIPH_BY_AGENT);
    }
    if (rc != PER_OK) {
        fail_per(r, rc);
        return;
    }
    lla_jw_kv_str(&r->w, "device", dv->cfg.name);
    lla_jw_kv_num(&r->w, "value", v);
}

static bool v_set_actuator(const tool_def_t *d, const tool_args_t *a, tool_result_t *r)
{
    int idx = periph_find(tool_arg_str(d, a, "device", ""));
    if (idx < 0) return false;
    const periph_dev_t *dv = periph_get(idx);
    if (dv->cfg.type == DEV_GPIO_OUT) {
        float v;
        if (periph_readback(idx, &v) != PER_OK) return false;
        lla_jw_kv_num(&r->w, "readback", v);
        return v == (float)tool_arg_num(d, a, "value", -1);
    }
    /* pwm/rgb have no electrical readback: the commanded state is reported */
    lla_jw_kv_str(&r->w, "readback", "commanded_only");
    return dv->cfg.type == DEV_RGB || dv->value == (float)tool_arg_num(d, a, "value", -1);
}

static const tool_arg_def_t A_PWM[] = {
    {.name = "device", .type = TARG_STR, .required = true, .max_len = 15},
    {.name = "duty_pct", .type = TARG_NUM, .required = true, .min = 0, .max = 100}};

static void x_pwm_set(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)dl;
    int idx = dev_arg(d, a, r);
    if (idx < 0) return;
    if (periph_get(idx)->cfg.type != DEV_PWM) {
        tool_result_fail(r, TOOL_ST_REJECTED, "wrong_device_type", "pwm device required");
        return;
    }
    float v = (float)tool_arg_num(d, a, "duty_pct", NAN);
    int rc = periph_set(idx, v, PERIPH_BY_AGENT);
    if (rc != PER_OK) {
        fail_per(r, rc);
        return;
    }
    lla_jw_kv_num(&r->w, "duty_pct", v);
}

/* ----------------------------------------------------------- buses */

static const tool_arg_def_t A_I2C_R[] = {
    {.name = "device", .type = TARG_STR, .required = true, .max_len = 15},
    {.name = "reg", .type = TARG_INT, .required = true, .min = 0, .max = 255},
    {.name = "len", .type = TARG_INT, .required = true, .min = 1, .max = 32}};

static uint32_t ms_left(uint64_t dl)
{
    uint64_t now = lla_now_ms();
    return now >= dl ? 0 : (uint32_t)(dl - now);
}

static void x_i2c_read(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    int idx = dev_arg(d, a, r);
    if (idx < 0) return;
    uint32_t left = ms_left(dl);
    if (!left) {
        tool_result_fail(r, TOOL_ST_TIMEOUT, "deadline", NULL);
        return;
    }
    uint8_t buf[32];
    size_t n = (size_t)tool_arg_int(d, a, "len", 1);
    int rc = periph_i2c_read(idx, (uint8_t)tool_arg_int(d, a, "reg", 0), buf, n, left);
    if (rc != PER_OK) {
        fail_per(r, rc == PER_ERR_UNCERTAIN ? PER_ERR_HW : rc); /* reads have no side effect */
        return;
    }
    char hex[65];
    for (size_t i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", buf[i]);
    lla_jw_kv_str(&r->w, "hex", hex);
}

static const tool_arg_def_t A_I2C_W[] = {
    {.name = "device", .type = TARG_STR, .required = true, .max_len = 15},
    {.name = "reg", .type = TARG_INT, .required = true, .min = 0, .max = 255},
    {.name = "hex", .desc = "bytes as hex, e.g. \"0a1f\"", .type = TARG_STR, .required = true, .max_len = 32}};

static void x_i2c_write(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    int idx = dev_arg(d, a, r);
    if (idx < 0) return;
    const char *hex = tool_arg_str(d, a, "hex", "");
    size_t hl = strlen(hex);
    uint8_t buf[16];
    if (hl == 0 || hl % 2 || hl / 2 > sizeof(buf)) {
        tool_result_fail(r, TOOL_ST_REJECTED, "invalid_arguments", "hex must have an even number of digits, max 32");
        return;
    }
    for (size_t i = 0; i < hl / 2; i++) {
        unsigned v;
        char t[3] = {hex[2 * i], hex[2 * i + 1], 0};
        if (!isxdigit((unsigned char)t[0]) || !isxdigit((unsigned char)t[1]) || sscanf(t, "%2x", &v) != 1) {
            tool_result_fail(r, TOOL_ST_REJECTED, "invalid_arguments", "hex contains non-hex characters");
            return;
        }
        buf[i] = (uint8_t)v;
    }
    uint32_t left = ms_left(dl);
    if (!left) {
        tool_result_fail(r, TOOL_ST_TIMEOUT, "deadline", NULL);
        return;
    }
    int rc = periph_i2c_write(idx, (uint8_t)tool_arg_int(d, a, "reg", 0), buf, hl / 2, left);
    if (rc != PER_OK) {
        fail_per(r, rc);
        return;
    }
    lla_jw_kv_int(&r->w, "written", (int64_t)(hl / 2));
}

static const tool_arg_def_t A_UART[] = {
    {.name = "device", .type = TARG_STR, .required = true, .max_len = 15},
    {.name = "text", .desc = "printable ASCII", .type = TARG_STR, .required = true, .max_len = 128}};

static void x_uart_send(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    int idx = dev_arg(d, a, r);
    if (idx < 0) return;
    const char *t = tool_arg_str(d, a, "text", "");
    for (const char *p = t; *p; p++) {
        if (*p < 0x20 || *p > 0x7e) {
            tool_result_fail(r, TOOL_ST_REJECTED, "invalid_arguments", "only printable ASCII allowed");
            return;
        }
    }
    uint32_t left = ms_left(dl);
    if (!left) {
        tool_result_fail(r, TOOL_ST_TIMEOUT, "deadline", NULL);
        return;
    }
    int rc = periph_uart_send(idx, (const uint8_t *)t, strlen(t), left);
    if (rc != PER_OK) {
        fail_per(r, rc);
        return;
    }
    lla_jw_kv_int(&r->w, "sent", (int64_t)strlen(t));
}

/* ----------------------------------------------------------- table */

static const tool_def_t k_tools[] = {
    {.name = "get_registered_capabilities", .desc = "List the board's logical devices, units, limits and writability.",
     .perm = TOOL_PERM_READ, .timeout_ms = 200, .exec = x_caps},
    {.name = "read_sensor", .desc = "Read a sensor or input device by name; returns value and unit.", .args = A_DEV,
     .n_args = 1, .perm = TOOL_PERM_READ, .timeout_ms = 500, .exec = x_read_sensor},
    {.name = "adc_read", .desc = "Read an ADC device: calibrated millivolts and converted value.", .args = A_DEV,
     .n_args = 1, .perm = TOOL_PERM_READ, .timeout_ms = 200, .exec = x_adc_read},
    {.name = "gpio_read", .desc = "Read the logical level of a GPIO device.", .args = A_DEV, .n_args = 1,
     .perm = TOOL_PERM_READ, .timeout_ms = 100, .exec = x_gpio_read},
    {.name = "gpio_write", .desc = "Set a gpio_out device's logical level (if locally authorized).", .args = A_GPIO_W,
     .n_args = 2, .perm = TOOL_PERM_ACTUATE, .effect = TOOL_EFFECT_IDEMPOTENT, .timeout_ms = 100,
     .exec = x_gpio_write, .verify = v_gpio_write},
    {.name = "set_actuator", .desc = "Set an actuator (switch 0/1, pwm %, rgb brightness % with optional color).",
     .args = A_ACT, .n_args = 3, .perm = TOOL_PERM_ACTUATE, .effect = TOOL_EFFECT_IDEMPOTENT, .timeout_ms = 200,
     .exec = x_set_actuator, .verify = v_set_actuator},
    {.name = "pwm_set", .desc = "Set PWM duty cycle percent within the device's configured limits.", .args = A_PWM,
     .n_args = 2, .perm = TOOL_PERM_ACTUATE, .effect = TOOL_EFFECT_IDEMPOTENT, .timeout_ms = 100, .exec = x_pwm_set},
    {.name = "gpio_configure", .desc = "Configure a profile-approved spare pin as input or output (starts low).",
     .args = A_GPIO_CFG, .n_args = 3, .perm = TOOL_PERM_CONFIG, .effect = TOOL_EFFECT_IDEMPOTENT, .timeout_ms = 100,
     .exec = x_gpio_configure},
    {.name = "gpio_watch", .desc = "Start debounced edge monitoring of a gpio_in device (events go to the event engine).",
     .args = A_DEV, .n_args = 1, .perm = TOOL_PERM_CONFIG, .effect = TOOL_EFFECT_IDEMPOTENT, .timeout_ms = 100,
     .exec = x_gpio_watch},
    {.name = "gpio_stop_watch", .desc = "Stop monitoring a gpio_in device.", .args = A_DEV, .n_args = 1,
     .perm = TOOL_PERM_CONFIG, .effect = TOOL_EFFECT_IDEMPOTENT, .timeout_ms = 100, .exec = x_gpio_watch},
    {.name = "i2c_read", .desc = "Read registers from a registered, readable I2C device.", .args = A_I2C_R, .n_args = 3,
     .perm = TOOL_PERM_READ, .timeout_ms = 200, .exec = x_i2c_read},
    {.name = "i2c_write", .desc = "Write to an allowlisted register of a registered I2C device.", .args = A_I2C_W,
     .n_args = 3, .perm = TOOL_PERM_ACTUATE, .effect = TOOL_EFFECT_NON_IDEMPOTENT, .timeout_ms = 200,
     .exec = x_i2c_write},
    {.name = "uart_send", .desc = "Send printable text to a registered, authorized UART device.", .args = A_UART,
     .n_args = 2, .perm = TOOL_PERM_ACTUATE, .effect = TOOL_EFFECT_NON_IDEMPOTENT, .timeout_ms = 500,
     .exec = x_uart_send},
};

int periph_tools_register(tool_registry_t *reg)
{
    for (size_t i = 0; i < sizeof(k_tools) / sizeof(k_tools[0]); i++) {
        if (tool_registry_add(reg, &k_tools[i]) < 0) return -1;
    }
    return (int)(sizeof(k_tools) / sizeof(k_tools[0]));
}

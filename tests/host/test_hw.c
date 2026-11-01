/* Peripheral registry, periph tools, rules, events, config, registry, conv. */
#include <math.h>

#include "agent.h"
#include "ble_mgr.h"
#include "config.h"
#include "event_engine.h"
#include "periph.h"
#include "periph_tools.h"
#include "test.h"

/* ------------------------------------------------------------ mock HAL */

static int g_level[32], g_mode[32]; /* mode: 0 unused, 1 out, 2 in */
static int g_released[32];
static float g_duty[8];
static uint8_t g_rgb[3];
static int g_adc_mv = 1650;
static float g_chip = 41.5f;
static int g_i2c_fail; /* HAL error to return */
static uint8_t g_i2c_last[20];
static size_t g_i2c_last_n;
static int g_gpio_fail_pin = -1;

static int m_out(int pin, int level, bool od)
{
    (void)od;
    g_mode[pin] = 1;
    g_level[pin] = level;
    return HAL_OK;
}
static int m_in(int pin, hal_pull_t p)
{
    (void)p;
    g_mode[pin] = 2;
    return HAL_OK;
}
static int m_set(int pin, int level)
{
    if (pin == g_gpio_fail_pin) return HAL_ERR_IO;
    g_level[pin] = level;
    return HAL_OK;
}
static int m_get(int pin, int *level)
{
    *level = g_level[pin];
    return HAL_OK;
}
static int m_release(int pin)
{
    g_mode[pin] = 0;
    g_released[pin]++;
    return HAL_OK;
}
static int m_watch(int pin, bool en, hal_edge_cb_t cb, void *u)
{
    (void)pin; (void)en; (void)cb; (void)u;
    return HAL_OK;
}
static int m_adc(int pin, int *mv)
{
    (void)pin;
    *mv = g_adc_mv;
    return HAL_OK;
}
static int m_pwm_init(int pin, int ch, uint32_t f, float d)
{
    (void)pin; (void)f;
    g_duty[ch] = d;
    return HAL_OK;
}
static int m_pwm(int ch, float d)
{
    g_duty[ch] = d;
    return HAL_OK;
}
static int m_i2c_init(int port, int sda, int scl, uint32_t hz)
{
    (void)port; (void)sda; (void)scl; (void)hz;
    return HAL_OK;
}
static int m_i2c(int port, uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn, uint32_t to)
{
    (void)port; (void)addr; (void)to;
    if (g_i2c_fail) return g_i2c_fail;
    if (wn) {
        memcpy(g_i2c_last, w, wn);
        g_i2c_last_n = wn;
    }
    if (rn == 6 && wn == 0) { /* SHT3x: 25.0 C → raw = (25+45)/175*65535 = 26214 (0x6666); 50% → 0x7FFF */
        uint8_t b[6] = {0x66, 0x66, 0, 0x7f, 0xff, 0};
        uint8_t crc(const uint8_t *, int);
        b[2] = crc(b, 2);
        b[5] = crc(b + 3, 2);
        memcpy(r, b, 6);
    } else {
        for (size_t i = 0; i < rn; i++) r[i] = (uint8_t)(0xA0 + i);
    }
    return HAL_OK;
}
uint8_t crc(const uint8_t *d, int n)
{
    uint8_t c = 0xFF;
    for (int i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x31 : c << 1);
    }
    return c;
}
static int m_uart_init(int p, int tx, int rx, uint32_t b)
{
    (void)p; (void)tx; (void)rx; (void)b;
    return HAL_OK;
}
static int m_uart(int p, const uint8_t *d, size_t n, uint32_t to)
{
    (void)p; (void)d; (void)n; (void)to;
    return HAL_OK;
}
static int m_rgb_init(int pin)
{
    (void)pin;
    return HAL_OK;
}
static int m_rgb(int pin, uint8_t r, uint8_t g, uint8_t b)
{
    (void)pin;
    g_rgb[0] = r;
    g_rgb[1] = g;
    g_rgb[2] = b;
    return HAL_OK;
}
static int m_chip(float *c)
{
    *c = g_chip;
    return HAL_OK;
}

static const hal_ops_t k_mock_hal = {m_out, m_in, m_set, m_get, m_release, m_watch, m_adc, m_pwm_init, m_pwm,
                                     m_i2c_init, m_i2c, m_uart_init, m_uart, m_rgb_init, m_rgb, m_chip};

static const char *k_profile =
    "{\"board\":\"esp32c5-devkitc-1\",\"spare_pins\":[8,9],\"devices\":["
    "{\"name\":\"status_led\",\"type\":\"rgb\",\"pin\":27,\"llm_write\":true,\"max\":30},"
    "{\"name\":\"cooling_fan\",\"type\":\"gpio_out\",\"pin\":23,\"safe\":0,\"llm_write\":true,\"max_on_s\":60,\"min_toggle_ms\":1000},"
    "{\"name\":\"heater\",\"type\":\"gpio_out\",\"pin\":24,\"active_low\":true,\"safe\":0},"
    "{\"name\":\"button\",\"type\":\"gpio_in\",\"pin\":28,\"active_low\":true,\"pull\":\"up\"},"
    "{\"name\":\"room_temp\",\"type\":\"adc\",\"pin\":4,\"kind\":\"ntc\",\"beta\":3950,\"r0\":10000,\"rs\":10000,\"vref_mv\":3300},"
    "{\"name\":\"light\",\"type\":\"adc\",\"pin\":5,\"a\":0.1,\"b\":-5,\"unit\":\"lux\"},"
    "{\"name\":\"blower\",\"type\":\"pwm\",\"pin\":6,\"freq\":25000,\"min\":20,\"max\":80,\"safe\":0,\"llm_write\":true},"
    "{\"name\":\"chip_temp\",\"type\":\"chip_temp\"},"
    "{\"name\":\"env\",\"type\":\"sht3x\",\"sda\":10,\"scl\":0,\"addr\":68},"
    "{\"name\":\"expander\",\"type\":\"i2c\",\"sda\":10,\"scl\":0,\"addr\":32,\"readable\":true,\"write_regs\":[1,3]},"
    "{\"name\":\"display\",\"type\":\"uart\",\"tx\":1,\"llm_write\":true,\"max_len\":16}"
    "]}";

static void load_ok(void)
{
    memset(g_mode, 0, sizeof(g_mode));
    memset(g_level, 0, sizeof(g_level));
    hal_set(&k_mock_hal);
    periph_init(&board_esp32c5_devkitc1);
    char err[128];
    int rc = periph_load_profile(k_profile, strlen(k_profile), err, sizeof(err));
    if (rc != PER_OK) fprintf(stderr, "    profile error: %s\n", err);
    CHECK_EQ_INT(rc, PER_OK);
}

static void test_profile_validation(void)
{
    load_ok();
    CHECK_EQ_INT(periph_count(), 11);
    CHECK_EQ_INT(g_mode[23], 1);
    CHECK_EQ_INT(g_level[23], 0);
    CHECK_EQ_INT(g_level[24], 1); /* active-low heater safe=0 → physical high */
    CHECK_EQ_INT(g_mode[28], 2);
    char err[128];
    struct {
        const char *js;
        const char *msg;
    } bad[] = {
        {"{\"devices\":[{\"name\":\"x\",\"type\":\"gpio_out\",\"pin\":17}]}", "reserved"},
        {"{\"devices\":[{\"name\":\"x\",\"type\":\"gpio_out\",\"pin\":12}]}", "console"},
        {"{\"devices\":[{\"name\":\"x\",\"type\":\"gpio_out\",\"pin\":13}]}", "USB"},
        {"{\"devices\":[{\"name\":\"x\",\"type\":\"gpio_out\",\"pin\":7}]}", "strapping"},
        {"{\"devices\":[{\"name\":\"x\",\"type\":\"gpio_out\",\"pin\":40}]}", "does not exist"},
        {"{\"devices\":[{\"name\":\"x\",\"type\":\"adc\",\"pin\":8}]}", "capability"},
        {"{\"devices\":[{\"name\":\"a\",\"type\":\"gpio_out\",\"pin\":8},{\"name\":\"b\",\"type\":\"gpio_in\",\"pin\":8}]}", "in use"},
        {"{\"devices\":[{\"name\":\"a\",\"type\":\"gpio_out\",\"pin\":8},{\"name\":\"a\",\"type\":\"gpio_out\",\"pin\":9}]}", "duplicate"},
        {"{\"devices\":[{\"name\":\"Bad Name\",\"type\":\"gpio_out\",\"pin\":8}]}", "name"},
        {"{\"devices\":[{\"name\":\"x\",\"type\":\"relay\",\"pin\":8}]}", "unknown type"},
        {"{\"devices\":[{\"name\":\"u\",\"type\":\"uart\",\"tx\":11}]}", "reserved"},
        {"{\"board\":\"esp32s3\",\"devices\":[]}", "board"},
        {"{\"devices\":[{\"name\":\"x\",\"type\":\"gpio_out\",\"pin\":8}],\"spare_pins\":[8]}", "spare_pins"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK_EQ_INT(periph_load_profile(bad[i].js, strlen(bad[i].js), err, sizeof(err)), PER_ERR_PROFILE);
        CHECK_CONTAINS(err, bad[i].msg);
    }
    CHECK_EQ_INT(periph_count(), 11); /* failed loads change nothing */
    const char *strap = "{\"allow_strapping\":true,\"devices\":[{\"name\":\"x\",\"type\":\"gpio_out\",\"pin\":7}]}";
    CHECK_EQ_INT(periph_load_profile(strap, strlen(strap), err, sizeof(err)), PER_OK);
    CHECK(g_released[23] > 0); /* old pins released */
}

static void test_actuation_policy(void)
{
    load_ok();
    int fan = periph_find("cooling_fan"), heater = periph_find("heater"), btn = periph_find("button");
    CHECK_EQ_INT(periph_set(fan, 1, PERIPH_BY_AGENT), PER_OK);
    CHECK_EQ_INT(g_level[23], 1);
    float v;
    CHECK_EQ_INT(periph_readback(fan, &v), PER_OK);
    CHECK(v == 1);
    CHECK_EQ_INT(periph_set(fan, 0, PERIPH_BY_AGENT), PER_ERR_RATE); /* min_toggle */
    test_clock_advance(1500);
    CHECK_EQ_INT(periph_set(fan, 0, PERIPH_BY_AGENT), PER_OK);
    CHECK_EQ_INT(periph_set(fan, 0.5f, PERIPH_BY_AGENT), PER_ERR_RANGE);
    CHECK_EQ_INT(periph_set(heater, 1, PERIPH_BY_AGENT), PER_ERR_NOT_AUTHORIZED); /* llm_write false */
    CHECK_EQ_INT(periph_set(heater, 1, PERIPH_BY_LOCAL), PER_OK);
    CHECK_EQ_INT(g_level[24], 0); /* active low */
    CHECK_EQ_INT(periph_set(btn, 1, PERIPH_BY_LOCAL), PER_ERR_TYPE);
    int blower = periph_find("blower");
    CHECK_EQ_INT(periph_set(blower, 90, PERIPH_BY_AGENT), PER_ERR_RANGE);
    CHECK_EQ_INT(periph_set(blower, 50, PERIPH_BY_AGENT), PER_OK);
    CHECK(fabsf(g_duty[0] - 50) < 0.01f);
    int led = periph_find("status_led");
    CHECK_EQ_INT(periph_set_rgb(led, 255, 0, 0, PERIPH_BY_AGENT), PER_OK);
    CHECK_EQ_INT(g_rgb[0], 76); /* capped at max 30 % */
    /* lock */
    periph_lock(fan, 2);
    test_clock_advance(1500);
    CHECK_EQ_INT(periph_set(fan, 1, PERIPH_BY_AGENT), PER_ERR_LOCKED);
    CHECK_EQ_INT(periph_set(fan, 1, PERIPH_BY_LOCAL), PER_ERR_LOCKED);
    CHECK_EQ_INT(periph_set(fan, 1, 3), PER_ERR_LOCKED); /* other rule */
    CHECK_EQ_INT(periph_set(fan, 1, 2), PER_OK);          /* owning rule */
    periph_unlock(fan, 2);
    /* hardware failure */
    g_gpio_fail_pin = 23;
    test_clock_advance(1500);
    CHECK_EQ_INT(periph_set(fan, 0, PERIPH_BY_AGENT), PER_ERR_HW);
    g_gpio_fail_pin = -1;
    periph_all_safe();
    CHECK_EQ_INT(g_level[23], 0);
    CHECK_EQ_INT(g_level[24], 1);
    CHECK(g_duty[0] == 0);
    CHECK_EQ_INT(g_rgb[0] + g_rgb[1] + g_rgb[2], 0);
}

static void test_max_on_watchdog(void)
{
    load_ok();
    int fan = periph_find("cooling_fan");
    CHECK_EQ_INT(periph_set(fan, 1, PERIPH_BY_AGENT), PER_OK);
    CHECK_EQ_INT(periph_tick(lla_now_ms() + 30000), 0);
    CHECK_EQ_INT(periph_tick(lla_now_ms() + 61000), 1);
    CHECK_EQ_INT(g_level[23], 0);
}

static void test_describe_bounded(void)
{
    load_ok(); /* 11 devices */
    char buf[300];
    lla_jw_t w;
    lla_jw_init(&w, buf, sizeof(buf));
    lla_jw_obj_begin(&w);
    lla_jw_key(&w, "devices");
    int omitted = periph_describe(&w, 64);
    CHECK(omitted > 0 && omitted < 11);
    lla_jw_kv_int(&w, "devices_omitted", omitted);
    lla_jw_obj_end(&w);
    CHECK(lla_jw_ok(&w));
    lla_jtok_t t[128];
    CHECK(lla_json_parse(buf, strlen(buf), t, 128) > 0); /* partial list, still valid JSON */
    CHECK_CONTAINS(buf, "status_led");
    char big[4096];
    lla_jw_init(&w, big, sizeof(big));
    lla_jw_obj_begin(&w);
    lla_jw_key(&w, "devices");
    CHECK_EQ_INT(periph_describe(&w, 64), 0);
    lla_jw_obj_end(&w);
    CHECK(lla_jw_ok(&w));
}

static void test_sensors(void)
{
    load_ok();
    float v, v2;
    g_adc_mv = 1650; /* NTC equal to series resistor → 25 C */
    CHECK_EQ_INT(periph_read(periph_find("room_temp"), &v, &v2), PER_OK);
    CHECK(fabsf(v - 25.0f) < 0.05f);
    g_adc_mv = 3300; /* open circuit */
    CHECK_EQ_INT(periph_read(periph_find("room_temp"), &v, &v2), PER_ERR_HW);
    g_adc_mv = 1000;
    CHECK_EQ_INT(periph_read(periph_find("light"), &v, NULL), PER_OK);
    CHECK(fabsf(v - 95.0f) < 0.01f);
    CHECK_EQ_INT(periph_read(periph_find("chip_temp"), &v, NULL), PER_OK);
    CHECK(fabsf(v - 41.5f) < 0.01f);
    CHECK_EQ_INT(periph_read(periph_find("env"), &v, &v2), PER_OK);
    CHECK(fabsf(v - 25.0f) < 0.01f);
    CHECK(fabsf(v2 - 50.0f) < 0.01f);
    g_level[28] = 0; /* active-low button pressed */
    CHECK_EQ_INT(periph_read(periph_find("button"), &v, NULL), PER_OK);
    CHECK(v == 1);
}

static void test_bus_authorization(void)
{
    load_ok();
    int ex = periph_find("expander");
    uint8_t buf[4];
    CHECK_EQ_INT(periph_i2c_read(ex, 0x10, buf, 2, 50), PER_OK);
    CHECK_EQ_INT(buf[1], 0xA1);
    uint8_t d = 0x55;
    CHECK_EQ_INT(periph_i2c_write(ex, 2, &d, 1, 50), PER_ERR_NOT_AUTHORIZED);
    CHECK_EQ_INT(periph_i2c_write(ex, 3, &d, 1, 50), PER_OK);
    CHECK_EQ_INT(g_i2c_last[0], 3);
    g_i2c_fail = HAL_ERR_IO;
    CHECK_EQ_INT(periph_i2c_write(ex, 3, &d, 1, 50), PER_ERR_UNCERTAIN); /* write failures are uncertain */
    g_i2c_fail = 0;
    CHECK_EQ_INT(periph_i2c_read(periph_find("env"), 0, buf, 2, 50), PER_ERR_TYPE);
    int disp = periph_find("display");
    CHECK_EQ_INT(periph_uart_send(disp, (const uint8_t *)"hello", 5, 50), PER_OK);
    CHECK_EQ_INT(periph_uart_send(disp, (const uint8_t *)"0123456789abcdefg", 17, 50), PER_ERR_RANGE);
}

/* periph tools through the registry */
static tool_registry_t s_reg;

static void invoke(const char *tool, const char *args, tool_source_t src, tool_result_t *r)
{
    int idx = tool_registry_find(&s_reg, tool, strlen(tool));
    tool_registry_invoke(&s_reg, idx, src, args, strlen(args), lla_now_ms(), r, NULL, NULL);
}

static int s_watch_calls;
static int watch_fn(int idx, bool en)
{
    (void)idx; (void)en;
    s_watch_calls++;
    return PER_OK;
}

static void test_periph_tools(void)
{
    load_ok();
    tool_registry_init(&s_reg);
    s_reg.policy.min_interval_ms = 0;
    CHECK(periph_tools_register(&s_reg) > 10);
    periph_tools_set_watch_fn(watch_fn);
    tool_result_t r;
    invoke("get_registered_capabilities", "{}", TOOL_SRC_CONSOLE, &r);
    CHECK_EQ_INT(r.status, TOOL_ST_OK);
    CHECK_CONTAINS(r.json, "\"name\":\"cooling_fan\"");
    invoke("set_actuator", "{\"device\":\"cooling_fan\",\"value\":1}", TOOL_SRC_CONSOLE, &r);
    CHECK_EQ_INT(r.status, TOOL_ST_OK);
    CHECK(r.verified);
    CHECK_CONTAINS(r.json, "\"verified\":true");
    invoke("set_actuator", "{\"device\":\"heater\",\"value\":1}", TOOL_SRC_CONSOLE, &r);
    CHECK_EQ_INT(r.status, TOOL_ST_REJECTED);
    CHECK_CONTAINS(r.json, "not_authorized_by_local_policy");
    invoke("set_actuator", "{\"device\":\"nope\",\"value\":1}", TOOL_SRC_CONSOLE, &r);
    CHECK_CONTAINS(r.json, "device_not_found");
    invoke("set_actuator", "{\"device\":\"status_led\",\"value\":100,\"color\":\"#00ff00\"}", TOOL_SRC_CONSOLE, &r);
    CHECK_EQ_INT(r.status, TOOL_ST_OK);
    CHECK_EQ_INT(g_rgb[1], 76);
    invoke("set_actuator", "{\"device\":\"status_led\",\"value\":100,\"color\":\"green\"}", TOOL_SRC_CONSOLE, &r);
    CHECK_EQ_INT(r.status, TOOL_ST_REJECTED);
    invoke("read_sensor", "{\"device\":\"env\"}", TOOL_SRC_AUTONOMOUS, &r);
    CHECK_CONTAINS(r.json, "humidity_pct");
    invoke("gpio_write", "{\"device\":\"cooling_fan\",\"level\":1}", TOOL_SRC_AUTONOMOUS, &r);
    CHECK_CONTAINS(r.json, "permission_denied");
    /* postcondition failure → unknown outcome */
    test_clock_advance(2000);
    g_gpio_fail_pin = -1;
    invoke("gpio_write", "{\"device\":\"cooling_fan\",\"level\":0}", TOOL_SRC_CONSOLE, &r);
    CHECK_EQ_INT(r.status, TOOL_ST_OK);
    /* gpio_configure only on spare pins */
    invoke("gpio_configure", "{\"pin\":17,\"mode\":\"output\"}", TOOL_SRC_CONSOLE, &r);
    CHECK_CONTAINS(r.json, "not_authorized");
    invoke("gpio_configure", "{\"pin\":8,\"mode\":\"output\"}", TOOL_SRC_CONSOLE, &r);
    CHECK_EQ_INT(r.status, TOOL_ST_OK);
    CHECK_CONTAINS(r.json, "gpio8");
    invoke("gpio_write", "{\"device\":\"gpio8\",\"level\":1}", TOOL_SRC_CONSOLE, &r);
    CHECK_EQ_INT(r.status, TOOL_ST_OK);
    CHECK_EQ_INT(g_level[8], 1);
    invoke("gpio_watch", "{\"device\":\"button\"}", TOOL_SRC_CONSOLE, &r);
    CHECK_EQ_INT(s_watch_calls, 1);
    invoke("i2c_write", "{\"device\":\"expander\",\"reg\":3,\"hex\":\"0aff\"}", TOOL_SRC_CONSOLE, &r);
    CHECK_EQ_INT(r.status, TOOL_ST_OK);
    CHECK_EQ_INT(g_i2c_last_n, 3);
    invoke("i2c_write", "{\"device\":\"expander\",\"reg\":3,\"hex\":\"0g\"}", TOOL_SRC_CONSOLE, &r);
    CHECK_EQ_INT(r.status, TOOL_ST_REJECTED);
    g_i2c_fail = HAL_ERR_TIMEOUT;
    invoke("i2c_write", "{\"device\":\"expander\",\"reg\":1,\"hex\":\"01\"}", TOOL_SRC_CONSOLE, &r);
    CHECK_EQ_INT(r.status, TOOL_ST_UNKNOWN);
    g_i2c_fail = 0;
    invoke("uart_send", "{\"device\":\"display\",\"text\":\"hi\\u0007\"}", TOOL_SRC_CONSOLE, &r);
    CHECK_EQ_INT(r.status, TOOL_ST_REJECTED);
    invoke("adc_read", "{\"device\":\"light\"}", TOOL_SRC_CONSOLE, &r);
    CHECK_CONTAINS(r.json, "\"mv\"");
}

/* ------------------------------------------------------------ rules */

static event_t s_events[16];
static int s_nev;
static void emit(const event_t *e)
{
    if (s_nev < 16) s_events[s_nev++] = *e;
}
static int act(int a, float v, int rule) { return periph_set(a, v, rule); }
static int lockf(int a, int rule, bool lock) { return lock ? periph_lock(a, rule) : periph_unlock(a, rule); }

static void rules_setup(rules_t *rs, const char *json)
{
    load_ok();
    rules_env_t env = {.actuate = act, .lock = lockf, .emit = emit, .find_dev = periph_find};
    rules_init(rs, &env);
    s_nev = 0;
    static lla_jtok_t t[128];
    int n = lla_json_parse(json, strlen(json), t, 128);
    CHECK(n > 0);
    char err[96] = "";
    int rc = rules_parse(rs, json, t, 0, err, sizeof(err));
    if (rc) fprintf(stderr, "    rules error: %s\n", err);
    CHECK_EQ_INT(rc, 0);
}

static void test_rules_hysteresis_and_lock(void)
{
    rules_t rs;
    rules_setup(&rs, "[{\"name\":\"cooling\",\"sensor\":\"room_temp\",\"op\":\">\",\"threshold\":32,\"hysteresis\":2,"
                     "\"actuator\":\"cooling_fan\",\"on\":1,\"off\":0,\"safety\":true,\"stale_s\":30,\"fault\":1}]");
    int fan = periph_find("cooling_fan"), temp = periph_find("room_temp");
    uint64_t t = lla_now_ms();
    rules_eval(&rs, temp, 31.0f, t);
    CHECK_EQ_INT(g_level[23], 0);
    rules_eval(&rs, temp, 32.5f, t + 1000);
    CHECK_EQ_INT(g_level[23], 1);
    CHECK_EQ_INT(periph_get(fan)->locked_by, 0);
    /* the agent cannot switch the fan off while the safety rule holds it */
    test_clock_advance(5000);
    CHECK_EQ_INT(periph_set(fan, 0, PERIPH_BY_AGENT), PER_ERR_LOCKED);
    rules_eval(&rs, temp, 31.0f, t + 2000); /* inside hysteresis band */
    CHECK_EQ_INT(g_level[23], 1);
    rules_eval(&rs, temp, 29.9f, t + 3000);
    CHECK_EQ_INT(g_level[23], 0);
    CHECK_EQ_INT(periph_get(fan)->locked_by, -1);
    CHECK_EQ_INT(s_nev, 2);
    CHECK_EQ_INT(s_events[0].type, EV_RULE_ON);
    CHECK_EQ_INT(s_events[1].type, EV_RULE_OFF);
    /* stale sensor → fail-safe ON + lock */
    rules_tick(&rs, t + 20000);
    CHECK_EQ_INT(g_level[23], 0);
    rules_tick(&rs, t + 34000);
    CHECK_EQ_INT(g_level[23], 1);
    CHECK(rs.r[0].faulted);
    CHECK_EQ_INT(periph_get(fan)->locked_by, 0);
    CHECK_EQ_INT(s_events[2].type, EV_RULE_FAULT);
    /* sensor recovers and is cool → released */
    rules_eval(&rs, temp, 25.0f, t + 35000);
    CHECK(!rs.r[0].faulted);
    CHECK_EQ_INT(periph_get(fan)->locked_by, -1);
}

static void test_rules_validation(void)
{
    rules_t rs;
    load_ok();
    rules_env_t env = {.actuate = act, .lock = lockf, .emit = emit, .find_dev = periph_find};
    rules_init(&rs, &env);
    lla_jtok_t t[64];
    char err[96];
    const char *bad[] = {
        "[{\"name\":\"a\",\"sensor\":\"nope\",\"op\":\">\",\"threshold\":1,\"actuator\":\"cooling_fan\"}]",
        "[{\"name\":\"a\",\"sensor\":\"room_temp\",\"op\":\"=\",\"threshold\":1,\"actuator\":\"cooling_fan\"}]",
        "[{\"name\":\"a\",\"sensor\":\"room_temp\",\"op\":\">\",\"actuator\":\"cooling_fan\"}]",
        ("[{\"name\":\"a\",\"sensor\":\"room_temp\",\"op\":\">\",\"threshold\":1,\"actuator\":\"cooling_fan\",\"safety\":true},"
         "{\"name\":\"b\",\"sensor\":\"room_temp\",\"op\":\"<\",\"threshold\":1,\"actuator\":\"cooling_fan\",\"safety\":true}]"),
    };
    for (size_t i = 0; i < 4; i++) {
        CHECK(lla_json_parse(bad[i], strlen(bad[i]), t, 64) > 0);
        CHECK_EQ_INT(rules_parse(&rs, bad[i], t, 0, err, sizeof(err)), -1);
    }
}

/* Deterministic automation keeps working while the LLM/network is down:
 * the rule path has no dependency on the agent or transport at all. */
static void test_rules_offline(void)
{
    rules_t rs;
    rules_setup(&rs, "[{\"name\":\"cool\",\"sensor\":\"chip_temp\",\"op\":\">\",\"threshold\":50,\"hysteresis\":5,"
                     "\"actuator\":\"cooling_fan\",\"safety\":true}]");
    int ct = periph_find("chip_temp");
    float v;
    g_chip = 55;
    CHECK_EQ_INT(periph_read(ct, &v, NULL), PER_OK);
    rules_eval(&rs, ct, v, lla_now_ms());
    CHECK_EQ_INT(g_level[23], 1);
    g_chip = 40;
    CHECK_EQ_INT(periph_read(ct, &v, NULL), PER_OK);
    rules_eval(&rs, ct, v, lla_now_ms());
    CHECK_EQ_INT(g_level[23], 0);
}

/* ------------------------------------------------------------ events */

static void test_event_queue(void)
{
    evq_t q;
    evq_init(&q, 1000);
    event_t e = {.type = EV_GPIO_EDGE, .dev = 3, .value = 1, .ts = 100};
    CHECK(evq_push(&q, &e));
    e.ts = 500;
    CHECK(!evq_push(&q, &e)); /* duplicate within window */
    e.ts = 1200;
    CHECK(evq_push(&q, &e));
    CHECK_EQ_INT(q.deduped, 1);
    for (int i = 0; i < 20; i++) {
        event_t s = {.type = EV_SENSOR, .dev = 1, .value = 20, .ts = 2000};
        evq_push(&q, &s);
    }
    CHECK_EQ_INT(q.count, EVQ_LEN);
    CHECK_EQ_INT(q.dropped, 6);
    event_t out;
    int n = 0;
    while (evq_pop(&q, &out)) n++;
    CHECK_EQ_INT(n, EVQ_LEN);
}

static void test_debounce(void)
{
    debounce_t d = {.stable = -1};
    int lvl;
    CHECK(!debounce_poll(&d, 1, 0, 30, &lvl)); /* baseline */
    debounce_edge(&d, 100);
    CHECK(!debounce_poll(&d, 0, 110, 30, &lvl)); /* still bouncing */
    debounce_edge(&d, 115);
    CHECK(!debounce_poll(&d, 0, 140, 30, &lvl));
    CHECK(debounce_poll(&d, 0, 150, 30, &lvl));
    CHECK_EQ_INT(lvl, 0);
    debounce_edge(&d, 200); /* glitch that returns to stable */
    CHECK(!debounce_poll(&d, 0, 240, 30, &lvl));
}

static void test_triggers(void)
{
    load_ok();
    const char *js = "[{\"on\":\"gpio_edge\",\"device\":\"button\",\"value\":1,\"min_interval_s\":30,\"prompt\":\"Button pressed\"}]";
    lla_jtok_t t[32];
    CHECK(lla_json_parse(js, strlen(js), t, 32) > 0);
    triggers_t ts;
    char err[96];
    CHECK_EQ_INT(triggers_parse(&ts, js, t, 0, periph_find, err, sizeof(err)), 0);
    event_t e = {.type = EV_GPIO_EDGE, .dev = (int8_t)periph_find("button"), .value = 1, .ts = 1};
    CHECK(triggers_match(&ts, &e, 10000) != NULL);
    CHECK(triggers_match(&ts, &e, 20000) == NULL); /* rate limited */
    CHECK_EQ_INT(ts.t[0].suppressed, 1);
    CHECK(triggers_match(&ts, &e, 41000) != NULL);
    e.value = 0;
    CHECK(triggers_match(&ts, &e, 90000) == NULL);
    const char *bad = "[{\"on\":\"earthquake\",\"prompt\":\"x\"}]";
    CHECK(lla_json_parse(bad, strlen(bad), t, 32) > 0);
    CHECK_EQ_INT(triggers_parse(&ts, bad, t, 0, periph_find, err, sizeof(err)), -1);
}

/* ------------------------------------------------------------ config */

static struct {
    char ns[16], key[16];
    uint8_t v[600];
    size_t n;
    bool used;
} s_kv[16];
static int s_kv_writes;

static int kv_find(const char *ns, const char *key)
{
    for (int i = 0; i < 16; i++)
        if (s_kv[i].used && !strcmp(s_kv[i].ns, ns) && !strcmp(s_kv[i].key, key)) return i;
    return -1;
}
static int kv_get(const char *ns, const char *key, void *buf, size_t *len)
{
    int i = kv_find(ns, key);
    if (i < 0) return CFG_NOT_FOUND;
    if (*len < s_kv[i].n) return CFG_TOO_SMALL;
    memcpy(buf, s_kv[i].v, s_kv[i].n);
    *len = s_kv[i].n;
    return CFG_OK;
}
static int kv_set(const char *ns, const char *key, const void *buf, size_t len)
{
    int i = kv_find(ns, key);
    for (int j = 0; i < 0 && j < 16; j++)
        if (!s_kv[j].used) i = j;
    if (i < 0 || len > sizeof(s_kv[i].v)) return CFG_IO;
    s_kv[i].used = true;
    snprintf(s_kv[i].ns, 16, "%s", ns);
    snprintf(s_kv[i].key, 16, "%s", key);
    memcpy(s_kv[i].v, buf, len);
    s_kv[i].n = len;
    s_kv_writes++;
    return CFG_OK;
}
static int kv_erase(const char *ns, const char *key)
{
    int i = kv_find(ns, key);
    if (i >= 0) s_kv[i].used = false;
    return CFG_OK;
}
static int kv_erase_ns(const char *ns)
{
    for (int i = 0; i < 16; i++)
        if (!strcmp(s_kv[i].ns, ns)) s_kv[i].used = false;
    return CFG_OK;
}
static const cfg_backend_t k_kv = {kv_get, kv_set, kv_erase, kv_erase_ns};

static void test_config(void)
{
    memset(s_kv, 0, sizeof(s_kv));
    s_kv_writes = 0;
    cfg_init(&k_kv);
    CHECK_EQ_INT(cfg_set_str(CFG_NS, "wifi_ssid", "lab"), CFG_OK);
    CHECK_EQ_INT(cfg_set_str(CFG_NS, "wifi_ssid", "lab"), CFG_UNCHANGED);
    CHECK_EQ_INT(s_kv_writes, 1);
    char b[32];
    CHECK_EQ_INT(cfg_get_str(CFG_NS, "wifi_ssid", b, sizeof(b)), CFG_OK);
    CHECK_STR(b, "lab");
    CHECK_EQ_INT(cfg_get_str(CFG_NS, "missing", b, sizeof(b)), CFG_NOT_FOUND);
    CHECK_EQ_INT(cfg_get_str(CFG_NS, "wifi_ssid", b, 2), CFG_TOO_SMALL);
    /* hourly write budget */
    int limited = 0;
    for (int i = 0; i < 100; i++) {
        char v[8];
        snprintf(v, sizeof(v), "%d", i);
        if (cfg_set_str(CFG_NS, "counter", v) == CFG_RATE_LIMITED) limited++;
    }
    CHECK(limited > 0);
    CHECK(s_kv_writes <= CFG_MAX_WRITES_PER_HOUR);
    test_clock_advance(3600001);
    CHECK_EQ_INT(cfg_set_str(CFG_NS, "counter", "x"), CFG_OK);
    char red[32];
    cfg_redact("sk-ant-supersecret", red, sizeof(red));
    CHECK_STR(red, "<set, 18 chars>");
    CHECK(strstr(red, "sk") == NULL);
    cfg_redact("", red, sizeof(red));
    CHECK_STR(red, "<unset>");
    CHECK_EQ_INT(cfg_factory_reset(), CFG_OK);
    CHECK_EQ_INT(cfg_get_str(CFG_NS, "wifi_ssid", b, sizeof(b)), CFG_NOT_FOUND);
}

/* ------------------------------------------------------------ registry */

static void x_noop(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)dl;
    lla_jw_kv_str(&r->w, "s", a->a[0].present ? a->a[0].v.s : "");
}

static void test_registry(void)
{
    static const char *const ev[] = {"a", "b", NULL};
    static const tool_arg_def_t args[] = {{.name = "mode", .type = TARG_ENUM, .required = true, .enum_vals = ev},
                                          {.name = "n", .type = TARG_INT, .min = -5, .max = 5},
                                          {.name = "f", .type = TARG_NUM, .min = 0, .max = 1},
                                          {.name = "b", .type = TARG_BOOL},
                                          {.name = "s", .type = TARG_STR, .max_len = 4}};
    static const tool_def_t t1 = {.name = "t_one", .desc = "d", .args = args, .n_args = 5, .exec = x_noop};
    static const tool_def_t bad = {.name = "Bad-Name", .exec = x_noop};
    static const tool_def_t danger = {.name = "zap", .desc = "d", .perm = TOOL_PERM_DANGEROUS, .exec = x_noop};
    tool_registry_t r;
    tool_registry_init(&r);
    CHECK_EQ_INT(tool_registry_add(&r, &t1), 0);
    CHECK_EQ_INT(tool_registry_add(&r, &t1), -1);
    CHECK_EQ_INT(tool_registry_add(&r, &bad), -1);
    CHECK_EQ_INT(tool_registry_add(&r, &danger), 1);
    tool_args_t a;
    char why[80];
    const char *ok = "{\"mode\":\"a\",\"n\":-5,\"f\":0.5,\"b\":true,\"s\":\"abcd\"}";
    CHECK(tool_args_parse(&t1, ok, strlen(ok), &a, why, sizeof(why)));
    CHECK_EQ_INT(tool_arg_int(&t1, &a, "n", 0), -5);
    CHECK(tool_arg_bool(&t1, &a, "b", false));
    const char *cases[] = {"{\"mode\":\"c\"}",        "{}",         "{\"mode\":\"a\",\"n\":6}",
                           "{\"mode\":\"a\",\"n\":1.5}", "{\"mode\":\"a\",\"s\":\"abcde\"}",
                           "{\"mode\":\"a\",\"b\":1}",   "[1]",        "{\"mode\":\"a\",\"mode\":\"b\"}",
                           "{\"mode\":\"a\",\"f\":\"x\"}", "{\"mode\":\"a\""};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        CHECK(!tool_args_parse(&t1, cases[i], strlen(cases[i]), &a, why, sizeof(why)));
    CHECK(tool_args_parse(&t1, "{\"mode\":\"a\",\"n\":3.0}", 20, &a, why, sizeof(why))); /* integral float ok */
    /* dangerous tool not permitted for console by default, not advertised */
    CHECK(!tool_registry_permitted(&r, 1, TOOL_SRC_CONSOLE));
    char buf[2048];
    lla_jw_t w;
    lla_jw_init(&w, buf, sizeof(buf));
    tool_registry_write_schemas(&r, TOOL_SRC_CONSOLE, TOOL_FMT_OPENAI, &w);
    CHECK(lla_jw_ok(&w));
    CHECK(strstr(buf, "zap") == NULL);
    CHECK_CONTAINS(buf, "\"enum\":[\"a\",\"b\"]");
    CHECK_CONTAINS(buf, "\"additionalProperties\":false");
    lla_jtok_t t[256];
    CHECK(lla_json_parse(buf, strlen(buf), t, 256) > 0);
    r.policy.ceiling[TOOL_SRC_CONSOLE] = TOOL_PERM_DANGEROUS;
    CHECK(tool_registry_permitted(&r, 1, TOOL_SRC_CONSOLE));
    r.policy.deny_mask = 1ULL << 1;
    CHECK(!tool_registry_permitted(&r, 1, TOOL_SRC_CONSOLE));
    /* result truncation keeps valid JSON */
    tool_result_t res;
    tool_result_begin(&res);
    for (int i = 0; i < 100; i++) lla_jw_kv_str(&res.w, "padding", "xxxxxxxxxxxxxxxx");
    tool_result_end(&res);
    CHECK_CONTAINS(res.json, "result_truncated");
    CHECK(lla_json_parse(res.json, strlen(res.json), t, 256) > 0);
}

/* ------------------------------------------------------------ conv */

static void test_conv(void)
{
    uint8_t buf[256];
    llm_conv_t c;
    llm_conv_init(&c, buf, sizeof(buf));
    CHECK(llm_conv_add_user(&c, 1, "hello", 5));
    llm_call_view_t calls[2] = {{{"id1", 3}, {"read_sensor", 11}, {"{}", 2}}, {{"id2", 3}, {"gpio_read", 9}, {"{\"a\":1}", 7}}};
    CHECK(llm_conv_add_assistant(&c, 1, "", 0, calls, 2));
    llm_result_view_t res[2] = {{{"id1", 3}, {"read_sensor", 11}, {"{\"v\":1}", 7}, false},
                                {{"id2", 3}, {"gpio_read", 9}, {"{\"e\":1}", 7}, true}};
    CHECK(llm_conv_add_results(&c, 1, res, 2));
    CHECK_EQ_INT(llm_conv_count(&c), 3);
    uint16_t pos = 0;
    llm_msg_view_t m;
    CHECK(llm_conv_next(&c, &pos, &m) && m.kind == LLM_MSG_USER && m.text.n == 5);
    CHECK(llm_conv_next(&c, &pos, &m) && m.kind == LLM_MSG_ASSISTANT && m.n == 2);
    CHECK(m.calls[1].args.n == 7 && memcmp(m.calls[1].args.p, "{\"a\":1}", 7) == 0);
    CHECK(llm_conv_next(&c, &pos, &m) && m.kind == LLM_MSG_TOOL_RESULTS && m.results[1].is_error);
    CHECK(!llm_conv_next(&c, &pos, &m));
    /* run 2 forces eviction of run 1 as a whole */
    char big[150];
    memset(big, 'y', sizeof(big));
    CHECK(llm_conv_add_user(&c, 2, big, sizeof(big)));
    CHECK_EQ_INT(llm_conv_count(&c), 1);
    CHECK(!llm_conv_add_user(&c, 2, big, sizeof(big))); /* same run can't evict itself */
    llm_conv_drop_run(&c, 2);
    CHECK_EQ_INT(c.len, 0);
}

static void check_profile_file(const char *path)
{
    hal_set(&k_mock_hal);
    periph_init(&board_esp32c5_devkitc1);
    size_t n;
    char *js = test_read_file(path, &n);
    char err[128] = "";
    CHECK_EQ_INT(periph_load_profile(js, n, err, sizeof(err)), PER_OK);
    CHECK_STR(err, "");
    lla_jtok_t t[256];
    CHECK(lla_json_parse(js, n, t, 256) > 0);
    rules_t rs;
    rules_env_t env = {.actuate = act, .lock = lockf, .emit = emit, .find_dev = periph_find};
    rules_init(&rs, &env);
    CHECK_EQ_INT(rules_parse(&rs, js, t, lla_json_obj_get(js, t, 0, "rules"), err, sizeof(err)), 0);
    triggers_t ts;
    CHECK_EQ_INT(triggers_parse(&ts, js, t, lla_json_obj_get(js, t, 0, "triggers"), periph_find, err, sizeof(err)), 0);
    if (err[0]) fprintf(stderr, "    %s: %s\n", path, err);
    free(js);
}

/* The shipped default and every example profile must validate. */
static void test_default_profile_loads(void)
{
    check_profile_file("../../../main/default_profile.json");
    check_profile_file("../../../examples/led_agent/profile.json");
    check_profile_file("../../../examples/temperature_agent/profile.json");
    check_profile_file("../../../examples/ble_sensor_agent/profile.json");
    check_profile_file("../../../examples/autonomous_controller/profile.json");
}

void suite_periph(void)
{
    RUN(test_default_profile_loads);
    RUN(test_profile_validation);
    RUN(test_actuation_policy);
    RUN(test_max_on_watchdog);
    RUN(test_sensors);
    RUN(test_describe_bounded);
    RUN(test_bus_authorization);
    RUN(test_periph_tools);
}

void suite_rules(void)
{
    RUN(test_rules_hysteresis_and_lock);
    RUN(test_rules_validation);
    RUN(test_rules_offline);
}

static void test_ble_decoders(void)
{
    double v;
    const char *u;
    const uint8_t t[2] = {0x2c, 0x09}; /* 2348 → 23.48 C */
    CHECK(ble_decode_known(0x2A6E, t, 2, &v, &u) && v > 23.47 && v < 23.49 && !strcmp(u, "C"));
    const uint8_t tn[2] = {0x18, 0xfc}; /* -1000 → -10.00 C */
    CHECK(ble_decode_known(0x2A6E, tn, 2, &v, &u) && v < -9.99 && v > -10.01);
    const uint8_t tu[2] = {0x00, 0x80}; /* 0x8000 = unknown */
    CHECK(!ble_decode_known(0x2A6E, tu, 2, &v, &u));
    CHECK(!ble_decode_known(0x2A6E, t, 1, &v, &u)); /* wrong length */
    const uint8_t h[2] = {0x88, 0x13}; /* 5000 → 50.00 % */
    CHECK(ble_decode_known(0x2A6F, h, 2, &v, &u) && v > 49.99 && v < 50.01);
    const uint8_t hu[2] = {0xff, 0xff};
    CHECK(!ble_decode_known(0x2A6F, hu, 2, &v, &u));
    const uint8_t b = 87, bad = 101;
    CHECK(ble_decode_known(0x2A19, &b, 1, &v, &u) && v == 87);
    CHECK(!ble_decode_known(0x2A19, &bad, 1, &v, &u));
    const uint8_t p[4] = {0x40, 0x42, 0x0f, 0x00}; /* 1,000,000 → 100000.0 Pa */
    CHECK(ble_decode_known(0x2A6D, p, 4, &v, &u) && v > 99999.9 && v < 100000.1);
    CHECK(!ble_decode_known(0x2A00, p, 4, &v, &u)); /* unknown uuid → raw hex only */
}

void suite_events(void)
{
    RUN(test_ble_decoders);
    RUN(test_event_queue);
    RUN(test_debounce);
    RUN(test_triggers);
}

void suite_config(void) { RUN(test_config); }
void suite_registry(void) { RUN(test_registry); }
void suite_conv(void) { RUN(test_conv); }

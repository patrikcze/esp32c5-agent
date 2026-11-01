/* Minimal test harness for host tests (no external dependency). */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int g_test_failures;
extern int g_test_checks;
extern int g_test_log_level;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        g_test_checks++;                                                              \
        if (!(cond)) {                                                                \
            g_test_failures++;                                                        \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                             \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                                              \
    do {                                                                                                \
        long long _a = (long long)(a), _b = (long long)(b);                                             \
        g_test_checks++;                                                                                \
        if (_a != _b) {                                                                                 \
            g_test_failures++;                                                                          \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); \
        }                                                                                               \
    } while (0)

#define CHECK_STR(a, b)                                                                                   \
    do {                                                                                                  \
        const char *_a = (a), *_b = (b);                                                                  \
        g_test_checks++;                                                                                  \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                                                          \
            g_test_failures++;                                                                            \
            fprintf(stderr, "  FAIL %s:%d: %s == \"%s\" (got \"%s\")\n", __FILE__, __LINE__, #a, _b ? _b : "(null)", \
                    _a ? _a : "(null)");                                                                  \
        }                                                                                                 \
    } while (0)

#define CHECK_CONTAINS(hay, needle)                                                                     \
    do {                                                                                                \
        const char *_h = (hay);                                                                         \
        g_test_checks++;                                                                                \
        if (!_h || !strstr(_h, needle)) {                                                               \
            g_test_failures++;                                                                          \
            fprintf(stderr, "  FAIL %s:%d: \"%s\" not in: %.300s\n", __FILE__, __LINE__, needle, _h ? _h : ""); \
        }                                                                                               \
    } while (0)

typedef void (*test_fn)(void);
#define RUN(fn)                                       \
    do {                                              \
        int _before = g_test_failures;                \
        fn();                                         \
        fprintf(stderr, "%s %s\n", g_test_failures == _before ? "PASS" : "FAIL", #fn); \
    } while (0)

void test_clock_advance(uint64_t ms);
uint64_t test_clock_slept(void);
void test_clock_reset_slept(void);
char *test_read_file(const char *name, size_t *len);

/* Suites */
void suite_json(void);
void suite_registry(void);
void suite_openai(void);
void suite_anthropic(void);
void suite_conv(void);
void suite_agent(void);
void suite_periph(void);
void suite_rules(void);
void suite_events(void);
void suite_config(void);

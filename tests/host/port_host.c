/* Host implementation of lla_port.h with a controllable mock clock. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "lla_port.h"
#include "test.h"

static uint64_t s_now = 1000;
static uint64_t s_slept;
static uint32_t s_rand = 12345;
int g_test_log_level = LLA_LOG_WARN;

uint64_t lla_now_ms(void) { return s_now; }
void lla_sleep_ms(uint32_t ms)
{
    s_now += ms;
    s_slept += ms;
}
void test_clock_advance(uint64_t ms) { s_now += ms; }
uint64_t test_clock_slept(void) { return s_slept; }
void test_clock_reset_slept(void) { s_slept = 0; }

uint32_t lla_rand32(void)
{
    s_rand = s_rand * 1103515245u + 12345u;
    return s_rand >> 8;
}

void lla_log(lla_log_level_t lvl, const char *tag, const char *fmt, ...)
{
    if ((int)lvl > g_test_log_level) return;
    static const char lc[] = "?EWID";
    fprintf(stderr, "    [%c %s] ", lc[lvl], tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void lla_lock_init(lla_lock_t *l) { l->impl = NULL; }
void lla_lock(lla_lock_t *l) { (void)l; }
void lla_unlock(lla_lock_t *l) { (void)l; }

char *test_read_file(const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open fixture %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    b[n] = '\0';
    fclose(f);
    *len = (size_t)n;
    return b;
}

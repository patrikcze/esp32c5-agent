#include "test.h"

int g_test_failures;
int g_test_checks;

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;
#define SUITE(name)                                         \
    if (!only || strcmp(only, #name) == 0) {               \
        fprintf(stderr, "== suite %s\n", #name);           \
        suite_##name();                                    \
    }
    SUITE(json);
    SUITE(registry);
    SUITE(openai);
    SUITE(anthropic);
    SUITE(conv);
    SUITE(agent);
    SUITE(periph);
    SUITE(rules);
    SUITE(events);
    SUITE(config);
    fprintf(stderr, "\n%d checks, %d failures\n", g_test_checks, g_test_failures);
    return g_test_failures ? 1 : 0;
}

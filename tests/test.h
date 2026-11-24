#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

static int test_failures = 0;
static int test_checks = 0;

#define CHECK(cond) do { \
    test_checks++; \
    if (!(cond)) { test_failures++; \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_STR(a, b) do { \
    const char *_a = (a), *_b = (b); test_checks++; \
    if (!_a || !_b || strcmp(_a, _b) != 0) { test_failures++; \
        fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, \
                _a ? _a : "(null)", _b ? _b : "(null)"); } \
} while (0)

#define TEST_MAIN_END() do { \
    printf("%s: %d checks, %d failures\n", __FILE__, test_checks, test_failures); \
    return test_failures ? 1 : 0; \
} while (0)

#endif

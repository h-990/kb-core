/*
 * test_main.h — Minimal test framework for KB Core.
 *
 * No external dependencies. Each test file includes this header and
 * calls TEST_SUMMARY() at the end of main() to return 0 (pass) or 1 (fail).
 */
#ifndef TEST_MAIN_H
#define TEST_MAIN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

static int test_pass_count = 0;
static int test_fail_count = 0;
static const char *test_current_name = "";

/* Set the current test name for reporting. */
#define TEST_BEGIN(name) \
    do { test_current_name = (name); } while (0)

/* Assert a boolean condition. */
#define TEST_ASSERT(cond, msg) do {                                       \
    if (!(cond)) {                                                        \
        fprintf(stderr, "  FAIL [%s] %s:%d: %s\n",                       \
                test_current_name, __FILE__, __LINE__, (msg));            \
        test_fail_count++;                                                \
    } else {                                                              \
        test_pass_count++;                                                \
    }                                                                     \
} while (0)

/* Assert two integer-like values are equal. Uses stringification for nice
 * error messages and prints both values on failure. */
#define TEST_EQ(a, b) do {                                                \
    long long _a = (long long)(a), _b = (long long)(b);                   \
    if (_a != _b) {                                                       \
        fprintf(stderr, "  FAIL [%s] %s:%d: %s == %s : "                  \
                "got %" PRId64 ", expected %" PRId64 "\n",                 \
                test_current_name, __FILE__, __LINE__, #a, #b,            \
                (int64_t)_a, (int64_t)_b);                                \
        test_fail_count++;                                                \
    } else {                                                              \
        test_pass_count++;                                                \
    }                                                                     \
} while (0)

/* Assert two unsigned values are equal (hex output for bytes/addresses). */
#define TEST_EQ_HEX(a, b) do {                                            \
    unsigned long long _a = (unsigned long long)(a);                       \
    unsigned long long _b = (unsigned long long)(b);                       \
    if (_a != _b) {                                                       \
        fprintf(stderr, "  FAIL [%s] %s:%d: %s == %s : "                  \
                "got 0x%" PRIX64 ", expected 0x%" PRIX64 "\n",             \
                test_current_name, __FILE__, __LINE__, #a, #b,            \
                (uint64_t)_a, (uint64_t)_b);                              \
        test_fail_count++;                                                \
    } else {                                                              \
        test_pass_count++;                                                \
    }                                                                     \
} while (0)

/* Assert that a status code is KB_OK (0). */
#define TEST_OK(expr) do {                                                \
    int32_t _s = (int32_t)(expr);                                         \
    if (_s != 0) {                                                        \
        fprintf(stderr, "  FAIL [%s] %s:%d: %s returned %d (expected 0)\n",\
                test_current_name, __FILE__, __LINE__, #expr, _s);        \
        test_fail_count++;                                                \
    } else {                                                              \
        test_pass_count++;                                                \
    }                                                                     \
} while (0)

/* Print summary and return exit code. */
#define TEST_SUMMARY() do {                                               \
    printf("\n%d passed, %d failed\n", test_pass_count, test_fail_count);  \
    return test_fail_count > 0 ? 1 : 0;                                   \
} while (0)

#endif /* TEST_MAIN_H */

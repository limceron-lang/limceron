/*
 * Limceron Stage 0 — Minimal Test Framework
 *
 * Usage:
 *     TEST(test_name) {
 *         ASSERT(1 + 1 == 2);
 *         ASSERT_EQ(42, 42);
 *         ASSERT_STR_EQ("hello", "hello");
 *     }
 *
 *     int main(void) {
 *         RUN_TEST(test_name);
 *         TEST_SUMMARY();
 *         return test_failures > 0 ? 1 : 0;
 *     }
 */

#ifndef LCN_TEST_H
#define LCN_TEST_H

#include <stdio.h>
#include <string.h>
#include <math.h>

static int test_total    = 0;
static int test_passed   = 0;
static int test_failures = 0;
static int test_asserts  = 0;
static const char *current_test_name = NULL;
static int current_test_failed = 0;

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name) do {                                         \
    current_test_name = #name;                                      \
    current_test_failed = 0;                                        \
    test_total++;                                                   \
    test_##name();                                                  \
    if (current_test_failed == 0) {                                 \
        test_passed++;                                              \
        fprintf(stderr, "  \033[32m PASS \033[0m %s\n", #name);    \
    }                                                               \
} while(0)

#define ASSERT(expr) do {                                           \
    test_asserts++;                                                 \
    if (!(expr)) {                                                  \
        fprintf(stderr, "  \033[31m FAIL \033[0m %s\n", current_test_name); \
        fprintf(stderr, "         %s:%d: ASSERT(%s) failed\n",     \
                __FILE__, __LINE__, #expr);                         \
        current_test_failed = 1;                                    \
        test_failures++;                                            \
        return;                                                     \
    }                                                               \
} while(0)

#define ASSERT_EQ(a, b) do {                                        \
    test_asserts++;                                                 \
    long long _a = (long long)(a);                                  \
    long long _b = (long long)(b);                                  \
    if (_a != _b) {                                                 \
        fprintf(stderr, "  \033[31m FAIL \033[0m %s\n", current_test_name); \
        fprintf(stderr, "         %s:%d: ASSERT_EQ(%s, %s)\n",     \
                __FILE__, __LINE__, #a, #b);                        \
        fprintf(stderr, "         expected: %lld\n", _b);           \
        fprintf(stderr, "           actual: %lld\n", _a);           \
        current_test_failed = 1;                                    \
        test_failures++;                                            \
        return;                                                     \
    }                                                               \
} while(0)

#define ASSERT_NEQ(a, b) do {                                       \
    test_asserts++;                                                 \
    long long _a = (long long)(a);                                  \
    long long _b = (long long)(b);                                  \
    if (_a == _b) {                                                 \
        fprintf(stderr, "  \033[31m FAIL \033[0m %s\n", current_test_name); \
        fprintf(stderr, "         %s:%d: ASSERT_NEQ(%s, %s) both are %lld\n",\
                __FILE__, __LINE__, #a, #b, _a);                    \
        current_test_failed = 1;                                    \
        test_failures++;                                            \
        return;                                                     \
    }                                                               \
} while(0)

#define ASSERT_STR_EQ(a, b) do {                                    \
    test_asserts++;                                                 \
    const char *_a = (a);                                           \
    const char *_b = (b);                                           \
    if (_a == NULL && _b == NULL) break;                             \
    if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) {           \
        fprintf(stderr, "  \033[31m FAIL \033[0m %s\n", current_test_name); \
        fprintf(stderr, "         %s:%d: ASSERT_STR_EQ(%s, %s)\n", \
                __FILE__, __LINE__, #a, #b);                        \
        fprintf(stderr, "         expected: \"%s\"\n", _b ? _b : "(null)"); \
        fprintf(stderr, "           actual: \"%s\"\n", _a ? _a : "(null)"); \
        current_test_failed = 1;                                    \
        test_failures++;                                            \
        return;                                                     \
    }                                                               \
} while(0)

#define ASSERT_FLOAT_EQ(a, b) do {                                  \
    test_asserts++;                                                 \
    double _a = (double)(a);                                        \
    double _b = (double)(b);                                        \
    if (fabs(_a - _b) > 0.0001) {                                  \
        fprintf(stderr, "  \033[31m FAIL \033[0m %s\n", current_test_name); \
        fprintf(stderr, "         %s:%d: ASSERT_FLOAT_EQ(%s, %s)\n",\
                __FILE__, __LINE__, #a, #b);                        \
        fprintf(stderr, "         expected: %f\n", _b);             \
        fprintf(stderr, "           actual: %f\n", _a);             \
        current_test_failed = 1;                                    \
        test_failures++;                                            \
        return;                                                     \
    }                                                               \
} while(0)

#define ASSERT_NULL(a) do {                                         \
    test_asserts++;                                                 \
    if ((a) != NULL) {                                              \
        fprintf(stderr, "  \033[31m FAIL \033[0m %s\n", current_test_name); \
        fprintf(stderr, "         %s:%d: ASSERT_NULL(%s) was not null\n",\
                __FILE__, __LINE__, #a);                            \
        current_test_failed = 1;                                    \
        test_failures++;                                            \
        return;                                                     \
    }                                                               \
} while(0)

#define ASSERT_NOT_NULL(a) do {                                     \
    test_asserts++;                                                 \
    if ((a) == NULL) {                                              \
        fprintf(stderr, "  \033[31m FAIL \033[0m %s\n", current_test_name); \
        fprintf(stderr, "         %s:%d: ASSERT_NOT_NULL(%s) was null\n",\
                __FILE__, __LINE__, #a);                            \
        current_test_failed = 1;                                    \
        test_failures++;                                            \
        return;                                                     \
    }                                                               \
} while(0)

#define ASSERT_TRUE(a)  ASSERT(a)
#define ASSERT_FALSE(a) ASSERT(!(a))

#define TEST_SUMMARY() do {                                         \
    fprintf(stderr, "\n────────────────────────────────────────\n");\
    if (test_failures == 0) {                                       \
        fprintf(stderr, "\033[32m  ALL %d TESTS PASSED\033[0m (%d assertions)\n",\
                test_total, test_asserts);                           \
    } else {                                                        \
        fprintf(stderr, "\033[31m  %d of %d TESTS FAILED\033[0m (%d assertions)\n",\
                test_failures, test_total, test_asserts);            \
    }                                                               \
    fprintf(stderr, "────────────────────────────────────────\n\n");\
} while(0)

#endif /* LCN_TEST_H */

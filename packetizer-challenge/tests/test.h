#ifndef TEST_H
#define TEST_H

/* Minimal single-header test harness. One counter pair per translation unit,
 * so keep a test binary to a single .c file for now. */

#include <stdio.h>
#include <string.h>

static int test_checks = 0;
static int test_failures = 0;
static const char *test_current = "";

#define TEST(name) static void name(void)

#define RUN_TEST(name) \
    do { \
        test_current = #name; \
        name(); \
    } while (0)

#define CHECK(cond) \
    do { \
        test_checks++; \
        if (!(cond)) { \
            test_failures++; \
            fprintf(stderr, "%s:%d: [%s] CHECK failed: %s\n", \
                    __FILE__, __LINE__, test_current, #cond); \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        test_checks++; \
        long long check_eq_a = (long long)(a); \
        long long check_eq_b = (long long)(b); \
        if (check_eq_a != check_eq_b) { \
            test_failures++; \
            fprintf(stderr, "%s:%d: [%s] CHECK_EQ failed: %s = %lld, %s = %lld\n", \
                    __FILE__, __LINE__, test_current, #a, check_eq_a, #b, check_eq_b); \
        } \
    } while (0)

#define CHECK_MEM(a, b, n) \
    do { \
        test_checks++; \
        if (memcmp((a), (b), (n)) != 0) { \
            test_failures++; \
            fprintf(stderr, "%s:%d: [%s] CHECK_MEM failed: %s != %s (%zu bytes)\n", \
                    __FILE__, __LINE__, test_current, #a, #b, (size_t)(n)); \
        } \
    } while (0)

/* Call after all RUN_TEST()s and return its value from main. */
#define TEST_SUMMARY() \
    (fprintf(stderr, "%d checks, %d failed\n", test_checks, test_failures), \
     test_failures != 0)

#endif /* TEST_H */

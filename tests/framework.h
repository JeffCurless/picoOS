/*
 * MIT License with Commons Clause
 *
 * Copyright (c) 2026 Jeff Curless
 *
 * Required Notice: Copyright (c) 2026 Jeff Curless.
 *
 * This software is licensed under the MIT License, subject to the Commons Clause
 * License Condition v1.0. You may use, copy, modify, and distribute this software,
 * but you may not sell the software itself, offer it as a paid service, or use it
 * in a product or service whose value derives substantially from the software
 * without prior written permission from the copyright holder.
 */

/*
 * framework.h — minimal unit-test framework for picoOS host tests.
 *
 * Usage:
 *   #include "framework.h"          (once per test binary)
 *
 *   static void test_foo(void) {
 *       BEGIN_TEST(foo_description);
 *       CHECK(condition, "failure message");
 *       END_TEST();
 *   }
 *
 *   int main(void) {
 *       test_foo();
 *       SUMMARY();   // prints results and returns 0 (all pass) or 1 (any fail)
 *   }
 *
 * SUMMARY() contains a return statement — it must be the last line of main().
 */

#pragma once
#include <stdio.h>
#include <stdbool.h>

static int        _tests_run    = 0;
static int        _tests_failed = 0;
static const char *_cur_test   = "(none)";
static bool       _test_ok     = true;

#define BEGIN_TEST(name)  do {          \
    _cur_test = #name;                  \
    _test_ok  = true;                   \
    _tests_run++;                       \
} while (0)

#define CHECK(cond, msg)  do {                                              \
    if (!(cond)) {                                                          \
        printf("    FAIL [%s:%d]: %s\n", _cur_test, __LINE__, (msg));      \
        _test_ok = false;                                                   \
    }                                                                       \
} while (0)

#define END_TEST()  do {                                    \
    if (_test_ok)                                           \
        printf("  PASS  %s\n", _cur_test);                 \
    else {                                                  \
        printf("  FAIL  %s\n", _cur_test);                 \
        _tests_failed++;                                    \
    }                                                       \
} while (0)

/* Must be the last statement in main(). */
#define SUMMARY()  do {                                                         \
    printf("\n%d / %d tests passed\n", (_tests_run - _tests_failed), _tests_run); \
    return (_tests_failed > 0) ? 1 : 0;                                        \
} while (0)

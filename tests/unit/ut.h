/*
 *  ut.h - minimal unit-test harness for tinycc internal tests
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * One TU in the binary must define UT_MAIN_IMPL to instantiate the
 * shared counters. Tests use UT_ASSERT / UT_ASSERT_EQ inside `UT_TEST`
 * functions, which are registered into suites via UT_RUN in a
 * `UT_SUITE`. The runner calls UT_RUN_SUITE for each suite.
 */

#ifndef TCC_UT_H
#define TCC_UT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int ut_fail_count;
extern int ut_run_count;
extern int ut_test_count;
extern int ut_test_fail_count;
extern const char *ut_current_test;

#define UT_ASSERT(cond)                                                        \
  do                                                                           \
  {                                                                            \
    ut_run_count++;                                                            \
    if (!(cond))                                                               \
    {                                                                          \
      fprintf(stderr, "    FAIL %s:%d: %s (in %s)\n",                          \
              __FILE__, __LINE__, #cond, ut_current_test);                     \
      ut_fail_count++;                                                         \
      return -1;                                                               \
    }                                                                          \
  } while (0)

#define UT_ASSERT_EQ(a, b)                                                     \
  do                                                                           \
  {                                                                            \
    ut_run_count++;                                                            \
    long long _ut_a = (long long)(a);                                          \
    long long _ut_b = (long long)(b);                                          \
    if (_ut_a != _ut_b)                                                        \
    {                                                                          \
      fprintf(stderr,                                                          \
              "    FAIL %s:%d: %s (%lld) != %s (%lld) (in %s)\n",              \
              __FILE__, __LINE__, #a, _ut_a, #b, _ut_b, ut_current_test);      \
      ut_fail_count++;                                                         \
      return -1;                                                               \
    }                                                                          \
  } while (0)

#define UT_TEST(name) static int name(void)

#define UT_RUN(name)                                                           \
  do                                                                           \
  {                                                                            \
    ut_current_test = #name;                                                   \
    ut_test_count++;                                                           \
    int _ut_before = ut_fail_count;                                            \
    int _ut_rc = name();                                                       \
    int _ut_failed = (_ut_rc != 0) || (ut_fail_count != _ut_before);           \
    if (_ut_failed)                                                            \
      ut_test_fail_count++;                                                    \
    fprintf(stderr, "    %s %s\n", _ut_failed ? "FAIL" : "ok  ", #name);       \
  } while (0)

#define UT_SUITE(name) void ut_suite_##name(void)
#define UT_DECLARE_SUITE(name) void ut_suite_##name(void)
#define UT_RUN_SUITE(name)                                                     \
  do                                                                           \
  {                                                                            \
    fprintf(stderr, "== suite %s ==\n", #name);                                \
    ut_suite_##name();                                                         \
  } while (0)

#define UT_MAIN_IMPL                                                           \
  int ut_fail_count = 0;                                                       \
  int ut_run_count = 0;                                                        \
  int ut_test_count = 0;                                                       \
  int ut_test_fail_count = 0;                                                  \
  const char *ut_current_test = "<none>"

#define UT_REPORT_AND_EXIT()                                                   \
  do                                                                           \
  {                                                                            \
    fprintf(stderr,                                                            \
            "\n%d tests, %d asserts, %d failed tests, %d failed asserts\n",    \
            ut_test_count, ut_run_count,                                       \
            ut_test_fail_count, ut_fail_count);                                \
    return ut_test_fail_count == 0 ? 0 : 1;                                    \
  } while (0)

#endif /* TCC_UT_H */

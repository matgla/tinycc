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
 * shared counters. Tests use UT_ASSERT / UT_ASSERT_EQ / UT_ASSERT_STREQ inside `UT_TEST`
 * functions, which are registered into suites via UT_RUN in a
 * `UT_SUITE`. The runner calls UT_RUN_SUITE for each suite.
 */

#ifndef TCC_UT_H
#define TCC_UT_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UT_MAX_FAILURES 256
#define UT_MAX_FAILURE_MSG 256

struct ut_failure {
    const char *test;
    const char *file;
    int line;
    char msg[UT_MAX_FAILURE_MSG];
};

extern int ut_fail_count;
extern int ut_run_count;
extern int ut_test_count;
extern int ut_test_fail_count;
extern const char *ut_current_test;
extern struct ut_failure ut_failures[];
extern int ut_failure_count;

void ut_record_failure(const char *file, int line, const char *fmt, ...);

#define UT_ASSERT(cond)                                                        \
  do                                                                           \
  {                                                                            \
    ut_run_count++;                                                            \
    if (!(cond))                                                               \
    {                                                                          \
      ut_record_failure(__FILE__, __LINE__, "%s", #cond);                      \
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
      ut_record_failure(__FILE__, __LINE__,                                    \
                        "%s (%lld) != %s (%lld)", #a, _ut_a, #b, _ut_b);      \
      fprintf(stderr,                                                          \
              "    FAIL %s:%d: %s (%lld) != %s (%lld) (in %s)\n",              \
              __FILE__, __LINE__, #a, _ut_a, #b, _ut_b, ut_current_test);      \
      ut_fail_count++;                                                         \
      return -1;                                                               \
    }                                                                          \
  } while (0)

#define UT_ASSERT_NE(a, b)                                                     \
  do                                                                           \
  {                                                                            \
    ut_run_count++;                                                            \
    long long _ut_a = (long long)(a);                                          \
    long long _ut_b = (long long)(b);                                          \
    if (_ut_a == _ut_b)                                                        \
    {                                                                          \
      ut_record_failure(__FILE__, __LINE__,                                    \
                        "%s (%lld) == %s (%lld)", #a, _ut_a, #b, _ut_b);      \
      fprintf(stderr,                                                          \
              "    FAIL %s:%d: %s (%lld) == %s (%lld) (in %s)\n",              \
              __FILE__, __LINE__, #a, _ut_a, #b, _ut_b, ut_current_test);      \
      ut_fail_count++;                                                         \
      return -1;                                                               \
    }                                                                          \
  } while (0)

#define UT_ASSERT_STREQ(a, b)                                                  \
  do                                                                           \
  {                                                                            \
    ut_run_count++;                                                            \
    const char *_ut_a = (a);                                                   \
    const char *_ut_b = (b);                                                   \
    if (_ut_a == NULL || _ut_b == NULL                                         \
            ? _ut_a != _ut_b                                                    \
            : strcmp(_ut_a, _ut_b) != 0)                                       \
    {                                                                          \
      ut_record_failure(__FILE__, __LINE__,                                    \
                        "%s (\"%s\") != %s (\"%s\")",                          \
                        #a, _ut_a ? _ut_a : "(null)",                          \
                        #b, _ut_b ? _ut_b : "(null)");                         \
      fprintf(stderr,                                                          \
              "    FAIL %s:%d: %s (\"%s\") != %s (\"%s\") (in %s)\n",          \
              __FILE__, __LINE__, #a, _ut_a ? _ut_a : "(null)",                \
              #b, _ut_b ? _ut_b : "(null)", ut_current_test);                  \
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
    {                                                                          \
      ut_test_fail_count++;                                                    \
      if (ut_fail_count == _ut_before)                                         \
        ut_record_failure(__FILE__, __LINE__, "test returned %d", _ut_rc);     \
    }                                                                          \
    fprintf(stderr, "    %s %s\n", _ut_failed ? "FAIL" : "ok  ", #name);       \
  } while (0)

/* Annotation: declares that the enclosing suite covers optimization pass
 * <pass_name> (a string literal, e.g. UT_COVERS("neg_chain_cse")). Consumed by
 * tests/unit/check_pass_coverage.py to build the pass-coverage ledger. Expands
 * to a no-op statement so it can sit inside a UT_SUITE body. */
#define UT_COVERS(pass_name) ((void)sizeof(pass_name))

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
  const char *ut_current_test = "<none>";                                      \
  struct ut_failure ut_failures[UT_MAX_FAILURES];                              \
  int ut_failure_count = 0;                                                    \
                                                                               \
  void ut_record_failure(const char *file, int line, const char *fmt, ...)     \
  {                                                                            \
    if (ut_failure_count >= UT_MAX_FAILURES)                                   \
      return;                                                                  \
    struct ut_failure *f = &ut_failures[ut_failure_count++];                   \
    f->test = ut_current_test;                                                 \
    f->file = file;                                                            \
    f->line = line;                                                            \
    va_list ap;                                                                \
    va_start(ap, fmt);                                                         \
    vsnprintf(f->msg, sizeof(f->msg), fmt, ap);                                \
    va_end(ap);                                                                \
  }

#define UT_REPORT_AND_EXIT()                                                   \
  do                                                                           \
  {                                                                            \
    if (ut_failure_count > 0)                                                  \
    {                                                                          \
      fprintf(stderr, "\nFailed tests/asserts:\n");                            \
      for (int _ut_i = 0; _ut_i < ut_failure_count; _ut_i++)                   \
      {                                                                        \
        fprintf(stderr, "  %s:%d: %s (in %s)\n",                               \
                ut_failures[_ut_i].file, ut_failures[_ut_i].line,              \
                ut_failures[_ut_i].msg, ut_failures[_ut_i].test);              \
      }                                                                        \
    }                                                                          \
    fprintf(stderr,                                                            \
            "\n%d tests, %d asserts, %d failed tests, %d failed asserts\n",    \
            ut_test_count, ut_run_count,                                       \
            ut_test_fail_count, ut_fail_count);                                \
    return ut_test_fail_count == 0 ? 0 : 1;                                    \
  } while (0)

#endif /* TCC_UT_H */

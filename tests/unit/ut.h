/*
 *  ut.h - self-registering unit-test harness for tinycc internal tests
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_UT_H
#define TCC_UT_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UT_MAX_FAILURES 256
#define UT_MAX_FAILURE_MSG 256
#define UT_MAX_TESTS 8192
#define UT_MAX_FIXTURES 64
#define UT_MAX_SUITE_NAME 128

struct ut_failure {
    const char *test;
    const char *file;
    int line;
    char msg[UT_MAX_FAILURE_MSG];
};

struct ut_test_entry {
    const char *file;
    const char *name;
    int (*fn)(void);
};

struct ut_fixture {
    const char *file;
    void (*setup)(void);
    void (*teardown)(void);
};

extern int ut_fail_count;
extern int ut_run_count;
extern int ut_test_count;
extern int ut_test_fail_count;
extern const char *ut_current_test;
extern struct ut_failure ut_failures[];
extern int ut_failure_count;
extern struct ut_test_entry ut_tests[];
extern int ut_test_registered;
extern struct ut_fixture ut_fixtures[];
extern int ut_fixture_registered;

void ut_record_failure(const char *file, int line, const char *fmt, ...);

static inline void ut_register_test(const char *file, const char *name,
                                    int (*fn)(void))
{
  if (ut_test_registered >= UT_MAX_TESTS) {
    fprintf(stderr, "ut.h: UT_MAX_TESTS exceeded registering %s\n", name);
    abort();
  }
  ut_tests[ut_test_registered].file = file;
  ut_tests[ut_test_registered].name = name;
  ut_tests[ut_test_registered].fn = fn;
  ut_test_registered++;
}

static inline struct ut_fixture *ut_fixture_for(const char *file)
{
  for (int i = 0; i < ut_fixture_registered; i++) {
    if (strcmp(ut_fixtures[i].file, file) == 0)
      return &ut_fixtures[i];
  }
  return NULL;
}

static inline struct ut_fixture *ut_fixture_slot(const char *file)
{
  struct ut_fixture *f = ut_fixture_for(file);
  if (f)
    return f;
  if (ut_fixture_registered >= UT_MAX_FIXTURES) {
    fprintf(stderr, "ut.h: UT_MAX_FIXTURES exceeded for %s\n", file);
    abort();
  }
  f = &ut_fixtures[ut_fixture_registered++];
  f->file = file;
  return f;
}

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

#define UT_TEST(name)                                                          \
  static int name(void);                                                       \
  __attribute__((constructor)) static void ut_register_##name(void)            \
  {                                                                            \
    ut_register_test(__FILE__, #name, name);                                   \
  }                                                                            \
  static int name(void)

#define UT_TEST_DISABLED(name) static int name(void)

#define UT_SUITE_SETUP(fn)                                                     \
  __attribute__((constructor)) static void ut_register_setup_##fn(void)        \
  {                                                                            \
    ut_fixture_slot(__FILE__)->setup = fn;                                     \
  }                                                                            \
  extern int ut_eat_semicolon_setup_##fn

#define UT_SUITE_TEARDOWN(fn)                                                  \
  __attribute__((constructor)) static void ut_register_teardown_##fn(void)     \
  {                                                                            \
    ut_fixture_slot(__FILE__)->teardown = fn;                                  \
  }                                                                            \
  extern int ut_eat_semicolon_teardown_##fn

/* Annotation: the enclosing file covers optimization pass <pass_name>
 * (a string literal). Consumed textually by pass-coverage tooling. */
#define UT_CAT2_(a, b) a##b
#define UT_CAT_(a, b) UT_CAT2_(a, b)
#define UT_COVERS(pass_name)                                                   \
  static const char UT_CAT_(ut_covers_, __COUNTER__)[]                         \
      __attribute__((unused)) = pass_name

static inline const char *ut_suite_of(const char *file, char *buf, size_t n)
{
  const char *base = strrchr(file, '/');
  base = base ? base + 1 : file;
  if (strncmp(base, "test_", 5) == 0)
    base += 5;
  snprintf(buf, n, "%s", base);
  char *dot = strrchr(buf, '.');
  if (dot)
    *dot = '\0';
  return buf;
}

static inline int ut_filter_match(const char *suite, const char *name,
                                  int argc, char **argv)
{
  if (argc <= 1)
    return 1;
  for (int i = 1; i < argc; i++) {
    if (strstr(suite, argv[i]) || strstr(name, argv[i]))
      return 1;
  }
  return 0;
}

static inline void ut_run_one(const struct ut_test_entry *t)
{
  ut_current_test = t->name;
  ut_test_count++;
  int _ut_before = ut_fail_count;
  int _ut_rc = t->fn();
  int _ut_failed = (_ut_rc != 0) || (ut_fail_count != _ut_before);
  if (_ut_failed)
  {
    ut_test_fail_count++;
    if (ut_fail_count == _ut_before)
      ut_record_failure(t->file, 0, "test returned %d", _ut_rc);
  }
  fprintf(stderr, "    %s %s\n", _ut_failed ? "FAIL" : "ok  ", t->name);
}

static inline int ut_report(void)
{
  if (ut_failure_count > 0)
  {
    fprintf(stderr, "\nFailed tests/asserts:\n");
    for (int i = 0; i < ut_failure_count; i++)
    {
      fprintf(stderr, "  %s:%d: %s (in %s)\n",
              ut_failures[i].file, ut_failures[i].line,
              ut_failures[i].msg, ut_failures[i].test);
    }
  }
  fprintf(stderr,
          "\n%d tests, %d asserts, %d failed tests, %d failed asserts\n",
          ut_test_count, ut_run_count,
          ut_test_fail_count, ut_fail_count);
  return ut_test_fail_count == 0 ? 0 : 1;
}

/* Runs registered tests grouped by suite (file order within a TU, link order
 * across TUs). argv[1..] are substring filters on suite or test name.
 * stop_after (may be NULL) names the last suite to run. */
static inline int ut_run_all_until(int argc, char **argv,
                                   const char *stop_after)
{
  char cur[UT_MAX_SUITE_NAME];
  struct ut_fixture *fx = NULL;
  cur[0] = '\0';
  for (int i = 0; i < ut_test_registered; i++) {
    char sn[UT_MAX_SUITE_NAME];
    ut_suite_of(ut_tests[i].file, sn, sizeof(sn));
    if (!ut_filter_match(sn, ut_tests[i].name, argc, argv))
      continue;
    if (strcmp(sn, cur) != 0) {
      if (fx && fx->teardown)
        fx->teardown();
      fx = NULL;
      if (cur[0] && stop_after && strcmp(cur, stop_after) == 0)
        return ut_report();
      snprintf(cur, sizeof(cur), "%s", sn);
      fprintf(stderr, "== suite %s ==\n", cur);
      fx = ut_fixture_for(ut_tests[i].file);
      if (fx && fx->setup)
        fx->setup();
    }
    ut_run_one(&ut_tests[i]);
  }
  if (fx && fx->teardown)
    fx->teardown();
  return ut_report();
}

static inline int ut_run_all(int argc, char **argv)
{
  return ut_run_all_until(argc, argv, NULL);
}

#define UT_MAIN_IMPL                                                           \
  int ut_fail_count = 0;                                                       \
  int ut_run_count = 0;                                                        \
  int ut_test_count = 0;                                                       \
  int ut_test_fail_count = 0;                                                  \
  const char *ut_current_test = "<none>";                                      \
  struct ut_failure ut_failures[UT_MAX_FAILURES];                              \
  int ut_failure_count = 0;                                                    \
  struct ut_test_entry ut_tests[UT_MAX_TESTS];                                 \
  int ut_test_registered = 0;                                                  \
  struct ut_fixture ut_fixtures[UT_MAX_FIXTURES];                              \
  int ut_fixture_registered = 0;                                               \
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

#endif /* TCC_UT_H */

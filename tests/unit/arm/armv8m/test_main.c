/*
 *  test_main.c - entry point for the main tinycc unit-test binary
 *
 *  Tests self-register via UT_TEST constructors; argv filters by
 *  suite/test-name substring (see tests/unit/README.md).
 */

#include "ut.h"

UT_MAIN_IMPL;

int main(int argc, char **argv)
{
#ifdef UT_COVERAGE_EARLY_EXIT
  extern void __gcov_dump(void);
  ut_run_all_until(argc, argv, "tccdbg");
  __gcov_dump();
  return 0;
#else
  return ut_run_all(argc, argv);
#endif
}

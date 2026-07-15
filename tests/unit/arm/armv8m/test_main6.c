/*
 *  test_main6.c - entry point for the tccelf/ unit-test binary (build_tccelf/run_unit_tests_tccelf)
 *
 *  Tests self-register via UT_TEST constructors; argv filters by
 *  suite/test-name substring (see tests/unit/README.md).
 */

#include "ut.h"

UT_MAIN_IMPL;

int main(int argc, char **argv)
{
  return ut_run_all(argc, argv);
}

/*
 *  test_main10.c - entry point for the tcc/ unit-test binary (build_tcc/run_unit_tests_tcc)
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

/*
 *  test_main9.c - entry point for the tccyaff/ unit-test binary (build_tccyaff/run_unit_tests_tccyaff)
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

/*
 *  test_main4.c - entry point for the libtcc-api/ unit-test binary (build_libtcc_api/run_unit_tests_libtcc_api)
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

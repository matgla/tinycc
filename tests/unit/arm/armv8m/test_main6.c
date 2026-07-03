/*
 *  test_main6.c - entry point for the tccelf/ unit-test binary
 *  (build_tccelf/run_unit_tests_tccelf)
 *
 *  Links the REAL tccelf.c against a minimal stub layer.  Add new suites
 *  with UT_DECLARE_SUITE + UT_RUN_SUITE as coverage expands.
 */

#include "ut.h"

UT_MAIN_IMPL;

UT_DECLARE_SUITE(tccelf);

int main(void)
{
  UT_RUN_SUITE(tccelf);
  UT_REPORT_AND_EXIT();
}

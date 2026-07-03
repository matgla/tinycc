/*
 *  test_main5.c - entry point for the tccopt/ unit-test binary
 *  (build_tccopt/run_unit_tests_tccopt)
 *
 *  Separate from test_main.c: this binary links the REAL tccopt.c (see the
 *  Makefile's UT5_* section) instead of leaving it coverage-only. Add new
 *  suites with UT_DECLARE_SUITE + UT_RUN_SUITE as more areas land.
 */

#include "ut.h"

UT_MAIN_IMPL;

UT_DECLARE_SUITE(tccopt);

int main(void)
{
  UT_RUN_SUITE(tccopt);
  UT_REPORT_AND_EXIT();
}

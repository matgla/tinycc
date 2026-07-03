/*
 *  test_main7.c - entry point for the tccpp/ unit-test binary
 *  (build_tccpp/run_unit_tests_tccpp)
 *
 *  Separate from test_main.c: this binary links the REAL tccpp.c directly
 *  (see the Makefile's UT7_* section).
 */

#include "ut.h"

UT_MAIN_IMPL;

UT_DECLARE_SUITE(tccpp);

int main(void)
{
  UT_RUN_SUITE(tccpp);
  UT_REPORT_AND_EXIT();
}

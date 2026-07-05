/*
 *  test_main8.c - entry point for the tcctools/ unit-test binary
 *  (build_tcctools/run_unit_tests_tcctools)
 *
 *  Separate from test_main.c: this binary links the REAL tcctools.c (see the
 *  Makefile's UT8_* section) instead of leaving it coverage-only.
 */

#include "ut.h"

UT_MAIN_IMPL;

UT_DECLARE_SUITE(tcctools);

int main(void)
{
    UT_RUN_SUITE(tcctools);
    UT_REPORT_AND_EXIT();
}

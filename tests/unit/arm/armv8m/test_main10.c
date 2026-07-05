/*
 *  test_main10.c - entry point for the tcc/ unit-test binary
 *  (build_tcc/run_unit_tests_tcc)
 *
 *  Separate from the other test_main*.c files: this binary pulls in tcc.c
 *  directly so the static/ST_FUNC helpers there can be exercised in isolation.
 */

#include "ut.h"

UT_MAIN_IMPL;

UT_DECLARE_SUITE(tcc);

int main(void)
{
  UT_RUN_SUITE(tcc);
  UT_REPORT_AND_EXIT();
}

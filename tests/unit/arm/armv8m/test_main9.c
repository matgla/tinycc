/*
 *  test_main9.c - entry point for the tccyaff/ unit-test binary
 *  (build_tccyaff/run_unit_tests_tccyaff)
 *
 *  Separate from the other test_main*.c files: this binary links the REAL
 *  tccyaff.c and tccelf.c from the tinycc source tree.
 */

#include "ut.h"

UT_MAIN_IMPL;

UT_DECLARE_SUITE(tccyaff);

int main(void)
{
  UT_RUN_SUITE(tccyaff);
  UT_REPORT_AND_EXIT();
}

/*
 *  test_main4.c - entry point for the libtcc-api/ unit-test binary
 *  (build_libtcc_api/run_unit_tests_libtcc_api)
 *
 *  Separate from test_main.c: this binary links the REAL libtcc.c (see the
 *  Makefile's UT4_* section) instead of leaving it coverage-only. Add new
 *  suites with UT_DECLARE_SUITE + UT_RUN_SUITE as more areas land.
 */

#include "ut.h"

UT_MAIN_IMPL;

UT_DECLARE_SUITE(libtcc_lifecycle);
UT_DECLARE_SUITE(libtcc_paths);
UT_DECLARE_SUITE(libtcc_symbols);
UT_DECLARE_SUITE(libtcc_options_opt);
UT_DECLARE_SUITE(libtcc_options_target);
UT_DECLARE_SUITE(libtcc_options_linker);
UT_DECLARE_SUITE(libtcc_output_files);

int main(void)
{
  UT_RUN_SUITE(libtcc_lifecycle);
  UT_RUN_SUITE(libtcc_paths);
  UT_RUN_SUITE(libtcc_symbols);
  UT_RUN_SUITE(libtcc_options_opt);
  UT_RUN_SUITE(libtcc_options_target);
  UT_RUN_SUITE(libtcc_options_linker);
  UT_RUN_SUITE(libtcc_output_files);
  UT_REPORT_AND_EXIT();
}

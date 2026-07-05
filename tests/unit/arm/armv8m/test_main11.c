/*
 *  test_main11.c - main entry point for the SSA optimizer unit test binary
 *
 *  Registers all SSA optimizer test suites and runs them.
 */

#include "ut.h"

UT_MAIN_IMPL;

UT_DECLARE_SUITE(ssa_opt_sccp);
UT_DECLARE_SUITE(ssa_opt_dead_loop);

int main(void)
{
  UT_RUN_SUITE(ssa_opt_dead_loop);
  UT_RUN_SUITE(ssa_opt_sccp);
  UT_REPORT_AND_EXIT();
}

/*
 *  test_main11.c - main entry point for the SSA optimizer unit test binary
 *
 *  Registers all SSA optimizer test suites and runs them.
 */

#include "ut.h"

UT_MAIN_IMPL;

UT_DECLARE_SUITE(ssa_opt_cprop);
UT_DECLARE_SUITE(ssa_opt_sccp);
UT_DECLARE_SUITE(ssa_opt_dead_loop);
UT_DECLARE_SUITE(ssa_opt_dce_global);
UT_DECLARE_SUITE(ssa_opt_gvn);
UT_DECLARE_SUITE(ssa_opt_const_string_fold);
UT_DECLARE_SUITE(ssa_opt_bitop_const_fold);
UT_DECLARE_SUITE(ssa_opt_strength);
UT_DECLARE_SUITE(ssa_opt_narrow);
UT_DECLARE_SUITE(ssa_opt_fold);
UT_DECLARE_SUITE(ssa_opt_phi);
UT_DECLARE_SUITE(ssa_opt_branch);
UT_DECLARE_SUITE(ssa_opt_cmp_eq);
UT_DECLARE_SUITE(opt_ssa_domwalk);
UT_DECLARE_SUITE(opt_dsl_pair);
UT_DECLARE_SUITE(opt_dsl_phi);

int main(void)
{
  UT_RUN_SUITE(ssa_opt_cprop);
  UT_RUN_SUITE(ssa_opt_dead_loop);
  UT_RUN_SUITE(ssa_opt_sccp);
  UT_RUN_SUITE(ssa_opt_dce_global);
  UT_RUN_SUITE(ssa_opt_gvn);
  UT_RUN_SUITE(ssa_opt_const_string_fold);
  UT_RUN_SUITE(ssa_opt_bitop_const_fold);
  UT_RUN_SUITE(ssa_opt_strength);
  UT_RUN_SUITE(ssa_opt_narrow);
  UT_RUN_SUITE(ssa_opt_fold);
  UT_RUN_SUITE(ssa_opt_phi);
  UT_RUN_SUITE(ssa_opt_branch);
  UT_RUN_SUITE(ssa_opt_cmp_eq);
  UT_RUN_SUITE(opt_ssa_domwalk);
  UT_RUN_SUITE(opt_dsl_pair);
  UT_RUN_SUITE(opt_dsl_phi);
  UT_REPORT_AND_EXIT();
}

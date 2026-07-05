/*
 *  test_main11.c - main entry point for the SSA optimizer unit test binary
 *
 *  Registers all SSA optimizer test suites and runs them.
 */

#include "ut.h"

UT_MAIN_IMPL;

UT_DECLARE_SUITE(ssa_build);
UT_DECLARE_SUITE(ssa_opt_usedef);
UT_DECLARE_SUITE(ssa_opt_phi);
UT_DECLARE_SUITE(ssa_opt_strength);
UT_DECLARE_SUITE(ssa_opt_narrow);
UT_DECLARE_SUITE(ssa_opt_reassoc);
UT_DECLARE_SUITE(ssa_opt_cmp_eq);
UT_DECLARE_SUITE(ssa_opt_fold);
UT_DECLARE_SUITE(ssa_opt_gvn);
UT_DECLARE_SUITE(ssa_opt_branch);
UT_DECLARE_SUITE(ssa_opt_dce);
UT_DECLARE_SUITE(ssa_opt_load_cse);
UT_DECLARE_SUITE(ssa_opt_cprop);
UT_DECLARE_SUITE(ssa_opt_sccp);
UT_DECLARE_SUITE(ssa_opt_dead_loop);
UT_DECLARE_SUITE(ssa_metamorphic);

int main(void)
{
  UT_RUN_SUITE(ssa_build);
  UT_RUN_SUITE(ssa_opt_usedef);
  UT_RUN_SUITE(ssa_opt_phi);
  UT_RUN_SUITE(ssa_opt_strength);
  UT_RUN_SUITE(ssa_opt_narrow);
  UT_RUN_SUITE(ssa_opt_reassoc);
  UT_RUN_SUITE(ssa_opt_cmp_eq);
  UT_RUN_SUITE(ssa_opt_fold);
  UT_RUN_SUITE(ssa_opt_gvn);
  UT_RUN_SUITE(ssa_opt_branch);
  UT_RUN_SUITE(ssa_opt_dce);
  UT_RUN_SUITE(ssa_opt_load_cse);
  UT_RUN_SUITE(ssa_opt_cprop);
  UT_RUN_SUITE(ssa_opt_sccp);
  UT_RUN_SUITE(ssa_opt_dead_loop);
  UT_RUN_SUITE(ssa_metamorphic);
  UT_REPORT_AND_EXIT();
}

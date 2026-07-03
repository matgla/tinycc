/*
 *  test_main2.c - entry point for the backend/ unit-test binary
 *  (build_backend/run_unit_tests_backend)
 *
 *  Separate from test_main.c/the main run_unit_tests binary: this binary
 *  links the REAL arm-thumb-gen.c + arm-thumb-callsite.c (see the Makefile's
 *  UT2_* section) instead of codegen_mop_stubs.c's fakes, to test the actual
 *  Thumb-2 encoding the backend emits. Add new suites with UT_DECLARE_SUITE
 *  + UT_RUN_SUITE as more phases land.
 */

#include "ut.h"

UT_MAIN_IMPL;

UT_DECLARE_SUITE(gen_dispatch_smoke);
UT_DECLARE_SUITE(gen_arith);
UT_DECLARE_SUITE(gen_mem);
UT_DECLARE_SUITE(gen_branch);
UT_DECLARE_SUITE(gen_switch);
UT_DECLARE_SUITE(gen_fp);
UT_DECLARE_SUITE(gen_atomic);
UT_DECLARE_SUITE(gen_call);
UT_DECLARE_SUITE(gen_callsite);
UT_DECLARE_SUITE(gen_prolog);
UT_DECLARE_SUITE(gen_setjmp);

int main(void)
{
  UT_RUN_SUITE(gen_dispatch_smoke);
  UT_RUN_SUITE(gen_arith);
  UT_RUN_SUITE(gen_mem);
  UT_RUN_SUITE(gen_branch);
  UT_RUN_SUITE(gen_switch);
  UT_RUN_SUITE(gen_fp);
  UT_RUN_SUITE(gen_atomic);
  UT_RUN_SUITE(gen_call);
  UT_RUN_SUITE(gen_callsite);
  UT_RUN_SUITE(gen_prolog);
  UT_RUN_SUITE(gen_setjmp);
  UT_REPORT_AND_EXIT();
}

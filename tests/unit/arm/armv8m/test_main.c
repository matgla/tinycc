/*
 *  test_main.c - entry point for tinycc unit-test binary
 *
 *  Each suite is declared here and invoked by main(). Add new suites
 *  with UT_DECLARE_SUITE + UT_RUN_SUITE as more phases land.
 */

#include "ut.h"

UT_MAIN_IMPL;

UT_DECLARE_SUITE(chained_hash);
UT_DECLARE_SUITE(ir_pool);
UT_DECLARE_SUITE(ir_type);
UT_DECLARE_SUITE(ir_vreg);
UT_DECLARE_SUITE(thop_adr);
UT_DECLARE_SUITE(thop_alu_reg);
UT_DECLARE_SUITE(thop_bitfield);
UT_DECLARE_SUITE(thop_block);
UT_DECLARE_SUITE(thop_constraints);
UT_DECLARE_SUITE(thop_branch);
UT_DECLARE_SUITE(thop_mrs);
UT_DECLARE_SUITE(thop_tbb);
UT_DECLARE_SUITE(thop_shift_reg);
UT_DECLARE_SUITE(thop_shift_imm);
UT_DECLARE_SUITE(thop_system);
UT_DECLARE_SUITE(thop_vfp);
UT_DECLARE_SUITE(thop_cmp);
UT_DECLARE_SUITE(thop_extend);
UT_DECLARE_SUITE(thop_ldrd);
UT_DECLARE_SUITE(thop_ldaex);
UT_DECLARE_SUITE(thop_ldrex);
UT_DECLARE_SUITE(thop_mem_exclusive);
UT_DECLARE_SUITE(thop_mem_imm);
UT_DECLARE_SUITE(thop_mem_reg);
UT_DECLARE_SUITE(thop_mem_unpriv);
UT_DECLARE_SUITE(thop_mov);
UT_DECLARE_SUITE(thop_ldr_literal);

int main(void)
{
  UT_RUN_SUITE(chained_hash);
  UT_RUN_SUITE(ir_pool);
  UT_RUN_SUITE(ir_type);
  UT_RUN_SUITE(ir_vreg);
  UT_RUN_SUITE(thop_adr);
  UT_RUN_SUITE(thop_alu_reg);
  UT_RUN_SUITE(thop_bitfield);
  UT_RUN_SUITE(thop_block);
  UT_RUN_SUITE(thop_constraints);
  UT_RUN_SUITE(thop_branch);
  UT_RUN_SUITE(thop_mrs);
  UT_RUN_SUITE(thop_tbb);
  UT_RUN_SUITE(thop_shift_reg);
  UT_RUN_SUITE(thop_shift_imm);
  UT_RUN_SUITE(thop_system);
  UT_RUN_SUITE(thop_vfp);
  UT_RUN_SUITE(thop_cmp);
  UT_RUN_SUITE(thop_extend);
  UT_RUN_SUITE(thop_ldrd);
  UT_RUN_SUITE(thop_ldaex);
  UT_RUN_SUITE(thop_ldrex);
  UT_RUN_SUITE(thop_mem_exclusive);
  UT_RUN_SUITE(thop_mem_imm);
  UT_RUN_SUITE(thop_mem_reg);
  UT_RUN_SUITE(thop_mem_unpriv);
  UT_RUN_SUITE(thop_mov);
  UT_RUN_SUITE(thop_ldr_literal);
  UT_REPORT_AND_EXIT();
}

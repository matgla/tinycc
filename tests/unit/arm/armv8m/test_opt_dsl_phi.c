/*
 *  test_opt_dsl_phi.c - DSL framework SSA phi primitives
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ssa_build.h"

#include "opt_dsl_phi.h"

#include "ut.h"

#define I32 IROP_BTYPE_INT32

OPT_GEN_PHI(test_phi_trivial)
{
  PATTERN_PHI(.kind = IR_PHI_PATTERN_TRIVIAL);
  REWRITE_PHI(.replacement = replacement_vreg);
}

static const OptDslPhiRule test_phi_rules[] = {
  OPT_PHI_ENTRY(test_phi_trivial),
};

static IRPhiNode make_phi(int32_t dest, IRPhiOperand *operands, int count)
{
  IRPhiNode phi = {0};
  phi.dest_vreg = dest;
  phi.operands = operands;
  phi.num_operands = count;
  return phi;
}

UT_TEST(test_phi_match_trivial_variants)
{
  int32_t replacement = -1;
  IROptPhiPatternSpec spec = { .kind = IR_PHI_PATTERN_TRIVIAL };
  IRPhiOperand operands[] = {
    { .vreg = -1 },
    { .vreg = 3 },
    { .vreg = 5 },
    { .vreg = 3 },
  };
  IRPhiNode phi = make_phi(5, operands, 4);

  UT_ASSERT_EQ(opt_dsl_phi_match(&phi, spec, &replacement), 1);
  UT_ASSERT_EQ(replacement, 3);

  phi.num_operands = 1;
  operands[0].vreg = 7;
  replacement = -1;
  UT_ASSERT_EQ(opt_dsl_phi_match(&phi, spec, &replacement), 1);
  UT_ASSERT_EQ(replacement, 7);
  return 0;
}

UT_TEST(test_phi_match_rejects_non_trivial)
{
  int32_t replacement = -1;
  IROptPhiPatternSpec spec = { .kind = IR_PHI_PATTERN_TRIVIAL };
  IRPhiOperand mixed[] = {
    { .vreg = 3 },
    { .vreg = 4 },
  };
  IRPhiOperand self[] = {
    { .vreg = 5 },
    { .vreg = -1 },
  };
  IRPhiNode phi = make_phi(5, mixed, 2);

  UT_ASSERT_EQ(opt_dsl_phi_match(&phi, spec, &replacement), 0);
  phi = make_phi(5, self, 2);
  UT_ASSERT_EQ(opt_dsl_phi_match(&phi, spec, &replacement), 0);

  spec.kind = (IROptPhiPattern)99;
  UT_ASSERT_EQ(opt_dsl_phi_match(&phi, spec, &replacement), 0);
  return 0;
}

UT_TEST(test_phi_dsl_rule_rewrite)
{
  IRPhiOperand operands[] = {
    { .vreg = 3 },
    { .vreg = 5 },
    { .vreg = 3 },
  };
  IRPhiNode phi = make_phi(5, operands, 3);
  IROptPhiRewriteSpec rewrite = { .replacement = -1 };

  UT_ASSERT_EQ(opt_dsl_dispatch_test_phi_trivial(NULL, &phi, &rewrite), 1);
  UT_ASSERT_EQ(rewrite.replacement, 3);

  operands[2].vreg = 4;
  UT_ASSERT_EQ(opt_dsl_dispatch_test_phi_trivial(NULL, &phi, &rewrite), 0);
  return 0;
}

UT_TEST(test_phi_count_tracks_nodes_and_operands)
{
  ssa_ctx c = ssa_ctx_new(2, 8);
  ssa_ctx_init_manual(&c);
  int32_t t3 = utb_vreg(utb_temp(3, I32));
  int32_t t5 = utb_vreg(utb_temp(5, I32));
  int32_t t6 = utb_vreg(utb_temp(6, I32));
  int operands = -1;

  ssa_add_phi(&c, 0, t5, (int32_t[]){ t3, t3 }, 2);
  ssa_add_phi(&c, 1, t6, (int32_t[]){ t3, t5, t3 }, 3);
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(opt_dsl_phi_count(c.ctx, &operands), 2);
  UT_ASSERT_EQ(operands, 5);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_phi_runner_reaches_fixed_point)
{
  ssa_ctx c = ssa_ctx_new(1, 8);
  ssa_ctx_init_manual(&c);
  int32_t t3 = utb_vreg(utb_temp(3, I32));
  int32_t t5 = utb_vreg(utb_temp(5, I32));
  int32_t t6 = utb_vreg(utb_temp(6, I32));

  ssa_add_phi(&c, 0, t5, (int32_t[]){ t3, t3 }, 2);
  ssa_add_phi(&c, 0, t6, (int32_t[]){ t5, t3 }, 2);
  int use_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(7, I32),
                            utb_temp(6, I32));
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ(ssa_vinfo(&c, t3)->use_count, 3);
  UT_ASSERT_EQ(ssa_vinfo(&c, t5)->def_phi_block, 0);
  UT_ASSERT_EQ(ssa_vinfo(&c, t6)->def_phi_block, 0);

  int changed = opt_dsl_run_phi_rules(
      c.ctx, test_phi_rules, OPT_DSL_TABLE_COUNT(test_phi_rules));
  UT_ASSERT_EQ(changed, 2);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 0);
  UT_ASSERT_EQ(irop_get_vreg(ssa_instr_src1(&c, use_i)), t3);
  UT_ASSERT_EQ(ssa_vinfo(&c, t3)->use_count, 1);
  UT_ASSERT_EQ(ssa_vinfo(&c, t5)->use_count, 0);
  UT_ASSERT_EQ(ssa_vinfo(&c, t5)->def_phi_block, -1);
  UT_ASSERT_EQ(ssa_vinfo(&c, t6)->def_phi_block, -1);

  int32_t t4 = utb_vreg(utb_temp(4, I32));
  UT_ASSERT_EQ(ssa_opt_replace_all_uses(c.ctx, t3, t4), 1);
  UT_ASSERT_EQ(irop_get_vreg(ssa_instr_src1(&c, use_i)), t4);
  UT_ASSERT_EQ(ssa_vinfo(&c, t3)->use_count, 0);
  UT_ASSERT_EQ(ssa_vinfo(&c, t4)->use_count, 1);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_phi_runner_keeps_protected_use)
{
  ssa_ctx c = ssa_ctx_new(1, 8);
  ssa_ctx_init_manual(&c);
  int32_t t3 = utb_vreg(utb_temp(3, I32));
  int32_t t5 = utb_vreg(utb_temp(5, I32));

  ssa_add_phi(&c, 0, t5, (int32_t[]){ t3, t3 }, 2);
  int use_i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(7, I32),
                             utb_temp(3, I32), utb_temp(5, I32));
  ssa_ctx_rebuild(&c);

  int oi = c.ir->compact_instructions[use_i].orig_index;
  uint8_t *barrel_shifts = tcc_mallocz((size_t)(oi + 1));
  barrel_shifts[oi] = 1;
  c.ir->barrel_shifts = barrel_shifts;
  c.ir->barrel_shifts_len = oi + 1;

  int t3_uses = ssa_vinfo(&c, t3)->use_count;
  int t5_uses = ssa_vinfo(&c, t5)->use_count;
  int t5_def_phi_block = ssa_vinfo(&c, t5)->def_phi_block;

  int changed = opt_dsl_run_phi_rules(
      c.ctx, test_phi_rules, OPT_DSL_TABLE_COUNT(test_phi_rules));
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(ssa_block_phi_count(&c, 0), 1);
  UT_ASSERT_EQ(irop_get_vreg(ssa_instr_src2(&c, use_i)), t5);
  UT_ASSERT_EQ(ssa_vinfo(&c, t3)->use_count, t3_uses);
  UT_ASSERT_EQ(ssa_vinfo(&c, t5)->use_count, t5_uses);
  UT_ASSERT_EQ(ssa_vinfo(&c, t5)->def_phi_block, t5_def_phi_block);

  c.ir->barrel_shifts = NULL;
  c.ir->barrel_shifts_len = 0;
  tcc_free(barrel_shifts);
  ssa_ctx_free(&c);
  return 0;
}

UT_SUITE(opt_dsl_phi)
{
  UT_RUN(test_phi_match_trivial_variants);
  UT_RUN(test_phi_match_rejects_non_trivial);
  UT_RUN(test_phi_dsl_rule_rewrite);
  UT_RUN(test_phi_count_tracks_nodes_and_operands);
  UT_RUN(test_phi_runner_reaches_fixed_point);
  UT_RUN(test_phi_runner_keeps_protected_use);
}

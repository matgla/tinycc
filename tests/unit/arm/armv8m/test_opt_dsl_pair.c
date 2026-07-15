/*
 *  test_opt_dsl_pair.c - DSL framework PAIR() / RETIRE_PAIR() primitives
 *
 *  Covers source/opt/framework/opt_dsl_ssa.h directly (narrow.c only exercises
 *  it transitively, and only for the SRC1 link):
 *    - opt_dsl_pair_match(): SRC1/SRC2 link selection, specific vs any (.op=-1)
 *      opcode, single_use gating, multi-def and non-vreg rejection.
 *    - opt_dsl_pair_retire(): use-list fixup with delete_second 0 vs 1.
 *    - PATTERN + PAIR(DEF_OF_SRC2) + GUARD + RETIRE_PAIR + REWRITE end-to-end
 *      through ssa_opt_run_gens.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ssa_build.h"

#include "opt_dsl.h"
#include "opt_dsl_ssa.h"
#include "opt/ssa/ssa_opt_helpers.h"

#include "ut.h"

#define I32 IROP_BTYPE_INT32

/* vinfo of the vreg an operand encodes. */
#define VI(c, op) ssa_opt_vinfo((c).ctx, irop_get_vreg(op))

/* ========================================================================
 * opt_dsl_pair_match: SRC1 link, specific opcode, out fields
 * ======================================================================== */

UT_TEST(test_pair_match_src1_op)
{
  ssa_ctx c = ssa_ctx_new(1, 5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  int shl_i = ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                             utb_imm(4, I32), UTB_NONE);
  int shr_i = ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(4, I32), UTB_NONE);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  OptDslPair p;
  IROptPairSpec spec = { .link = IR_PAIR_DEF_OF_SRC1, .op = TCCIR_OP_SHL };
  UT_ASSERT_EQ(opt_dsl_pair_match(c.ctx, shr_i, spec, &p), 1);
  UT_ASSERT_EQ(p.idx, shl_i);
  UT_ASSERT_EQ(p.op, TCCIR_OP_SHL);
  UT_ASSERT_EQ((int)p.vi->def_instr, shl_i);
  UT_ASSERT_EQ(irop_get_vreg(p.src1), irop_get_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ(p.src2.u.imm32, 4);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * opt_dsl_pair_match: SRC2 link resolves the def of src2, not src1
 * ======================================================================== */

UT_TEST(test_pair_match_src2_link)
{
  ssa_ctx c = ssa_ctx_new(1, 6);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  int shl_i = ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                             utb_imm(2, I32), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(7, I32));
  /* t4 = t3 + t1 : src1 = t3 (ASSIGN def), src2 = t1 (SHL def) */
  int add_i = ssa_add_instr4(&c, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(3, I32),
                             utb_temp(1, I32), UTB_NONE);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  OptDslPair p;
  IROptPairSpec s2 = { .link = IR_PAIR_DEF_OF_SRC2, .op = TCCIR_OP_SHL };
  UT_ASSERT_EQ(opt_dsl_pair_match(c.ctx, add_i, s2, &p), 1);
  UT_ASSERT_EQ(p.idx, shl_i);

  /* The SRC1 def is an ASSIGN, so a SHL-specific SRC1 link must NOT match. */
  IROptPairSpec s1 = { .link = IR_PAIR_DEF_OF_SRC1, .op = TCCIR_OP_SHL };
  UT_ASSERT_EQ(opt_dsl_pair_match(c.ctx, add_i, s1, &p), 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * opt_dsl_pair_match: .op = -1 matches any producer and reports its opcode
 * ======================================================================== */

UT_TEST(test_pair_match_any_op)
{
  ssa_ctx c = ssa_ctx_new(1, 5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  int add_i = ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32));
  int and_i = ssa_add_instr4(&c, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(0xF, I32), UTB_NONE);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  OptDslPair p;
  IROptPairSpec any = { .link = IR_PAIR_DEF_OF_SRC1, .op = -1 };
  UT_ASSERT_EQ(opt_dsl_pair_match(c.ctx, and_i, any, &p), 1);
  UT_ASSERT_EQ(p.idx, add_i);
  UT_ASSERT_EQ(p.op, TCCIR_OP_ADD);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * opt_dsl_pair_match: specific opcode mismatch → no match
 * ======================================================================== */

UT_TEST(test_pair_match_op_mismatch)
{
  ssa_ctx c = ssa_ctx_new(1, 5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(4, I32), UTB_NONE);
  int shr_i = ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(4, I32), UTB_NONE);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  OptDslPair p;
  IROptPairSpec want_shr = { .link = IR_PAIR_DEF_OF_SRC1, .op = TCCIR_OP_SHR };
  UT_ASSERT_EQ(opt_dsl_pair_match(c.ctx, shr_i, want_shr, &p), 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * opt_dsl_pair_match: .single_use gates on the producer's use count
 * ======================================================================== */

UT_TEST(test_pair_match_single_use_gate)
{
  ssa_ctx c = ssa_ctx_new(1, 6);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(4, I32), UTB_NONE);
  /* Two consumers of t1 → use_count == 2. */
  int shr_i = ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(4, I32), UTB_NONE);
  ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(3, I32), utb_temp(1, I32),
                 utb_imm(2, I32), UTB_NONE);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  UT_ASSERT_EQ((int)VI(c, utb_temp(1, I32))->use_count, 2);

  OptDslPair p;
  IROptPairSpec su = { .link = IR_PAIR_DEF_OF_SRC1, .op = TCCIR_OP_SHL, .single_use = 1 };
  UT_ASSERT_EQ(opt_dsl_pair_match(c.ctx, shr_i, su, &p), 0);

  IROptPairSpec no_su = { .link = IR_PAIR_DEF_OF_SRC1, .op = TCCIR_OP_SHL };
  UT_ASSERT_EQ(opt_dsl_pair_match(c.ctx, shr_i, no_su, &p), 1);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * opt_dsl_pair_match: multi-def producer → no match
 * ======================================================================== */

UT_TEST(test_pair_match_multi_def)
{
  ssa_ctx c = ssa_ctx_new(1, 5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(4, I32), UTB_NONE);
  ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                 utb_imm(4, I32), UTB_NONE);
  int shr_i = ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(4, I32), UTB_NONE);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  OptDslPair p;
  IROptPairSpec spec = { .link = IR_PAIR_DEF_OF_SRC1, .op = TCCIR_OP_SHL };
  UT_ASSERT_EQ(opt_dsl_pair_match(c.ctx, shr_i, spec, &p), 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * opt_dsl_pair_match: linked operand is not a vreg → no match
 * ======================================================================== */

UT_TEST(test_pair_match_non_vreg)
{
  ssa_ctx c = ssa_ctx_new(1, 4);
  /* src1 is an immediate, not a vreg. */
  int shr_i = ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(2, I32), utb_imm(5, I32),
                             utb_imm(4, I32), UTB_NONE);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  OptDslPair p;
  IROptPairSpec any = { .link = IR_PAIR_DEF_OF_SRC1, .op = -1 };
  UT_ASSERT_EQ(opt_dsl_pair_match(c.ctx, shr_i, any, &p), 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * opt_dsl_pair_retire: delete_second = 0 moves the use edge, keeps producer
 * ======================================================================== */

UT_TEST(test_pair_retire_keep)
{
  ssa_ctx c = ssa_ctx_new(1, 5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  int shl_i = ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                             utb_imm(4, I32), UTB_NONE);
  int shr_i = ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(4, I32), UTB_NONE);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  OptDslPair p;
  IROptPairSpec spec = { .link = IR_PAIR_DEF_OF_SRC1, .op = TCCIR_OP_SHL };
  UT_ASSERT_EQ(opt_dsl_pair_match(c.ctx, shr_i, spec, &p), 1);
  UT_ASSERT_EQ((int)VI(c, utb_temp(1, I32))->use_count, 1);
  UT_ASSERT_EQ((int)VI(c, utb_temp(0, I32))->use_count, 1);

  /* Forward instr's src1 from t1 to t0 without deleting the SHL. */
  opt_dsl_pair_retire(c.ctx, shr_i, &p, utb_temp(0, I32), 0);

  UT_ASSERT_EQ((int)VI(c, utb_temp(1, I32))->use_count, 0);
  UT_ASSERT_EQ((int)VI(c, utb_temp(0, I32))->use_count, 2);
  UT_ASSERT_EQ(utb_op(c.ir, shl_i), TCCIR_OP_SHL);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * opt_dsl_pair_retire: delete_second = 1 kills the producer
 * ======================================================================== */

UT_TEST(test_pair_retire_delete)
{
  ssa_ctx c = ssa_ctx_new(1, 5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  int shl_i = ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                             utb_imm(4, I32), UTB_NONE);
  int shr_i = ssa_add_instr4(&c, TCCIR_OP_SHR, utb_temp(2, I32), utb_temp(1, I32),
                             utb_imm(4, I32), UTB_NONE);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  OptDslPair p;
  IROptPairSpec spec = { .link = IR_PAIR_DEF_OF_SRC1, .op = TCCIR_OP_SHL };
  UT_ASSERT_EQ(opt_dsl_pair_match(c.ctx, shr_i, spec, &p), 1);

  opt_dsl_pair_retire(c.ctx, shr_i, &p, utb_temp(0, I32), 1);

  UT_ASSERT_EQ(utb_op(c.ir, shl_i), TCCIR_OP_NOP);
  UT_ASSERT_EQ((int)VI(c, utb_temp(1, I32))->use_count, 0);
  /* Deleting the SHL releases its own use of t0, and the consumer picks t0 up:
   * the forwarded value keeps exactly one user (count conserved at 1). */
  UT_ASSERT_EQ((int)VI(c, utb_temp(0, I32))->use_count, 1);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * End-to-end DSL gen: ADD whose src2 is defined by a single-use SHL #k, folded
 * to SUB with src2 forwarded to the SHL's input and the SHL retired.  Exercises
 * PATTERN + PAIR(DEF_OF_SRC2) + GUARD + RETIRE_PAIR + REWRITE.
 * ======================================================================== */

OPT_GEN_SSA(t_pair_src2, TCCIR_OP_ADD) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  PAIR(.link = IR_PAIR_DEF_OF_SRC2, .op = TCCIR_OP_SHL, .single_use = 1);
  GUARD(when(is_imm32(psrc2)));
  RETIRE_PAIR(psrc1, 1);
  REWRITE(.new_op = TCCIR_OP_SUB, .src2 = psrc1);
}

static const IRSSAOptGen t_pair_gens[] = {
  OPT_GEN_ENTRY(t_pair_src2, TCCIR_OP_ADD),
};

UT_TEST(test_pair_dsl_end_to_end)
{
  ssa_ctx c = ssa_ctx_new(1, 6);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  int shl_i = ssa_add_instr4(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32),
                             utb_imm(2, I32), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(7, I32));
  int add_i = ssa_add_instr4(&c, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(3, I32),
                             utb_temp(1, I32), UTB_NONE);
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_run_gens(c.ctx, t_pair_gens, OPT_DSL_TABLE_COUNT(t_pair_gens));
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, add_i), TCCIR_OP_SUB);
  UT_ASSERT_EQ(irop_get_vreg(utb_src2(c.ir, add_i)), irop_get_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_op(c.ir, shl_i), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_COVERS("opt_dsl:pair");

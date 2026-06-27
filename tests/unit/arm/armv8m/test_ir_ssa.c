/*
 *  test_ir_ssa.c - suite for ir/ssa.c SSA construction and renaming
 *
 *  Exercises phi placement, promotability decisions, and variable renaming.
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"

#include "ut.h"

static SValue sv_var(int vreg)
{
  SValue sv;
  svalue_init(&sv);
  sv.vr = vreg;
  sv.type.t = VT_INT;
  return sv;
}

static SValue sv_const(int v)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = v;
  sv.type.t = VT_INT;
  return sv;
}

static SValue sv_jump_target(int target_idx)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = target_idx;
  sv.type.t = VT_INT;
  return sv;
}

/* -------------------------------------------------------------------------- */
/* Null / trivial cases                                                       */
/* -------------------------------------------------------------------------- */

UT_TEST(test_ssa_construct_null)
{
  UT_ASSERT(tcc_ir_ssa_construct(NULL, NULL) == NULL);

  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT(tcc_ir_ssa_construct(ir, NULL) == NULL);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_ssa_construct_no_vars)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  SValue s_t0 = sv_var(t0);
  SValue s_one = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_t0);

  IRCFG *cfg = tcc_ir_cfg_build(ir);
  UT_ASSERT(cfg != NULL);
  tcc_ir_cfg_compute_dominators(cfg);
  tcc_ir_cfg_compute_dom_frontiers(cfg);

  UT_ASSERT(tcc_ir_ssa_construct(ir, cfg) == NULL);

  tcc_ir_cfg_free(cfg);
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_ssa_construct_unsupported_ops)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  SValue s_v0 = sv_var(v0);
  SValue s_one = sv_const(1);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_v0);
  tcc_ir_put(ir, TCCIR_OP_SETJMP, &s_v0, &s_v0, &s_v0);

  IRCFG *cfg = tcc_ir_cfg_build(ir);
  tcc_ir_cfg_compute_dominators(cfg);
  tcc_ir_cfg_compute_dom_frontiers(cfg);

  UT_ASSERT(tcc_ir_ssa_construct(ir, cfg) == NULL);

  tcc_ir_cfg_free(cfg);
  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Single block: no phis                                                      */
/* -------------------------------------------------------------------------- */

UT_TEST(test_ssa_single_block_no_phis)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  SValue s_v0 = sv_var(v0);
  SValue s_ten = sv_const(10);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_ten, NULL, &s_v0);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v0, NULL, NULL);

  IRCFG *cfg = tcc_ir_cfg_build(ir);
  UT_ASSERT(cfg != NULL);
  tcc_ir_cfg_compute_dominators(cfg);
  tcc_ir_cfg_compute_dom_frontiers(cfg);

  IRSSAState *ssa = tcc_ir_ssa_construct(ir, cfg);
  UT_ASSERT(ssa != NULL);

  for (int b = 0; b < cfg->num_blocks; b++)
    UT_ASSERT(ssa->block_phis[b] == NULL);

  tcc_ir_ssa_free(ssa);
  tcc_ir_cfg_free(cfg);
  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Diamond CFG: phi at merge                                                  */
/* -------------------------------------------------------------------------- */

UT_TEST(test_ssa_diamond_inserts_phi)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  SValue s_v0 = sv_var(v0);
  SValue s_zero = sv_const(0);
  SValue s_one = sv_const(1);
  SValue s_two = sv_const(2);
  SValue j3 = sv_jump_target(3);
  SValue j4 = sv_jump_target(4);

  /* 0: block 0 - conditional jump to block 2 (instr 3) */
  tcc_ir_put(ir, TCCIR_OP_JUMPIF, &s_zero, NULL, &j3);

  /* 1-2: block 1 - then branch defines v0 = 1, jumps to merge */
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_v0);
  tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &j4);

  /* 3: block 2 - else branch defines v0 = 2 */
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_two, NULL, &s_v0);

  /* 4: block 3 - merge, use v0 */
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v0, NULL, NULL);

  IRCFG *cfg = tcc_ir_cfg_build(ir);
  UT_ASSERT(cfg != NULL);
  UT_ASSERT(cfg->num_blocks >= 3);
  tcc_ir_cfg_compute_dominators(cfg);
  tcc_ir_cfg_compute_dom_frontiers(cfg);

  IRSSAState *ssa = tcc_ir_ssa_construct(ir, cfg);
  UT_ASSERT(ssa != NULL);

  /* Find the merge block (the one containing the RETURNVALUE). */
  int merge_block = cfg->instr_to_block[4];
  IRPhiNode *phi = ssa->block_phis[merge_block];
  UT_ASSERT(phi != NULL);
  UT_ASSERT_EQ(phi->num_operands, 2);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(phi->orig_vreg), TCCIR_VREG_TYPE_VAR);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_POSITION(phi->orig_vreg), 0);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(phi->dest_vreg), TCCIR_VREG_TYPE_TEMP);

  /* Pred blocks should be the two branch arms (then: instr 1-2, else: instr 3). */
  int then_block = cfg->instr_to_block[2];
  int else_block = cfg->instr_to_block[3];
  int seen_then = 0, seen_else = 0;
  for (int i = 0; i < phi->num_operands; i++)
  {
    int pb = phi->operands[i].pred_block;
    UT_ASSERT(pb >= 0 && pb < cfg->num_blocks);
    if (pb == then_block)
      seen_then = 1;
    if (pb == else_block)
      seen_else = 1;
  }
  UT_ASSERT(seen_then && seen_else);

  tcc_ir_ssa_free(ssa);
  tcc_ir_cfg_free(cfg);
  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Rename pass rewrites uses                                                  */
/* -------------------------------------------------------------------------- */

UT_TEST(test_ssa_rename_rewrites_uses)
{
  TCCIRState *ir = tcc_ir_alloc();
  int v0 = tcc_ir_vreg_alloc_var(ir);
  SValue s_v0 = sv_var(v0);
  SValue s_zero = sv_const(0);
  SValue s_one = sv_const(1);
  SValue s_two = sv_const(2);
  SValue j3 = sv_jump_target(3);
  SValue j4 = sv_jump_target(4);

  tcc_ir_put(ir, TCCIR_OP_JUMPIF, &s_zero, NULL, &j3);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_v0);
  tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &j4);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_two, NULL, &s_v0);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v0, NULL, NULL);

  IRCFG *cfg = tcc_ir_cfg_build(ir);
  tcc_ir_cfg_compute_dominators(cfg);
  tcc_ir_cfg_compute_dom_frontiers(cfg);

  IRSSAState *ssa = tcc_ir_ssa_construct(ir, cfg);
  UT_ASSERT(ssa != NULL);

  tcc_ir_ssa_rename(ir, ssa);

  /* The use of v0 in the RETURNVALUE should now be a TEMP. */
  int use_idx = 4;
  IRQuadCompact *q = &ir->compact_instructions[use_idx];
  IROperand src = tcc_ir_op_get_src1(ir, q);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(irop_get_vreg(src)), TCCIR_VREG_TYPE_TEMP);

  tcc_ir_ssa_free(ssa);
  tcc_ir_cfg_free(cfg);
  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(ir_ssa)
{
  UT_RUN(test_ssa_construct_null);
  UT_RUN(test_ssa_construct_no_vars);
  UT_RUN(test_ssa_construct_unsupported_ops);
  UT_RUN(test_ssa_single_block_no_phis);
  UT_RUN(test_ssa_diamond_inserts_phi);
  UT_RUN(test_ssa_rename_rewrites_uses);
}

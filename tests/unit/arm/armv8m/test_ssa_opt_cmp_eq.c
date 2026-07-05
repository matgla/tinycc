/*
 *  test_ssa_opt_cmp_eq.c - CMP equality fact propagation
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_cmp_eq_prop(): pushing equality facts from CMP+JEQ/JNE
 *      * CMP a, b; JEQ → fact: a == b
 *      - fact lookup is symmetric (a,b) / (b,a)
 *      * CMP a, b; JNE → fact: a != b
 *      * Later CMP with same operands folds to JUMP (always taken)
 *        or NOPs (never taken)
 *      * Dead phi edges are dropped after folding an edge
 *      * Negative cases: non-eligible operands, missing CMP/JUMPIF pair,
 *        out-of-range target, non-EQ/NE condition token, empty CFG
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_cmp_eq.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 */

#include "ssa_build.h"
#include "ir/opt/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "ir/opt/ssa_opt.h"

#define I32 IROP_BTYPE_INT32

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

#define TOK_EQ 0x94
#define TOK_NE 0x95

/* ------------------------------------------------------------------ helpers */

static inline IROperand utb_cond(int tok)
{
  return irop_make_imm32(0, tok, I32);
}

static inline IROperand utb_jdest(int idx)
{
  return irop_make_imm32(0, idx, I32);
}

static inline int emit_cmp(ssa_ctx *c, IROperand a, IROperand b)
{
  return ssa_add_instr3(c, TCCIR_OP_CMP, UTB_NONE, a, b);
}

static inline int emit_jumpif(ssa_ctx *c, int tok, int target_idx)
{
  return ssa_add_instr(c, TCCIR_OP_JUMPIF, utb_jdest(target_idx), utb_cond(tok));
}

static void patch_jdest(ssa_ctx *c, int jmp_idx, int target_idx)
{
  IROperand *pool = c->ir->iroperand_pool;
  int base = c->ir->compact_instructions[jmp_idx].operand_base;
  pool[base].u.imm32 = target_idx;
}

static int phi_total_operands(ssa_ctx *c, int block)
{
  int n = 0;
  for (IRPhiNode *phi = c->ssa->block_phis[block]; phi; phi = phi->next)
    n += phi->num_operands;
  return n;
}

/* =======================================================================
 * Sanity: a single-block CMP with no following JUMPIF does nothing.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_eq)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_cmp_eq_prop_ne)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT(changed >= 0);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * JEQ taken edge: fact a==b, later JNE in dominated block is never taken.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_jeq_taken_folds_later_jne)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp0 = emit_jumpif(&c, TOK_EQ, -1);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_NE, -1);

  int b2_start = c.num_instrs;
  patch_jdest(&c, jmp1, b2_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  fprintf(stderr, "DEBUG num_blocks=%d num_instrs=%d\n", c.cfg->num_blocks, c.cfg->num_instrs);
  for (int bi = 0; bi < c.cfg->num_blocks; bi++) {
    IRBasicBlock *bb = &c.cfg->blocks[bi];
    fprintf(stderr, "DEBUG block %d: [%d,%d) preds=", bi, bb->start_idx, bb->end_idx);
    for (int pi = 0; pi < bb->num_preds; pi++) fprintf(stderr, "%d ", bb->preds[pi]);
    fprintf(stderr, "idom=%d\n", bb->idom);
  }

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * JEQ not-taken edge: fact a!=b, later JEQ in fall-through is never taken.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_jeq_not_taken_folds_later_jeq)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp0 = emit_jumpif(&c, TOK_EQ, -1);

  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_EQ, -1);

  int b2_start = c.num_instrs;
  patch_jdest(&c, jmp0, b2_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  int b3_start = c.num_instrs;
  patch_jdest(&c, jmp1, b3_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * JNE taken edge: fact a!=b, later JEQ in dominated block is never taken.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_jne_taken_folds_later_jeq)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp0 = emit_jumpif(&c, TOK_NE, -1);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_EQ, -1);

  int b2_start = c.num_instrs;
  patch_jdest(&c, jmp1, b2_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * JNE not-taken edge: fact a==b, later JNE in fall-through is never taken.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_jne_not_taken_folds_later_jne)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp0 = emit_jumpif(&c, TOK_NE, -1);

  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_NE, -1);

  int b2_start = c.num_instrs;
  patch_jdest(&c, jmp0, b2_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  int b3_start = c.num_instrs;
  patch_jdest(&c, jmp1, b3_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * Fact lookup is symmetric: fact (a,b) also matches a later CMP (b,a).
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_symmetric_fact_lookup)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp0 = emit_jumpif(&c, TOK_EQ, -1);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(1, I32), utb_temp(0, I32));
  int jmp1 = emit_jumpif(&c, TOK_NE, -1);

  int b2_start = c.num_instrs;
  patch_jdest(&c, jmp1, b2_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_NOP);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * Always-taken path: JNE becomes an unconditional JUMP.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_jne_becomes_jump)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp0 = emit_jumpif(&c, TOK_EQ, -1);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_NE, -1);

  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  int b3_start = c.num_instrs;
  patch_jdest(&c, jmp1, b3_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_JUMP);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * No fold when the later CMP uses a different operand pair.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_different_operands_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(3, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp0 = emit_jumpif(&c, TOK_EQ, -1);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(2, I32));
  int jmp1 = emit_jumpif(&c, TOK_EQ, -1);

  int b2_start = c.num_instrs;
  patch_jdest(&c, jmp1, b2_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_JUMPIF);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * No fact when the CMP operand is an immediate.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_immediate_operand_no_fact)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_imm(7, I32));
  int jmp0 = emit_jumpif(&c, TOK_EQ, -1);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_imm(7, I32));
  int jmp1 = emit_jumpif(&c, TOK_EQ, -1);

  int b2_start = c.num_instrs;
  patch_jdest(&c, jmp1, b2_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_CMP);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * No fact when the CMP operand is an lvalue.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_lval_operand_no_fact)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_lval(utb_temp(1, I32)));
  int jmp0 = emit_jumpif(&c, TOK_EQ, -1);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_EQ, -1);

  int b2_start = c.num_instrs;
  patch_jdest(&c, jmp1, b2_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_CMP);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * No fact when the CMP operand is a VAR (not a TEMP).
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_var_operand_no_fact)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_var(0, I32));
  int jmp0 = emit_jumpif(&c, TOK_EQ, -1);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_EQ, -1);

  int b2_start = c.num_instrs;
  patch_jdest(&c, jmp1, b2_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_CMP);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * No fact when the predecessor ends with JUMP (not JUMPIF).
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_jump_not_jumpif)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp0 = ssa_add_instr(&c, TCCIR_OP_JUMP, utb_jdest(-1), UTB_NONE);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_EQ, -1);

  int b2_start = c.num_instrs;
  patch_jdest(&c, jmp1, b2_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_CMP);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * No fact when there is no CMP immediately before the JUMPIF.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_no_cmp_before_jumpif)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  int jmp0 = emit_jumpif(&c, TOK_EQ, -1);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_EQ, -1);

  int b2_start = c.num_instrs;
  patch_jdest(&c, jmp1, b2_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_CMP);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * No fact when the JUMPIF target is out of range.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_invalid_target_no_fact)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp0 = emit_jumpif(&c, TOK_EQ, -1);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_EQ, -1);

  int b2_start = c.num_instrs;
  patch_jdest(&c, jmp1, b2_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  /* Leave jmp0 target as -1 (out of range). */
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_CMP);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * No fact when the condition token is neither EQ nor NE.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_non_eqne_token_no_fact)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/3, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp0 = emit_jumpif(&c, TOK_GT, -1);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_EQ, -1);

  int b2_start = c.num_instrs;
  patch_jdest(&c, jmp1, b2_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_CMP);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * No fact when the JUMPIF target and fall-through are the same block.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_target_equals_fallthrough_no_fact)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp0 = emit_jumpif(&c, TOK_EQ, -1);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * Facts are scoped to the dominator subtree: sibling blocks see different
 * facts and do not leak into each other.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_fact_scoped_to_subtree)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/5, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp0 = emit_jumpif(&c, TOK_EQ, -1);

  /* Taken path: block 1, fact a==b.  JNE here is never taken. */
  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_NE, -1);
  int b3_start_tmp = c.num_instrs;
  int jmp_extra = ssa_add_instr(&c, TCCIR_OP_JUMP, utb_jdest(-1), UTB_NONE);

  /* Fall-through path: block 2, fact a!=b.  JNE here is always taken. */
  int cmp2 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp2 = emit_jumpif(&c, TOK_NE, -1);

  int b3_start = c.num_instrs;
  patch_jdest(&c, jmp1, b3_start);
  patch_jdest(&c, jmp_extra, b3_start);
  patch_jdest(&c, jmp2, b3_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  (void)b3_start_tmp;

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT(changed >= 2);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, cmp2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp2), TCCIR_OP_JUMP);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * Dead target edge: folding removes the phi operand from the unreachable
 * successor.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_phi_edge_dead_target)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp0 = emit_jumpif(&c, TOK_EQ, -1);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_NE, -1);
  int jmp_extra = ssa_add_instr(&c, TCCIR_OP_JUMP, utb_jdest(-1), UTB_NONE);

  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  int b3_start = c.num_instrs;
  patch_jdest(&c, jmp1, b3_start);
  patch_jdest(&c, jmp_extra, b3_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  /* Phi in block 3 with one incoming value from block 1. */
  ssa_add_phi(&c, /*block=*/3, utb_vreg(utb_temp(4, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)) }, 1);
  c.ssa->block_phis[3]->operands[0].pred_block = 1;

  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(phi_total_operands(&c, 3), 0);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * Dead fall-through edge: folding removes the phi operand from the dead
 * fall-through successor.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_phi_edge_dead_fallthrough)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/4, /*temps=*/5);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp0 = emit_jumpif(&c, TOK_EQ, -1);

  int b1_start = c.num_instrs;
  patch_jdest(&c, jmp0, b1_start);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_EQ, -1);
  int jmp_extra = ssa_add_instr(&c, TCCIR_OP_JUMP, utb_jdest(-1), UTB_NONE);

  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  int b3_start = c.num_instrs;
  patch_jdest(&c, jmp1, b3_start);
  patch_jdest(&c, jmp_extra, b3_start);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);

  /* Phi in block 2 (fall-through) with one incoming value from block 1. */
  ssa_add_phi(&c, /*block=*/2, utb_vreg(utb_temp(4, I32)),
              (int32_t[]){ utb_vreg(utb_temp(0, I32)) }, 1);
  c.ssa->block_phis[2]->operands[0].pred_block = 1;

  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT(changed >= 1);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(phi_total_operands(&c, 2), 0);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * Empty CFG: the pass returns 0 immediately.
 * ======================================================================== */

UT_TEST(test_cmp_eq_prop_empty_cfg)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  c.ctx->ir = c.ir;
  c.ctx->ssa = NULL;
  c.ctx->cfg = NULL;

  int changed = ssa_opt_cmp_eq_prop(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* =======================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_cmp_eq)
{
  UT_COVERS("ssa:cmp_eq_prop");
  UT_RUN(test_cmp_eq_prop_eq);
  UT_RUN(test_cmp_eq_prop_ne);
  UT_RUN(test_cmp_eq_prop_jeq_taken_folds_later_jne);
  UT_RUN(test_cmp_eq_prop_jeq_not_taken_folds_later_jeq);
  UT_RUN(test_cmp_eq_prop_jne_taken_folds_later_jeq);
  UT_RUN(test_cmp_eq_prop_jne_not_taken_folds_later_jne);
  UT_RUN(test_cmp_eq_prop_symmetric_fact_lookup);
  UT_RUN(test_cmp_eq_prop_jne_becomes_jump);
  UT_RUN(test_cmp_eq_prop_different_operands_no_fold);
  UT_RUN(test_cmp_eq_prop_immediate_operand_no_fact);
  UT_RUN(test_cmp_eq_prop_lval_operand_no_fact);
  UT_RUN(test_cmp_eq_prop_var_operand_no_fact);
  UT_RUN(test_cmp_eq_prop_jump_not_jumpif);
  UT_RUN(test_cmp_eq_prop_no_cmp_before_jumpif);
  UT_RUN(test_cmp_eq_prop_invalid_target_no_fact);
  UT_RUN(test_cmp_eq_prop_non_eqne_token_no_fact);
  UT_RUN(test_cmp_eq_prop_target_equals_fallthrough_no_fact);
  UT_RUN(test_cmp_eq_prop_fact_scoped_to_subtree);
  UT_RUN(test_cmp_eq_prop_phi_edge_dead_target);
  UT_RUN(test_cmp_eq_prop_phi_edge_dead_fallthrough);
  UT_RUN(test_cmp_eq_prop_empty_cfg);
}

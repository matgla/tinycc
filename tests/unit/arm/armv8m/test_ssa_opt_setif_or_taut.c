/*
 *  test_ssa_opt_setif_or_taut.c - SSA SETIF OR-chain tautology fold
 *
 *  Covers ssa_opt_setif_or_taut() (source/opt/ssa/scalar/setif_or_taut.c):
 *  an OR of two CMP+SETIF booleans over the same operands whose {LT,EQ,GT}
 *  masks union to 0b111 folds to constant 1, including OR-chains resolved
 *  through the def-use graph.  Mirrors flat test_opt_setif_or_taut.c.
 *
 *  HARNESS NOTES: links source/opt/ssa/scalar/setif_or_taut.c via UT11.
 */

#include "ssa_build.h"
#include "source/opt/ssa/include/ssa_opt.h"
#include "opt/ssa/setif_or_taut.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"

#define I32 IROP_BTYPE_INT32

#define TOK_ULT 0x92
#define TOK_EQ  0x94
#define TOK_LT  0x9c
#define TOK_GE  0x9d
#define TOK_GT  0x9f

static inline IROperand utb_cond(int tok) { return irop_make_imm32(0, tok, I32); }

static void build(ssa_ctx *c)
{
  ssa_ctx_build_cfg(c);
  ssa_ctx_build_ssa_plain(c);
  ssa_ctx_rebuild(c);
}

/* LT | GE over the same (t0,t1) covers all states -> OR folds to #1. */
UT_TEST(test_ssa_setif_or_lt_ge_folds_to_one)
{
  ssa_ctx c = ssa_ctx_new(1, 8);
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(2, I32), utb_cond(TOK_LT));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(3, I32), utb_cond(TOK_GE));
  int ior = ssa_add_instr3(&c, TCCIR_OP_OR, utb_temp(4, I32), utb_temp(2, I32), utb_temp(3, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(4, I32));

  build(&c);
  int ch = ssa_opt_setif_or_taut(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, ior), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, ior)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, ior)), 1);
  ssa_ctx_free(&c);
  return 0;
}

/* Chain: (LT|EQ) then |GT reaches 0b111 -> the OUTER OR folds, inner stays. */
UT_TEST(test_ssa_setif_or_chain_lt_eq_gt_folds_outer)
{
  ssa_ctx c = ssa_ctx_new(1, 8);
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(2, I32), utb_cond(TOK_LT));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(3, I32), utb_cond(TOK_EQ));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(4, I32), utb_cond(TOK_GT));
  int ior1 = ssa_add_instr3(&c, TCCIR_OP_OR, utb_temp(5, I32), utb_temp(2, I32), utb_temp(3, I32));
  int ior2 = ssa_add_instr3(&c, TCCIR_OP_OR, utb_temp(6, I32), utb_temp(5, I32), utb_temp(4, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(6, I32));

  build(&c);
  int ch = ssa_opt_setif_or_taut(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, ior1), TCCIR_OP_OR);       /* LT|EQ = 0b011, not folded */
  UT_ASSERT_EQ(utb_op(c.ir, ior2), TCCIR_OP_ASSIGN);   /* |GT = 0b111, folded */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, ior2)), 1);
  ssa_ctx_free(&c);
  return 0;
}

/* LT | LT does not cover -> no fold. */
UT_TEST(test_ssa_setif_or_same_mask_no_fold)
{
  ssa_ctx c = ssa_ctx_new(1, 8);
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(2, I32), utb_cond(TOK_LT));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(3, I32), utb_cond(TOK_LT));
  int ior = ssa_add_instr3(&c, TCCIR_OP_OR, utb_temp(4, I32), utb_temp(2, I32), utb_temp(3, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(4, I32));

  build(&c);
  int ch = ssa_opt_setif_or_taut(c.ctx);
  UT_ASSERT_EQ(ch, 0);
  UT_ASSERT_EQ(utb_op(c.ir, ior), TCCIR_OP_OR);
  ssa_ctx_free(&c);
  return 0;
}

/* Different compared operands (t0,t1 vs t0,t5) are incompatible -> no fold. */
UT_TEST(test_ssa_setif_or_different_operands_no_fold)
{
  ssa_ctx c = ssa_ctx_new(1, 8);
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(2, I32), utb_cond(TOK_LT));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(5, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(3, I32), utb_cond(TOK_GE));
  int ior = ssa_add_instr3(&c, TCCIR_OP_OR, utb_temp(4, I32), utb_temp(2, I32), utb_temp(3, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(4, I32));

  build(&c);
  int ch = ssa_opt_setif_or_taut(c.ctx);
  UT_ASSERT_EQ(ch, 0);
  UT_ASSERT_EQ(utb_op(c.ir, ior), TCCIR_OP_OR);
  ssa_ctx_free(&c);
  return 0;
}

/* Signed vs unsigned mismatch (ULT | GE) is incompatible -> no fold. */
UT_TEST(test_ssa_setif_or_signedness_mismatch_no_fold)
{
  ssa_ctx c = ssa_ctx_new(1, 8);
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(2, I32), utb_cond(TOK_ULT));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(3, I32), utb_cond(TOK_GE));
  int ior = ssa_add_instr3(&c, TCCIR_OP_OR, utb_temp(4, I32), utb_temp(2, I32), utb_temp(3, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(4, I32));

  build(&c);
  int ch = ssa_opt_setif_or_taut(c.ctx);
  UT_ASSERT_EQ(ch, 0);
  UT_ASSERT_EQ(utb_op(c.ir, ior), TCCIR_OP_OR);
  ssa_ctx_free(&c);
  return 0;
}

/* OR of plain temps that are not SETIF results -> no fold. */
UT_TEST(test_ssa_setif_or_non_setif_operands_no_fold)
{
  ssa_ctx c = ssa_ctx_new(1, 8);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(3, I32), utb_imm(2, I32));
  int ior = ssa_add_instr3(&c, TCCIR_OP_OR, utb_temp(4, I32), utb_temp(2, I32), utb_temp(3, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(4, I32));

  build(&c);
  int ch = ssa_opt_setif_or_taut(c.ctx);
  UT_ASSERT_EQ(ch, 0);
  UT_ASSERT_EQ(utb_op(c.ir, ior), TCCIR_OP_OR);
  ssa_ctx_free(&c);
  return 0;
}

/* A never-written PARAM operand is a stable constant input (sc::foo shape):
 * (a<0)|(a>=0) over param a covers LT|GE = 0b111 -> folds to #1. */
UT_TEST(test_ssa_setif_or_param_operand_folds)
{
  ssa_ctx c = ssa_ctx_new(1, 8);
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(2, I32), utb_cond(TOK_LT));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_param(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(3, I32), utb_cond(TOK_GE));
  int ior = ssa_add_instr3(&c, TCCIR_OP_OR, utb_temp(4, I32), utb_temp(2, I32), utb_temp(3, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(4, I32));

  build(&c);
  int ch = ssa_opt_setif_or_taut(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, ior), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, ior)), 1);
  ssa_ctx_free(&c);
  return 0;
}

/* A VAR operand is memory (can be aliased/written) -> not stable -> no fold. */
UT_TEST(test_ssa_setif_or_var_operand_no_fold)
{
  ssa_ctx c = ssa_ctx_new(1, 8);
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(2, I32), utb_cond(TOK_LT));
  ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_var(0, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(3, I32), utb_cond(TOK_GE));
  int ior = ssa_add_instr3(&c, TCCIR_OP_OR, utb_temp(4, I32), utb_temp(2, I32), utb_temp(3, I32));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(4, I32));

  build(&c);
  int ch = ssa_opt_setif_or_taut(c.ctx);
  UT_ASSERT_EQ(ch, 0);
  UT_ASSERT_EQ(utb_op(c.ir, ior), TCCIR_OP_OR);
  ssa_ctx_free(&c);
  return 0;
}

UT_COVERS("ssa:setif_or_taut");

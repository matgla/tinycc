/*
 *  test_ssa_opt_vrp.c - SSA value range propagation (ssa:vrp)
 *
 *  Covers ssa_opt_vrp() (source/opt/ssa/cfg/vrp.c): per-SSA-value [min,max]
 *  ranges seeded from immediates / ADD/SUB/AND/shift defs and refined on the
 *  taken / fall-through edge of a dominating CMP+JUMPIF, folding CMP+JUMPIF and
 *  CMP+SETIF over constant compares.  Mirrors the flat test_opt_vrp.c behaviour
 *  and adds the dominance-scoped edge-refinement case the flat pass can't do.
 *
 *  HARNESS NOTES:
 *    - Links the real source/opt/ssa/cfg/vrp.c via UT11.
 *    - Uses ssa_build.h for hand-built cfg + ssa + vinfo.
 */

#include "ssa_build.h"
#include "source/opt/ssa/include/ssa_opt.h"
#include "opt/ssa/vrp.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"

#define I32 IROP_BTYPE_INT32

#define TOK_ULT 0x92
#define TOK_UGE 0x93
#define TOK_EQ  0x94
#define TOK_NE  0x95
#define TOK_LT  0x9c
#define TOK_GE  0x9d
#define TOK_LE  0x9e
#define TOK_GT  0x9f

static inline IROperand utb_cond(int tok) { return irop_make_imm32(0, tok, I32); }
static inline IROperand utb_jdest(int idx) { return irop_make_imm32(0, idx, I32); }

static inline int emit_cmp(ssa_ctx *c, IROperand a, IROperand b)
{
  return ssa_add_instr3(c, TCCIR_OP_CMP, UTB_NONE, a, b);
}

static inline int emit_jumpif(ssa_ctx *c, int tok, int target_idx)
{
  return ssa_add_instr(c, TCCIR_OP_JUMPIF, utb_jdest(target_idx), utb_cond(tok));
}

static void build(ssa_ctx *c)
{
  ssa_ctx_build_cfg(c);
  ssa_ctx_build_ssa_plain(c);
  ssa_ctx_rebuild(c);
}

/* ==================================================== singleton const ranges */

/* T0=#5 -> [5,5] proves CMP T0,#20 / JUMPIF LT always taken -> JUMP. */
UT_TEST(test_ssa_vrp_const_range_lt_folds_to_jump)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int icmp = emit_cmp(&c, utb_temp(0, I32), utb_imm(20, I32));
  int ijmp = emit_jumpif(&c, TOK_LT, 4);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)utb_dest(c.ir, ijmp).u.imm32, 4);
  ssa_ctx_free(&c);
  return 0;
}

/* Singleton [5,5]: CMP T0,#5 / JUMPIF EQ always true -> JUMP. */
UT_TEST(test_ssa_vrp_singleton_eq_folds_to_jump)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int icmp = emit_cmp(&c, utb_temp(0, I32), utb_imm(5, I32));
  int ijmp = emit_jumpif(&c, TOK_EQ, 4);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* Singleton [5,5]: CMP T0,#3 / JUMPIF LT always false -> both NOP. */
UT_TEST(test_ssa_vrp_singleton_lt_false_nops_both)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int icmp = emit_cmp(&c, utb_temp(0, I32), utb_imm(3, I32));
  int ijmp = emit_jumpif(&c, TOK_LT, 4);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_NOP);
  ssa_ctx_free(&c);
  return 0;
}

/* Non-negative singleton [5,5] proves ULT #10. */
UT_TEST(test_ssa_vrp_unsigned_range_ult_folds_to_jump)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int icmp = emit_cmp(&c, utb_temp(0, I32), utb_imm(10, I32));
  int ijmp = emit_jumpif(&c, TOK_ULT, 4);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* Negative singleton [-100,-100]: unsigned(-100) < unsigned(-1) -> ULT taken. */
UT_TEST(test_ssa_vrp_negative_range_unsigned_ult_folds_jump)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(-100, I32));
  int icmp = emit_cmp(&c, utb_temp(0, I32), utb_imm(-1, I32));
  int ijmp = emit_jumpif(&c, TOK_ULT, 4);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* ====================================================== arithmetic propagation */

/* Copy carries a range: T1 = T0 inherits [5,5]. */
UT_TEST(test_ssa_vrp_range_propagates_through_copy)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(5, I32));
  int ijmp = emit_jumpif(&c, TOK_EQ, 5);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* T0=#5 -> [5,5]; T1 = T0 + #3 -> [8,8] proves CMP T1,#8 / JUMPIF EQ taken. */
UT_TEST(test_ssa_vrp_add_propagates_range_folds_eq)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(3, I32));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(8, I32));
  int ijmp = emit_jumpif(&c, TOK_EQ, 5);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* T0=#10; T1 = T0 - #3 -> [7,7] proves CMP T1,#7 / JUMPIF NE always false. */
UT_TEST(test_ssa_vrp_sub_propagates_range_folds_ne_false)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  ssa_add_instr3(&c, TCCIR_OP_SUB, utb_temp(1, I32), utb_temp(0, I32), utb_imm(3, I32));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(7, I32));
  int ijmp = emit_jumpif(&c, TOK_NE, 5);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_NOP);
  ssa_ctx_free(&c);
  return 0;
}

/* AND with a non-negative mask bounds [0,mask]: T1 = T0 & (1<<26) is >= 0. */
UT_TEST(test_ssa_vrp_and_nonneg_mask_folds_ge_zero)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr3(&c, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1 << 26, I32));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(0, I32));
  int ijmp = emit_jumpif(&c, TOK_GE, 4);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* SHL scales a known range: T0=3, T1 = T0 << 2 = 12. */
UT_TEST(test_ssa_vrp_shl_scales_range_folds_eq)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(3, I32));
  ssa_add_instr3(&c, TCCIR_OP_SHL, utb_temp(1, I32), utb_temp(0, I32), utb_imm(2, I32));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(12, I32));
  int ijmp = emit_jumpif(&c, TOK_EQ, 5);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* SAR narrows a known range: T0=20, T1 = T0 >> 2 = 5. */
UT_TEST(test_ssa_vrp_sar_narrows_range_folds_eq)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(20, I32));
  ssa_add_instr3(&c, TCCIR_OP_SAR, utb_temp(1, I32), utb_temp(0, I32), utb_imm(2, I32));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(5, I32));
  int ijmp = emit_jumpif(&c, TOK_EQ, 5);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* SHR on a non-negative range narrows: T0=40, T1 = T0 >>u 3 = 5. */
UT_TEST(test_ssa_vrp_shr_nonneg_narrows_range_folds_eq)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(40, I32));
  ssa_add_instr3(&c, TCCIR_OP_SHR, utb_temp(1, I32), utb_temp(0, I32), utb_imm(3, I32));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(5, I32));
  int ijmp = emit_jumpif(&c, TOK_EQ, 5);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* ====================================================== zero tautology folds */

/* CMP T0,#0 / JUMPIF UGE is always true regardless of T0. */
UT_TEST(test_ssa_vrp_uge_zero_tautology_always_taken)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  int icmp = emit_cmp(&c, utb_temp(0, I32), utb_imm(0, I32));
  int ijmp = emit_jumpif(&c, TOK_UGE, 3);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* CMP T0,#0 / JUMPIF ULT is a contradiction -> both NOP. */
UT_TEST(test_ssa_vrp_ult_zero_tautology_never_taken)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  int icmp = emit_cmp(&c, utb_temp(0, I32), utb_imm(0, I32));
  int ijmp = emit_jumpif(&c, TOK_ULT, 3);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_NOP);
  ssa_ctx_free(&c);
  return 0;
}

/* ============================================================= CMP + SETIF */

/* T0=#5 -> [5,5]; CMP T0,#5 / SETIF EQ is always true -> ASSIGN #1. */
UT_TEST(test_ssa_vrp_cmp_setif_folds_to_const_one)
{
  ssa_ctx c = ssa_ctx_new(1, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int icmp = emit_cmp(&c, utb_temp(0, I32), utb_imm(5, I32));
  int iset = ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(1, I32), utb_cond(TOK_EQ));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32));

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, iset), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, iset)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, iset)), 1);
  ssa_ctx_free(&c);
  return 0;
}

/* T0=#5; CMP T0,#9 / SETIF EQ is always false -> ASSIGN #0. */
UT_TEST(test_ssa_vrp_cmp_setif_folds_to_const_zero)
{
  ssa_ctx c = ssa_ctx_new(1, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int icmp = emit_cmp(&c, utb_temp(0, I32), utb_imm(9, I32));
  int iset = ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(1, I32), utb_cond(TOK_EQ));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32));

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, iset), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, iset)), 0);
  ssa_ctx_free(&c);
  return 0;
}

/* T0=#5 -> [5,5] proves `CMP T0,#20 ; SELECT #1,#2 [cond=LT]` (dest = (T0<20) ? 1 : 2) picks the
 * then-arm: the SELECT becomes `dest <- #1` and the CMP drops. */
UT_TEST(test_ssa_vrp_cmp_select_folds_to_then_arm)
{
  ssa_ctx c = ssa_ctx_new(1, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int icmp = emit_cmp(&c, utb_temp(0, I32), utb_imm(20, I32));
  int isel = ssa_add_instr4(&c, TCCIR_OP_SELECT, utb_temp(1, I32),
                            utb_imm(1, I32), utb_imm(2, I32), utb_cond(TOK_LT));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32));

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, isel), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, isel)), 1);
  ssa_ctx_free(&c);
  return 0;
}

/* Logical SHR #29 of a 32-bit value is intrinsically in [0,7] regardless of the (unknown) input,
 * so `CMP T1,#8 ; JUMPIF GE` is proven never-taken even though T0 carries no range. */
UT_TEST(test_ssa_vrp_shr_intrinsic_range_no_src_range)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32)); /* T0 = P0, unknown */
  ssa_add_instr3(&c, TCCIR_OP_SHR, utb_temp(1, I32), utb_temp(0, I32), utb_imm(29, I32));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(8, I32));
  int ijmp = emit_jumpif(&c, TOK_GE, 5);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_NOP);
  ssa_ctx_free(&c);
  return 0;
}

/* ================================================= dominance-scoped edge refinement */

/* The SSA win the flat pass can't do across a real CFG edge: the entry
 *   CMP t0,#10 ; JUMPIF GT -> B
 * refines t0 to [11, INT32_MAX] in block B (uniquely dominated by the entry),
 * where a later CMP t0,#5 ; JUMPIF GT is proven always taken.  Layout:
 *   0 CMP t0,#10
 *   1 JUMPIF GT -> 4        (taken target = B)
 *   2 ASSIGN t2,#0          (fall-through block)
 *   3 JUMP -> 6
 *   4 CMP t0,#5             (B, dominated only by entry)  <- cmp1
 *   5 JUMPIF GT -> 6                                       <- jmp1
 *   6 RETURNVOID
 */
UT_TEST(test_ssa_vrp_taken_edge_refines_range_folds_later_cmp)
{
  ssa_ctx c = ssa_ctx_new(4, 4);
  emit_cmp(&c, utb_temp(0, I32), utb_imm(10, I32));
  emit_jumpif(&c, TOK_GT, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_jdest(6), UTB_NONE);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_imm(5, I32));
  int jmp1 = emit_jumpif(&c, TOK_GT, 6);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* Fall-through edge: entry CMP t0,#5 ; JUMPIF LT -> stub; on the fall-through
 * (NOT t0<5, i.e. t0>=5) a later CMP t0,#5 ; JUMPIF GE is always taken.  Layout:
 *   0 CMP t0,#5
 *   1 JUMPIF LT -> 5        (taken stub)
 *   2 CMP t0,#5             (fall-through block)  <- cmp1
 *   3 JUMPIF GE -> 6                              <- jmp1
 *   4 JUMP -> 6
 *   5 RETURNVOID            (stub)
 *   6 RETURNVOID
 */
UT_TEST(test_ssa_vrp_fallthrough_edge_refines_range_folds_later_cmp)
{
  ssa_ctx c = ssa_ctx_new(4, 4);
  emit_cmp(&c, utb_temp(0, I32), utb_imm(5, I32));
  emit_jumpif(&c, TOK_LT, 5);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_imm(5, I32));
  int jmp1 = emit_jumpif(&c, TOK_GE, 6);
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_jdest(6), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* ============================================== reg-reg compare-chain folds */

/* `(t0<t1) && (t0>t1)`: the fall-through of `CMP t0,t1 ; JUMPIF GE` proves t0<t1, so the
 * next block's `CMP t0,t1 ; JUMPIF LE` is always taken (LT implies LE).  Same operand order.
 *   0 CMP t0,t1
 *   1 JUMPIF GE -> 5        (taken stub; fall-through = LT)
 *   2 CMP t0,t1             (block B, fall-through)  <- cmp1
 *   3 JUMPIF LE -> 6                                 <- jmp1 (always taken)
 *   4 JUMP -> 6
 *   5 RETURNVOID            (stub)
 *   6 RETURNVOID
 */
UT_TEST(test_ssa_vrp_regreg_chain_same_order_folds_to_jump)
{
  ssa_ctx c = ssa_ctx_new(4, 4);
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  emit_jumpif(&c, TOK_GE, 5);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_LE, 6);
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_jdest(6), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* `(t0<t1) && (t1<t0)`: fall-through proves t0<t1; the swapped-operand `CMP t1,t0 ; JUMPIF GE`
 * has effective token swap(GE)=LE for (t0,t1), which LT implies -> always taken.
 *   0 CMP t0,t1
 *   1 JUMPIF GE -> 5        (fall-through = LT)
 *   2 CMP t1,t0             (operands swapped)  <- cmp1
 *   3 JUMPIF GE -> 6                            <- jmp1 (always taken)
 *   4 JUMP -> 6
 *   5 RETURNVOID
 *   6 RETURNVOID
 */
UT_TEST(test_ssa_vrp_regreg_chain_swapped_order_folds_to_jump)
{
  ssa_ctx c = ssa_ctx_new(4, 4);
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  emit_jumpif(&c, TOK_GE, 5);
  int cmp1 = emit_cmp(&c, utb_temp(1, I32), utb_temp(0, I32));
  int jmp1 = emit_jumpif(&c, TOK_GE, 6);
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_jdest(6), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* The reg-reg chain fold is restricted to the fall-through edge (as the flat pass is): when the
 * second CMP sits in the JUMPIF's TAKEN target block, no fact is carried and nothing folds.
 *   0 CMP t0,t1
 *   1 JUMPIF LT -> 4        (taken target = block B)
 *   2 ASSIGN t2,#0          (fall-through)
 *   3 JUMP -> 6
 *   4 CMP t0,t1             (B, the TAKEN target)  <- cmp1
 *   5 JUMPIF LE -> 6                               <- jmp1
 *   6 RETURNVOID
 */
UT_TEST(test_ssa_vrp_regreg_chain_taken_edge_no_fold)
{
  ssa_ctx c = ssa_ctx_new(4, 4);
  emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  emit_jumpif(&c, TOK_LT, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(0, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_jdest(6), UTB_NONE);
  int cmp1 = emit_cmp(&c, utb_temp(0, I32), utb_temp(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_LE, 6);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  ssa_opt_vrp(c.ctx);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_JUMPIF);
  ssa_ctx_free(&c);
  return 0;
}

/* A direct VAR-slot read (is_lval, VAR-typed) is a valid reg-reg operand: only the pred's
 * JUMPIF runs between the two identical CMPs, so V0/V1 hold the same values.  sv_same_operand
 * matches the full operand identity, so the chain folds just like the plain-vreg case. */
static inline IROperand utb_var_lval(int pos, int btype)
{
  IROperand op = utb_var(pos, btype);
  op.is_lval = 1;
  return op;
}

UT_TEST(test_ssa_vrp_regreg_chain_lval_var_operands_folds)
{
  ssa_ctx c = ssa_ctx_new(4, 4);
  emit_cmp(&c, utb_var_lval(0, I32), utb_var_lval(1, I32));
  emit_jumpif(&c, TOK_GE, 5);
  int cmp1 = emit_cmp(&c, utb_var_lval(0, I32), utb_var_lval(1, I32));
  int jmp1 = emit_jumpif(&c, TOK_LE, 6);
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_jdest(6), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* Addressing mismatch: the pred compares V0 direct but B compares *V0 (is_lval).  These are
 * different values, so sv_same_operand rejects the pair and nothing folds. */
UT_TEST(test_ssa_vrp_regreg_chain_operand_flag_mismatch_no_fold)
{
  ssa_ctx c = ssa_ctx_new(4, 4);
  emit_cmp(&c, utb_var(0, I32), utb_var(1, I32));            /* pred: V0, V1 direct */
  emit_jumpif(&c, TOK_GE, 5);
  int cmp1 = emit_cmp(&c, utb_var_lval(0, I32), utb_var(1, I32)); /* B: *V0 vs V1 */
  int jmp1 = emit_jumpif(&c, TOK_LE, 6);
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_jdest(6), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  ssa_opt_vrp(c.ctx);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_JUMPIF);
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================== stable-PARAM edge refinement */

/* A never-written PARAM is a constant input: `CMP P0,#5 ; JUMPIF LT` on the fall-through proves
 * P0 >= 5, folding a later `CMP P0,#5 ; JUMPIF GE` to an unconditional JUMP.  Needs next_parameter
 * set so the param-stability table covers P0. */
UT_TEST(test_ssa_vrp_stable_param_edge_refines_folds)
{
  ssa_ctx c = ssa_ctx_new(4, 4);
  c.ir->next_parameter = 1;
  emit_cmp(&c, utb_param(0, I32), utb_imm(5, I32));
  emit_jumpif(&c, TOK_LT, 5);
  int cmp1 = emit_cmp(&c, utb_param(0, I32), utb_imm(5, I32));
  int jmp1 = emit_jumpif(&c, TOK_GE, 6);
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_jdest(6), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* A PARAM written anywhere in the function is not a constant input, so no range is carried and
 * the compare stays conditional. */
UT_TEST(test_ssa_vrp_written_param_not_tracked)
{
  ssa_ctx c = ssa_ctx_new(4, 4);
  c.ir->next_parameter = 1;
  emit_cmp(&c, utb_param(0, I32), utb_imm(5, I32));
  emit_jumpif(&c, TOK_LT, 6);
  int cmp1 = emit_cmp(&c, utb_param(0, I32), utb_imm(5, I32));
  int jmp1 = emit_jumpif(&c, TOK_GE, 7);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_param(0, I32), utb_imm(0, I32)); /* writes P0 */
  ssa_add_instr(&c, TCCIR_OP_JUMP, utb_jdest(7), UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  ssa_opt_vrp(c.ctx);
  UT_ASSERT_EQ(utb_op(c.ir, cmp1), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(c.ir, jmp1), TCCIR_OP_JUMPIF);
  ssa_ctx_free(&c);
  return 0;
}

/* ==================================================================== guards */

/* No known range for T0 -> the branch stays conditional. */
UT_TEST(test_ssa_vrp_unknown_range_left_untouched)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  int icmp = emit_cmp(&c, utb_temp(0, I32), utb_imm(0, I32));
  int ijmp = emit_jumpif(&c, TOK_NE, 4);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT_EQ(ch, 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMPIF);
  ssa_ctx_free(&c);
  return 0;
}

/* Immediate in CMP src1 (swapped operands): `20 > T0[5,5]` folds taken, not the swapped verdict. */
UT_TEST(test_ssa_vrp_swapped_cmp_operands_no_fold)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int icmp = emit_cmp(&c, utb_imm(20, I32), utb_temp(0, I32));
  int ijmp = emit_jumpif(&c, TOK_GT, 4);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);   /* GT taken; a NOP here means operand swap */
  ssa_ctx_free(&c);
  return 0;
}

/* T1 = T0 + #3 where T0 has no range -> dest stays unknown, later CMP untouched. */
UT_TEST(test_ssa_vrp_add_unknown_src_no_fold)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(3, I32));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(8, I32));
  int ijmp = emit_jumpif(&c, TOK_EQ, 4);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT_EQ(ch, 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMPIF);
  ssa_ctx_free(&c);
  return 0;
}

/* ================================================= intrinsic bitfield / boolean ranges */

/* src2 packs lsb (bits 0-4) and width (bits 5-9); width 0 means 8 (same decode as fold_bfx_value). */
static inline IROperand utb_bfx(int lsb, int width) { return utb_imm(lsb | (width << 5), I32); }

/* UBFX width 4 is intrinsically [0,15] regardless of the (unknown) input, so `CMP T1,#20 ; JUMPIF LT`
 * is proven always taken even though T0 carries no range. */
UT_TEST(test_ssa_vrp_ubfx_intrinsic_range_folds_lt)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32)); /* T0 = P0, unknown */
  ssa_add_instr3(&c, TCCIR_OP_UBFX, utb_temp(1, I32), utb_temp(0, I32), utb_bfx(0, 4));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(20, I32));
  int ijmp = emit_jumpif(&c, TOK_LT, 5);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* UBFX width 4 is [0,15], so `CMP T1,#100 ; JUMPIF GE` is proven never taken. */
UT_TEST(test_ssa_vrp_ubfx_intrinsic_range_folds_ge_false)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32));
  ssa_add_instr3(&c, TCCIR_OP_UBFX, utb_temp(1, I32), utb_temp(0, I32), utb_bfx(0, 4));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(100, I32));
  int ijmp = emit_jumpif(&c, TOK_GE, 5);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_NOP);
  ssa_ctx_free(&c);
  return 0;
}

/* SBFX width 4 sign-extends to [-8,7], so `CMP T1,#7 ; JUMPIF LE` is proven always taken. */
UT_TEST(test_ssa_vrp_sbfx_intrinsic_range_folds_le)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32));
  ssa_add_instr3(&c, TCCIR_OP_SBFX, utb_temp(1, I32), utb_temp(0, I32), utb_bfx(0, 4));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(7, I32));
  int ijmp = emit_jumpif(&c, TOK_LE, 5);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* SBFX width 4 is [-8,7], so `CMP T1,#8 ; JUMPIF GE` is proven never taken. */
UT_TEST(test_ssa_vrp_sbfx_intrinsic_range_folds_ge_false)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32));
  ssa_add_instr3(&c, TCCIR_OP_SBFX, utb_temp(1, I32), utb_temp(0, I32), utb_bfx(0, 4));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(8, I32));
  int ijmp = emit_jumpif(&c, TOK_GE, 5);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_NOP);
  ssa_ctx_free(&c);
  return 0;
}

/* BOOL_AND produces a definitional {0,1}, so `CMP T1,#2 ; JUMPIF LT` is proven always taken. */
UT_TEST(test_ssa_vrp_bool_and_range_folds_lt)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32));
  ssa_add_instr3(&c, TCCIR_OP_BOOL_AND, utb_temp(1, I32), utb_temp(0, I32), utb_temp(0, I32));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(2, I32));
  int ijmp = emit_jumpif(&c, TOK_LT, 5);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* BOOL_OR produces a definitional {0,1}, so `CMP T1,#0 ; JUMPIF GE` is proven always taken. */
UT_TEST(test_ssa_vrp_bool_or_range_folds_ge)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32));
  ssa_add_instr3(&c, TCCIR_OP_BOOL_OR, utb_temp(1, I32), utb_temp(0, I32), utb_temp(0, I32));
  int icmp = emit_cmp(&c, utb_temp(1, I32), utb_imm(0, I32));
  int ijmp = emit_jumpif(&c, TOK_GE, 5);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* Empty CFG: the pass returns 0 immediately. */
UT_TEST(test_ssa_vrp_empty_cfg)
{
  ssa_ctx c = ssa_ctx_new(1, 4);
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  c.ctx->ir = c.ir;
  c.ctx->ssa = NULL;
  c.ctx->cfg = NULL;

  int ch = ssa_opt_vrp(c.ctx);
  UT_ASSERT_EQ(ch, 0);
  ssa_ctx_free(&c);
  return 0;
}

UT_COVERS("ssa:vrp");

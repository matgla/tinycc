/*
 *  test_ssa_opt_cmp_offset_fold.c - SSA CMP constant-offset fold
 *
 *  Covers ssa_opt_cmp_offset_fold() (source/opt/ssa/scalar/cmp_offset_fold.c):
 *  CMP A,B ; JUMPIF/SELECT folds when A = B + K (or shared base A=X+K1,
 *  B=X+K2) since A-B is a known constant.  Signed / EQ / NE only.
 *
 *  HARNESS NOTES: links source/opt/ssa/scalar/cmp_offset_fold.c via UT11.
 */

#include "ssa_build.h"
#include "source/opt/ssa/include/ssa_opt.h"
#include "opt/ssa/cmp_offset_fold.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"

#define I32 IROP_BTYPE_INT32

#define TOK_ULT 0x92
#define TOK_EQ  0x94
#define TOK_NE  0x95
#define TOK_LT  0x9c
#define TOK_GT  0x9f

static inline IROperand utb_cond(int tok) { return irop_make_imm32(0, tok, I32); }
static inline IROperand utb_jdest(int idx) { return irop_make_imm32(0, idx, I32); }

static void build(ssa_ctx *c)
{
  ssa_ctx_build_cfg(c);
  ssa_ctx_build_ssa_plain(c);
  ssa_ctx_rebuild(c);
}

/* t1 = t0 + 5 ; CMP t1,t0 ; JUMPIF GT -> delta 5 > 0 always -> JUMP. */
UT_TEST(test_ssa_cmp_offset_add_gt_folds_jump)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_jdest(4), utb_cond(TOK_GT));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_cmp_offset_fold(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)utb_dest(c.ir, ijmp).u.imm32, 4);
  ssa_ctx_free(&c);
  return 0;
}

/* t1 = t0 + 5 ; CMP t1,t0 ; JUMPIF LT -> delta 5 < 0 never -> both NOP. */
UT_TEST(test_ssa_cmp_offset_add_lt_nops_both)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_jdest(4), utb_cond(TOK_LT));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_cmp_offset_fold(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_NOP);
  ssa_ctx_free(&c);
  return 0;
}

/* Reversed orientation: t0 = t1 + 5 (so vr2 = vr1 + K) ; CMP t1,t0 ; NE -> delta -5 != 0 -> JUMP. */
UT_TEST(test_ssa_cmp_offset_reversed_ne_folds_jump)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(1, I32), utb_imm(5, I32));
  int icmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_jdest(4), utb_cond(TOK_NE));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_cmp_offset_fold(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* Common base: t1 = t0+5, t2 = t0+2 ; CMP t1,t2 ; GT -> delta 3 > 0 -> JUMP. */
UT_TEST(test_ssa_cmp_offset_common_base_gt_folds_jump)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_imm(2, I32));
  int icmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(2, I32));
  int ijmp = ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_jdest(5), utb_cond(TOK_GT));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_cmp_offset_fold(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMP);
  ssa_ctx_free(&c);
  return 0;
}

/* SELECT form: t1 = t0+5 ; CMP t1,t0 ; SELECT t3 = (GT ? t8 : t9) -> chosen then (t8). */
UT_TEST(test_ssa_cmp_offset_select_folds_to_assign)
{
  ssa_ctx c = ssa_ctx_new(1, 10);
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int isel = ssa_add_instr4(&c, TCCIR_OP_SELECT, utb_temp(3, I32),
                            utb_temp(8, I32), utb_temp(9, I32), utb_cond(TOK_GT));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32));

  build(&c);
  int ch = ssa_opt_cmp_offset_fold(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, isel), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(c.ir, isel)), 8);
  ssa_ctx_free(&c);
  return 0;
}

/* Unsigned condition is not folded (needs an overflow proof). */
UT_TEST(test_ssa_cmp_offset_unsigned_no_fold)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int ijmp = ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_jdest(4), utb_cond(TOK_ULT));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_cmp_offset_fold(c.ctx);
  UT_ASSERT_EQ(ch, 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_CMP);
  UT_ASSERT_EQ(utb_op(c.ir, ijmp), TCCIR_OP_JUMPIF);
  ssa_ctx_free(&c);
  return 0;
}

/* No offset relationship between the two operands -> no fold. */
UT_TEST(test_ssa_cmp_offset_unrelated_no_fold)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  int icmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), utb_temp(1, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_jdest(3), utb_cond(TOK_GT));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_cmp_offset_fold(c.ctx);
  UT_ASSERT_EQ(ch, 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_CMP);
  ssa_ctx_free(&c);
  return 0;
}

/* Zero offset (t1 = t0 + 0) is skipped (copy, handled elsewhere) -> no fold. */
UT_TEST(test_ssa_cmp_offset_zero_delta_no_fold)
{
  ssa_ctx c = ssa_ctx_new(3, 4);
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0, I32));
  int icmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_JUMPIF, utb_jdest(4), utb_cond(TOK_EQ));
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);
  ssa_add_instr(&c, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE);

  build(&c);
  int ch = ssa_opt_cmp_offset_fold(c.ctx);
  UT_ASSERT_EQ(ch, 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_CMP);
  ssa_ctx_free(&c);
  return 0;
}

/* SETIF consumer: t1 = t0+5 ; CMP t1,t0 ; SETIF GT -> delta 5 > 0 -> ASSIGN #1. */
UT_TEST(test_ssa_cmp_offset_setif_gt_folds_to_one)
{
  ssa_ctx c = ssa_ctx_new(1, 4);
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int iset = ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(2, I32), utb_cond(TOK_GT));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32));

  build(&c);
  int ch = ssa_opt_cmp_offset_fold(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, iset), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(c.ir, iset)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, iset)), 1);
  UT_ASSERT_EQ(utb_vreg_pos(utb_dest(c.ir, iset)), 2);
  ssa_ctx_free(&c);
  return 0;
}

/* SETIF consumer, false: t1 = t0+5 ; CMP t1,t0 ; SETIF LT -> delta 5 < 0 -> ASSIGN #0. */
UT_TEST(test_ssa_cmp_offset_setif_lt_folds_to_zero)
{
  ssa_ctx c = ssa_ctx_new(1, 4);
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(5, I32));
  int icmp = ssa_add_instr3(&c, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_temp(0, I32));
  int iset = ssa_add_instr(&c, TCCIR_OP_SETIF, utb_temp(2, I32), utb_cond(TOK_LT));
  ssa_add_instr(&c, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32));

  build(&c);
  int ch = ssa_opt_cmp_offset_fold(c.ctx);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(c.ir, icmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(c.ir, iset), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(c.ir, utb_src1(c.ir, iset)), 0);
  ssa_ctx_free(&c);
  return 0;
}

UT_COVERS("ssa:cmp_offset_fold");

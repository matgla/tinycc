/*
 *  test_ssa_opt_reassoc.c - reassociation pass
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_reassoc(): reordering associative operations so constant
 *      operands bubble together, enabling the fold pass to collapse them.
 *    - reassoc_binary(): (x OP c1) OP c2 → x OP (c1 OP c2)
 *    - reassoc_add_cancel_const(): (a+c) + (a-c) → a+a
 *    - dispatch table: ADD, SUB, MUL, AND, OR, XOR, SHL, SHR, SAR, ROR
 *
 *  HARNESS NOTES:
 *    - Links the real source/opt/ssa/scalar/reassoc.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 */

#include "ssa_build.h"
#include "ir/opt/ssa_opt.h"
#include "opt/ssa/reassoc.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "ir/opt/ssa_opt.h"

#define I32 IROP_BTYPE_INT32

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

static int reassoc_emit_imm(ssa_ctx *c, TccIrOp op, int dst, int src,
                            int32_t imm)
{
  return ssa_add_instr3(c, op, utb_temp(dst, I32), utb_temp(src, I32),
                        utb_imm(imm, I32));
}

static int reassoc_emit_tmp(ssa_ctx *c, TccIrOp op, int dst, int src1,
                            int src2)
{
  return ssa_add_instr3(c, op, utb_temp(dst, I32), utb_temp(src1, I32),
                        utb_temp(src2, I32));
}

/* ========================================================================
 * Reassociation of addition: (a + c1) + c2 → a + (c1+c2)
 * ======================================================================== */

UT_TEST(test_reassoc_add_positive)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1; t1 = t0 + #2 → t2 = t1 + #3 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 1, 0, 2);
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 2, 1, 3);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* The outer ADD should now be: t2 = t0 + (2+3) = t0 + 5 */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_ADD);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, 2)), IROP_VR(utb_temp(0, I32)));
  IROperand src2 = utb_src2(c.ir, 2);
  UT_ASSERT(src2.tag == IROP_TAG_IMM32);
  UT_ASSERT_EQ(src2.u.imm32, 5);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Reassociation of addition: (a - c1) + c2 → a + (c2-c1)
 * ======================================================================== */

UT_TEST(test_reassoc_sub_then_add)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #5; t1 = t0 - #2; t2 = t1 + #3 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  reassoc_emit_imm(&c, TCCIR_OP_SUB, 1, 0, 2);
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 2, 1, 3);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* Outer is now: t2 = t0 + (3-2) = t0 + 1 */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_ADD);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, 2)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, 2).u.imm32, 1);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Reassociation of addition: (a + c1) - c2 → a + (c1-c2)
 * ======================================================================== */

UT_TEST(test_reassoc_add_then_sub)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #5; t1 = t0 + #3; t2 = t1 - #2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 1, 0, 3);
  reassoc_emit_imm(&c, TCCIR_OP_SUB, 2, 1, 2);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* Outer: t2 = t0 + (3-2) = t0 + 1 */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_ADD);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, 2)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, 2).u.imm32, 1);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Reassociation of subtraction: (a - c1) - c2 → a - (c1+c2)
 * ======================================================================== */

UT_TEST(test_reassoc_sub_sub)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #5; t1 = t0 - #2; t2 = t1 - #3 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  reassoc_emit_imm(&c, TCCIR_OP_SUB, 1, 0, 2);
  reassoc_emit_imm(&c, TCCIR_OP_SUB, 2, 1, 3);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* Outer: t2 = t0 - (2+3) = t0 - 5 */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_SUB);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, 2)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, 2).u.imm32, 5);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Reassociation of multiplication: (a * c1) * c2 → a * (c1*c2)
 * ======================================================================== */

UT_TEST(test_reassoc_mul)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #2; t1 = t0 * #3; t2 = t1 * #4 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(2, I32));
  reassoc_emit_imm(&c, TCCIR_OP_MUL, 1, 0, 3);
  reassoc_emit_imm(&c, TCCIR_OP_MUL, 2, 1, 4);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* Outer: t2 = t0 * (3*4) = t0 * 12 */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_MUL);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, 2)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, 2).u.imm32, 12);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Reassociation of AND: (a & c1) & c2 → a & (c1&c2)
 * ======================================================================== */

UT_TEST(test_reassoc_and)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #0xFF; t1 = t0 & #0x0F; t2 = t1 & #0xF0 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xFF, I32));
  reassoc_emit_imm(&c, TCCIR_OP_AND, 1, 0, 0x0F);
  reassoc_emit_imm(&c, TCCIR_OP_AND, 2, 1, 0xF0);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* Outer: t2 = t0 & (0x0F & 0xF0) = t0 & 0 */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_AND);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, 2)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, 2).u.imm32, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Reassociation of OR: (a | c1) | c2 → a | (c1|c2)
 * ======================================================================== */

UT_TEST(test_reassoc_or)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #0; t1 = t0 | #1; t2 = t1 | #2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0, I32));
  reassoc_emit_imm(&c, TCCIR_OP_OR, 1, 0, 1);
  reassoc_emit_imm(&c, TCCIR_OP_OR, 2, 1, 2);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* Outer: t2 = t0 | (1|2) = t0 | 3 */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_OR);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, 2)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, 2).u.imm32, 3);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Reassociation of XOR: (a ^ c1) ^ c2 → a ^ (c1^c2)
 * ======================================================================== */

UT_TEST(test_reassoc_xor)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #0xAA; t1 = t0 ^ #0x0F; t2 = t1 ^ #0xF0 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0xAA, I32));
  reassoc_emit_imm(&c, TCCIR_OP_XOR, 1, 0, 0x0F);
  reassoc_emit_imm(&c, TCCIR_OP_XOR, 2, 1, 0xF0);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* Outer: t2 = t0 ^ (0x0F ^ 0xF0) = t0 ^ 0xFF */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_XOR);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, 2)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, 2).u.imm32, 0xFF);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Reassociation of SHL: (a << c1) << c2 → a << (c1+c2)
 * ======================================================================== */

UT_TEST(test_reassoc_shl)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1; t1 = t0 << #2; t2 = t1 << #3 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  reassoc_emit_imm(&c, TCCIR_OP_SHL, 1, 0, 2);
  reassoc_emit_imm(&c, TCCIR_OP_SHL, 2, 1, 3);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* Outer: t2 = t0 << (2+3) = t0 << 5 */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_SHL);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, 2)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, 2).u.imm32, 5);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Reassociation of SHR: (a >> c1) >> c2 → a >> (c1+c2)
 * ======================================================================== */

UT_TEST(test_reassoc_shr)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #256; t1 = t0 >> #2; t2 = t1 >> #3 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(256, I32));
  reassoc_emit_imm(&c, TCCIR_OP_SHR, 1, 0, 2);
  reassoc_emit_imm(&c, TCCIR_OP_SHR, 2, 1, 3);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* Outer: t2 = t0 >> (2+3) = t0 >> 5 */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_SHR);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, 2)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, 2).u.imm32, 5);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Reassociation of SAR: (a >>> c1) >>> c2 → a >>> (c1+c2)
 * ======================================================================== */

UT_TEST(test_reassoc_sar)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #-16; t1 = t0 >>> #1; t2 = t1 >>> #2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(-16, I32));
  reassoc_emit_imm(&c, TCCIR_OP_SAR, 1, 0, 1);
  reassoc_emit_imm(&c, TCCIR_OP_SAR, 2, 1, 2);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* Outer: t2 = t0 >>> (1+2) = t0 >>> 3 */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_SAR);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, 2)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, 2).u.imm32, 3);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Reassociation of ROR: (a >>>= c1) >>>= c2 → a >>>= (c1+c2) mod 32
 * ======================================================================== */

UT_TEST(test_reassoc_ror)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #0x80000000; t1 = t0 >>>= #4; t2 = t1 >>>= #8 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0x80000000, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ROR, 1, 0, 4);
  reassoc_emit_imm(&c, TCCIR_OP_ROR, 2, 1, 8);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* Outer: t2 = t0 >>>= (4+8) = t0 >>>= 12 */
  UT_ASSERT_EQ(utb_op(c.ir, 2), TCCIR_OP_ROR);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, 2)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, 2).u.imm32, 12);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * SHL overflow guard: (a << 20) << 20 → no reassoc (20+20 >= 32)
 * ======================================================================== */

UT_TEST(test_reassoc_shl_overflow)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1; t1 = t0 << #20; t2 = t1 << #20 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  reassoc_emit_imm(&c, TCCIR_OP_SHL, 1, 0, 20);
  int i_outer = reassoc_emit_imm(&c, TCCIR_OP_SHL, 2, 1, 20);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, i_outer), TCCIR_OP_SHL);
  /* src2 should still be #20, not combined */
  UT_ASSERT_EQ(utb_src2(c.ir, i_outer).u.imm32, 20);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * SHR overflow guard: (a >> 20) >> 20 → no reassoc
 * ======================================================================== */

UT_TEST(test_reassoc_shr_overflow)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #0x100000; t1 = t0 >> #20; t2 = t1 >> #20 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(0x100000, I32));
  reassoc_emit_imm(&c, TCCIR_OP_SHR, 1, 0, 20);
  int i_outer = reassoc_emit_imm(&c, TCCIR_OP_SHR, 2, 1, 20);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, i_outer), TCCIR_OP_SHR);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No reassociation when inner def has multiple uses
 * ======================================================================== */

UT_TEST(test_reassoc_multi_use_inner)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #1; t1 = t0 + #2; t2 = t1 + #3; t3 = t1 + #4 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 1, 0, 2);
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 2, 1, 3);
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 3, 1, 4);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  /* t1 has 2 uses, so neither outer can reassociate */
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No reassociation when outer src2 is not an immediate
 * ======================================================================== */

UT_TEST(test_reassoc_no_imm_outer)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1; t1 = #2; t2 = t0 + #3; t3 = t2 + t1 (src2 is vreg, not imm) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 2, 0, 3);
  int i3 = reassoc_emit_tmp(&c, TCCIR_OP_ADD, 3, 2, 1);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, i3), TCCIR_OP_ADD);
  UT_ASSERT_EQ(IROP_VR(utb_src2(c.ir, i3)), IROP_VR(utb_temp(1, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No reassociation when inner src1 is a memory/local operand (lval)
 * ======================================================================== */

UT_TEST(test_reassoc_inner_lval_src1)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = &a (lval); t1 = t0 + #2; t2 = t1 + #3 */
  IROperand lval = utb_lval(utb_var(0, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), lval,
                 utb_imm(2, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 2, 1, 3);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No reassociation when inner src1 is a local (is_local)
 * ======================================================================== */

UT_TEST(test_reassoc_inner_local_src1)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = local(0); t1 = t0 + #2; t2 = t1 + #3 */
  IROperand local = utb_llocal(utb_temp(0, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32), local,
                 utb_imm(2, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 2, 1, 3);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No reassociation when inner src2 is not an immediate
 * ======================================================================== */

UT_TEST(test_reassoc_inner_no_imm_src2)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #1; t1 = #2; t2 = t0 + t1; t3 = t2 + #3 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  reassoc_emit_tmp(&c, TCCIR_OP_ADD, 2, 0, 1);
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 3, 2, 3);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No reassociation for non-matching op pairs: ADD outer / MUL inner
 * ======================================================================== */

UT_TEST(test_reassoc_mismatched_ops)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #2; t1 = t0 * #3; t2 = t1 + #4 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(2, I32));
  reassoc_emit_imm(&c, TCCIR_OP_MUL, 1, 0, 3);
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 2, 1, 4);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No reassociation when outer src1 is a PARAM vreg (not TEMP)
 * ======================================================================== */

UT_TEST(test_reassoc_param_src1)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t1 = PARAM + #3 */
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32),
                 utb_param(0, I32), utb_imm(3, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  /* The outer src1 is not a TEMP vreg; reassoc_binary rejects it. */
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * add_cancel_const: (a+c) + (a-c) → a+a
 * ======================================================================== */

UT_TEST(test_reassoc_add_cancel_same_base)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #10; t1 = t0 + #5; t2 = t0 - #5; t3 = t1 + t2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 1, 0, 5);
  reassoc_emit_imm(&c, TCCIR_OP_SUB, 2, 0, 5);
  int i3 = reassoc_emit_tmp(&c, TCCIR_OP_ADD, 3, 1, 2);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* Outer: t3 = t0 + t0 */
  UT_ASSERT_EQ(utb_op(c.ir, i3), TCCIR_OP_ADD);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i3)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(IROP_VR(utb_src2(c.ir, i3)), IROP_VR(utb_temp(0, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * add_cancel_const: (a+c) + (a+(-c)) → a+a (negative constant)
 * ======================================================================== */

UT_TEST(test_reassoc_add_cancel_neg_const)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #10; t1 = t0 + #5; t2 = t0 + #-5; t3 = t1 + t2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 1, 0, 5);
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 2, 0, -5);
  int i3 = reassoc_emit_tmp(&c, TCCIR_OP_ADD, 3, 1, 2);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* Outer: t3 = t0 + t0 */
  UT_ASSERT_EQ(utb_op(c.ir, i3), TCCIR_OP_ADD);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i3)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(IROP_VR(utb_src2(c.ir, i3)), IROP_VR(utb_temp(0, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * add_cancel_const: (a-c) + (a+c) → a+a (reversed order)
 * ======================================================================== */

UT_TEST(test_reassoc_add_cancel_reversed)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #10; t1 = t0 - #5; t2 = t0 + #5; t3 = t1 + t2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  reassoc_emit_imm(&c, TCCIR_OP_SUB, 1, 0, 5);
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 2, 0, 5);
  int i3 = reassoc_emit_tmp(&c, TCCIR_OP_ADD, 3, 1, 2);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* Outer: t3 = t0 + t0 */
  UT_ASSERT_EQ(utb_op(c.ir, i3), TCCIR_OP_ADD);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i3)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(IROP_VR(utb_src2(c.ir, i3)), IROP_VR(utb_temp(0, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No cancel: constants don't cancel (c1 != |c2| with opposite signs)
 * ======================================================================== */

UT_TEST(test_reassoc_add_cancel_no_match)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #10; t1 = t0 + #5; t2 = t0 - #3; t3 = t1 + t2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 1, 0, 5);
  reassoc_emit_imm(&c, TCCIR_OP_SUB, 2, 0, 3);
  reassoc_emit_tmp(&c, TCCIR_OP_ADD, 3, 1, 2);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  /* c1=5, c2=3 → 5-3=2 ≠ 0, no cancel */
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No cancel: different base vregs
 * ======================================================================== */

UT_TEST(test_reassoc_add_cancel_different_base)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  /* t0 = #10; t1 = #20; t2 = t0 + #5; t3 = t1 - #5; t4 = t2 + t3 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(20, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 2, 0, 5);
  reassoc_emit_imm(&c, TCCIR_OP_SUB, 3, 1, 5);
  reassoc_emit_tmp(&c, TCCIR_OP_ADD, 4, 2, 3);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  /* Different base vregs (t0 vs t1) → no cancel */
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No cancel: outer src operands are not vregs
 * ======================================================================== */

UT_TEST(test_reassoc_add_cancel_non_vreg_srcs)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #5; t1 = #5; t2 = t0 + t1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(5, I32));
  reassoc_emit_tmp(&c, TCCIR_OP_ADD, 2, 0, 1);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  /* Both srcs are defined by ASSIGN (not ADD/SUB), so cancel doesn't match */
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No cancel: inner defs have multiple uses
 * ======================================================================== */

UT_TEST(test_reassoc_add_cancel_multi_use)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  /* t0 = #10; t1 = t0 + #5; t2 = t0 - #5; t3 = t1 + t2; t4 = t1 + #1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 1, 0, 5);
  reassoc_emit_imm(&c, TCCIR_OP_SUB, 2, 0, 5);
  reassoc_emit_tmp(&c, TCCIR_OP_ADD, 3, 1, 2);
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 4, 1, 1);
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 5, 2, 1);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  /* t1 has 2 uses → cancel won't fire */
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No cancel: inner ops are not ADD/SUB
 * ======================================================================== */

UT_TEST(test_reassoc_add_cancel_wrong_inner_ops)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #10; t1 = t0 * #5; t2 = t0 / #5; t3 = t1 + t2 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  reassoc_emit_imm(&c, TCCIR_OP_MUL, 1, 0, 5);
  reassoc_emit_imm(&c, TCCIR_OP_UDIV, 2, 0, 5);
  reassoc_emit_tmp(&c, TCCIR_OP_ADD, 3, 1, 2);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  /* MUL/UDIV are not ADD/SUB → cancel won't match */
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Chained reassociation: (a+c1)+c2 then +c3 → a+(c1+c2+c3)
 * ======================================================================== */

UT_TEST(test_reassoc_chain_three)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = #1; t1 = t0 + #2; t2 = t1 + #3; t3 = t2 + #4 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 1, 0, 2);
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 2, 1, 3);
  int i3 = reassoc_emit_imm(&c, TCCIR_OP_ADD, 3, 2, 4);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  /* A single forward scan can reassociate the whole chain. */
  UT_ASSERT(changed >= 1);

  /* Final: t3 = t0 + 9 */
  UT_ASSERT_EQ(utb_op(c.ir, i3), TCCIR_OP_ADD);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i3)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, i3).u.imm32, 9);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No reassociation: outer src2 is an lval
 * ======================================================================== */

UT_TEST(test_reassoc_outer_lval_src2)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1; t1 = t0 + #2; t2 = t1 + &a (lval) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32));
  int i2 = ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                         utb_temp(1, I32));

  /* Patch src2 to be an lval */
  IROperand *pool = c.ir->iroperand_pool;
  int base = c.ir->compact_instructions[i2].operand_base + 2; /* src2 */
  pool[base].is_lval = 1;

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No reassociation: SUB outer, ADD inner → handled by reassoc_binary
 * (x + c1) - c2 = x + (c1-c2)
 * ======================================================================== */

UT_TEST(test_reassoc_sub_outer_add_inner_positive_combined)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #10; t1 = t0 + #8; t2 = t1 - #3 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 1, 0, 8);
  int i2 = reassoc_emit_imm(&c, TCCIR_OP_SUB, 2, 1, 3);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* (t0 + 8) - 3 = t0 + (8-3) = t0 + 5 */
  UT_ASSERT_EQ(utb_op(c.ir, i2), TCCIR_OP_ADD);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i2)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, i2).u.imm32, 5);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No reassociation: SUB outer, ADD inner → negative combined → SUB
 * (x + c1) - c2 = x - (c2-c1) when c2 > c1
 * ======================================================================== */

UT_TEST(test_reassoc_sub_outer_add_inner_negative_combined)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #10; t1 = t0 + #3; t2 = t1 - #8 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32));
  reassoc_emit_imm(&c, TCCIR_OP_ADD, 1, 0, 3);
  int i2 = reassoc_emit_imm(&c, TCCIR_OP_SUB, 2, 1, 8);

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT(changed >= 1);

  /* (t0 + 3) - 8 = t0 + (3-8) = t0 - 5 */
  UT_ASSERT_EQ(utb_op(c.ir, i2), TCCIR_OP_SUB);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i2)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_src2(c.ir, i2).u.imm32, 5);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * No reassociation when vinfo is missing for inner src1
 * ======================================================================== */

UT_TEST(test_reassoc_no_vinfo_inner)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1; t1 = t0 + #2; t2 = t1 + #3 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                         utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  /* Don't call ssa_ctx_rebuild — vinfo will be NULL */

  /* Manually create a ctx without vinfo */
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  c.ctx->ir = c.ir;
  c.ctx->ssa = c.ssa;
  c.ctx->cfg = c.cfg;
  c.ctx->vinfo_cap = c.num_temps;
  c.ctx->vinfo = NULL;

  int changed = ssa_opt_reassoc(c.ctx);
  UT_ASSERT_EQ(changed, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_COVERS("ssa:reassoc");

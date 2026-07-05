/*
 *  test_ssa_opt_fold.c - constant folding pass
 *
 *  Phase 2: Leaf passes.
 *
 *  Covers:
 *    - ssa_opt_fold(): evaluate ALU ops with two immediate operands
 *      * 32-bit and 64-bit full constant folding (ADD/SUB/MUL/AND/OR/XOR/SHL/
 *        SHR/SAR/ROR/DIV/UDIV/IMOD/UMOD)
 *      * Division-by-zero and INT_MIN/-1 overflow guards
 *      * Algebraic identities: x + 0, x - 0, x * 1, x | 0, x ^ 0, shifts by 0
 *      * Absorbing elements: x * 0, x & 0, x | ~0
 *      * x op x patterns (SUB/XOR -> 0, AND/OR -> self)
 *      * Bit-complement collapse: a | (a ^ -1) -> -1, a & (a ^ -1) -> 0
 *      * Double-negation collapse: 0 - (0 - x) -> x
 *      * Cross-block const-vreg resolution is rejected
 *      * Barrel-shift annotations block folding
 *      * Non-TEMP destinations are left untouched
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt_fold.c via UT11.
 *    - Uses ssa_build.h for hand-built vinfo + IR.
 */

#include "ssa_build.h"
#include "ir/opt/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "ir/opt/ssa_opt.h"

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* ========================================================================
 * Helpers
 * ======================================================================== */

static int fold_emit_binimm_bt(ssa_ctx *c, TccIrOp op, int dst, int btype,
                               int32_t a, int32_t b)
{
  return ssa_add_instr3(c, op, utb_temp(dst, btype),
                        irop_make_imm32(0, a, btype),
                        irop_make_imm32(0, b, btype));
}

static int fold_emit_binimm(ssa_ctx *c, TccIrOp op, int dst, int32_t a,
                            int32_t b)
{
  return fold_emit_binimm_bt(c, op, dst, I32, a, b);
}

static void fold_build_rebuild(ssa_ctx *c)
{
  ssa_ctx_build_cfg(c);
  ssa_ctx_build_ssa_plain(c);
  ssa_ctx_rebuild(c);
}

/* ========================================================================
 * Legacy smoke tests (kept for compatibility)
 * ======================================================================== */

UT_TEST(test_fold_add_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32));
  fold_build_rebuild(&c);
  int changed = ssa_opt_fold(c.ctx);
  UT_ASSERT(changed >= 0);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_mul_one)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32));
  ssa_add_instr(&c, TCCIR_OP_MUL, utb_temp(1, I32), utb_temp(0, I32));
  fold_build_rebuild(&c);
  int changed = ssa_opt_fold(c.ctx);
  UT_ASSERT(changed >= 0);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_mul_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_MUL, utb_temp(1, I32), utb_temp(0, I32));
  fold_build_rebuild(&c);
  int changed = ssa_opt_fold(c.ctx);
  UT_ASSERT(changed >= 0);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_non_immediate)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(3, I32));
  int add_i = ssa_add_instr(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                            utb_temp(0, I32));
  fold_build_rebuild(&c);
  int changed = ssa_opt_fold(c.ctx);
  UT_ASSERT_EQ(changed, 0);
  UT_ASSERT_EQ(utb_op(c.ir, add_i), TCCIR_OP_ADD);
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Constant folding: both operands immediate, 32-bit
 * ======================================================================== */

UT_TEST(test_fold_add_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm(&c, TCCIR_OP_ADD, 0, 3, 5);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 8);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_sub_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm(&c, TCCIR_OP_SUB, 0, 10, 4);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 6);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_mul_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm(&c, TCCIR_OP_MUL, 0, 6, 7);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 42);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_and_or_xor_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  int i_and = fold_emit_binimm(&c, TCCIR_OP_AND, 0, 0xF0, 0x0F);
  int i_or  = fold_emit_binimm(&c, TCCIR_OP_OR,  1, 0xF0, 0x0F);
  int i_xor = fold_emit_binimm(&c, TCCIR_OP_XOR, 2, 0xFF, 0x0F);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 3);
  UT_ASSERT_EQ(utb_src1(c.ir, i_and).u.imm32, 0);
  UT_ASSERT_EQ(utb_src1(c.ir, i_or).u.imm32, 0xFF);
  UT_ASSERT_EQ(utb_src1(c.ir, i_xor).u.imm32, 0xF0);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_shl_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm(&c, TCCIR_OP_SHL, 0, 1, 8);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 0x100);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_shr_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm(&c, TCCIR_OP_SHR, 0, 0x100, 4);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 0x10);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_sar_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm(&c, TCCIR_OP_SAR, 0, -16, 2);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, -4);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_ror_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm(&c, TCCIR_OP_ROR, 0, 0x80000000, 4);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 0x08000000);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_div_udiv_imod_umod_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  int i_div  = fold_emit_binimm(&c, TCCIR_OP_DIV,  0, 20, 5);
  int i_udiv = fold_emit_binimm(&c, TCCIR_OP_UDIV, 1, 20, 5);
  int i_imod = fold_emit_binimm(&c, TCCIR_OP_IMOD, 2, 20, 6);
  int i_umod = fold_emit_binimm(&c, TCCIR_OP_UMOD, 3, 20, 6);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 4);
  UT_ASSERT_EQ(utb_src1(c.ir, i_div).u.imm32, 4);
  UT_ASSERT_EQ(utb_src1(c.ir, i_udiv).u.imm32, 4);
  UT_ASSERT_EQ(utb_src1(c.ir, i_imod).u.imm32, 2);
  UT_ASSERT_EQ(utb_src1(c.ir, i_umod).u.imm32, 2);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_div_by_zero_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm(&c, TCCIR_OP_DIV, 0, 5, 0);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_DIV);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_udiv_by_zero_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm(&c, TCCIR_OP_UDIV, 0, 5, 0);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_UDIV);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_imod_by_zero_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm(&c, TCCIR_OP_IMOD, 0, 5, 0);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_IMOD);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_umod_by_zero_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm(&c, TCCIR_OP_UMOD, 0, 5, 0);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_UMOD);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_int_min_div_minus_one_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm(&c, TCCIR_OP_DIV, 0, INT32_MIN, -1);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_DIV);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_shifts_overflow_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  int i_shl = fold_emit_binimm(&c, TCCIR_OP_SHL, 0, 1, 32);
  int i_shr = fold_emit_binimm(&c, TCCIR_OP_SHR, 1, 1, 32);
  int i_sar = fold_emit_binimm(&c, TCCIR_OP_SAR, 2, -1, 32);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 3);
  UT_ASSERT_EQ(utb_src1(c.ir, i_shl).u.imm32, 0);
  UT_ASSERT_EQ(utb_src1(c.ir, i_shr).u.imm32, 0);
  UT_ASSERT_EQ(utb_src1(c.ir, i_sar).u.imm32, -1);
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Constant folding: 64-bit operations
 * ======================================================================== */

UT_TEST(test_fold_add_imm64)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm_bt(&c, TCCIR_OP_ADD, 0, I64, 3, 5);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 8);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_sub_mul_and_or_xor_imm64)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  int i_sub = fold_emit_binimm_bt(&c, TCCIR_OP_SUB, 0, I64, 9, 4);
  int i_mul = fold_emit_binimm_bt(&c, TCCIR_OP_MUL, 1, I64, 3, 4);
  int i_and = fold_emit_binimm_bt(&c, TCCIR_OP_AND, 2, I64, 0xF0, 0x0F);
  int i_or  = fold_emit_binimm_bt(&c, TCCIR_OP_OR,  3, I64, 0xF0, 0x0F);
  int i_xor = fold_emit_binimm_bt(&c, TCCIR_OP_XOR, 4, I64, 0xFF, 0x0F);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 5);
  UT_ASSERT_EQ(utb_src1(c.ir, i_sub).u.imm32, 5);
  UT_ASSERT_EQ(utb_src1(c.ir, i_mul).u.imm32, 12);
  UT_ASSERT_EQ(utb_src1(c.ir, i_and).u.imm32, 0);
  UT_ASSERT_EQ(utb_src1(c.ir, i_or).u.imm32, 0xFF);
  UT_ASSERT_EQ(utb_src1(c.ir, i_xor).u.imm32, 0xF0);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_shifts_imm64)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  int i_shl = fold_emit_binimm_bt(&c, TCCIR_OP_SHL, 0, I64, 1, 8);
  int i_shr = fold_emit_binimm_bt(&c, TCCIR_OP_SHR, 1, I64, 0x100, 4);
  int i_sar = fold_emit_binimm_bt(&c, TCCIR_OP_SAR, 2, I64, -16, 2);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 3);
  UT_ASSERT_EQ(utb_src1(c.ir, i_shl).u.imm32, 0x100);
  UT_ASSERT_EQ(utb_src1(c.ir, i_shr).u.imm32, 0x10);
  UT_ASSERT_EQ(utb_src1(c.ir, i_sar).u.imm32, -4);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_div64_imm)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  int i_div  = fold_emit_binimm_bt(&c, TCCIR_OP_DIV,  0, I64, 20, 5);
  int i_udiv = fold_emit_binimm_bt(&c, TCCIR_OP_UDIV, 1, I64, 20, 5);
  int i_imod = fold_emit_binimm_bt(&c, TCCIR_OP_IMOD, 2, I64, 20, 6);
  int i_umod = fold_emit_binimm_bt(&c, TCCIR_OP_UMOD, 3, I64, 20, 6);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 4);
  UT_ASSERT_EQ(utb_src1(c.ir, i_div).u.imm32, 4);
  UT_ASSERT_EQ(utb_src1(c.ir, i_udiv).u.imm32, 4);
  UT_ASSERT_EQ(utb_src1(c.ir, i_imod).u.imm32, 2);
  UT_ASSERT_EQ(utb_src1(c.ir, i_umod).u.imm32, 2);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_shifts_overflow_imm64)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  int i_shl = fold_emit_binimm_bt(&c, TCCIR_OP_SHL, 0, I64, 1, 64);
  int i_shr = fold_emit_binimm_bt(&c, TCCIR_OP_SHR, 1, I64, 1, 64);
  int i_sar = fold_emit_binimm_bt(&c, TCCIR_OP_SAR, 2, I64, -1, 64);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 3);
  UT_ASSERT_EQ(utb_src1(c.ir, i_shl).u.imm32, 0);
  UT_ASSERT_EQ(utb_src1(c.ir, i_shr).u.imm32, 0);
  UT_ASSERT_EQ(utb_src1(c.ir, i_sar).u.imm32, -1);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_ror_imm64_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm_bt(&c, TCCIR_OP_ROR, 0, I64, 1, 4);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ROR);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_mul_imm64_widening_result)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  utb_pools_init(c.ir);
  int i = fold_emit_binimm_bt(&c, TCCIR_OP_MUL, 0, I64, 0x10000, 0x10000);
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ASSIGN);
  {
    IROperand r = utb_src1(c.ir, i);
    UT_ASSERT(irop_get_tag(r) == IROP_TAG_I64);
    UT_ASSERT_EQ(irop_get_imm64_ex(c.ir, r), 0x100000000LL);
  }
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Algebraic identities with one immediate operand
 * ======================================================================== */

UT_TEST(test_fold_identity_add_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32),
                         utb_temp(0, I32), utb_imm(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_identity_sub_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_SUB, utb_temp(1, I32),
                         utb_temp(0, I32), utb_imm(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_identity_mul_one)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                         utb_temp(0, I32), utb_imm(1, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_identity_or_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_OR, utb_temp(1, I32),
                         utb_temp(0, I32), utb_imm(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_identity_xor_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_XOR, utb_temp(1, I32),
                         utb_temp(0, I32), utb_imm(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_identity_shifts_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i_shl = ssa_add_instr3(&c, TCCIR_OP_SHL, utb_temp(1, I32),
                             utb_temp(0, I32), utb_imm(0, I32));
  int i_shr = ssa_add_instr3(&c, TCCIR_OP_SHR, utb_temp(2, I32),
                             utb_temp(0, I32), utb_imm(0, I32));
  int i_sar = ssa_add_instr3(&c, TCCIR_OP_SAR, utb_temp(3, I32),
                             utb_temp(0, I32), utb_imm(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 3);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i_shl)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i_shr)), IROP_VR(utb_temp(0, I32)));
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i_sar)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_identity_and_minus_one)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_AND, utb_temp(1, I32),
                         utb_temp(0, I32), utb_imm(-1, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_absorb_mul_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                         utb_temp(0, I32), utb_imm(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 0);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_absorb_and_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_AND, utb_temp(1, I32),
                         utb_temp(0, I32), utb_imm(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 0);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_absorb_or_minus_one)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_OR, utb_temp(1, I32),
                         utb_temp(0, I32), utb_imm(-1, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, -1);
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Commutative identities with immediate src1
 * ======================================================================== */

UT_TEST(test_fold_commutative_add_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32),
                         utb_imm(0, I32), utb_temp(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_commutative_or_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_OR, utb_temp(1, I32),
                         utb_imm(0, I32), utb_temp(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_commutative_xor_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_XOR, utb_temp(1, I32),
                         utb_imm(0, I32), utb_temp(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_commutative_mul_one)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                         utb_imm(1, I32), utb_temp(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_commutative_and_minus_one)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_AND, utb_temp(1, I32),
                         utb_imm(-1, I32), utb_temp(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_commutative_absorb_mul_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_MUL, utb_temp(1, I32),
                         utb_imm(0, I32), utb_temp(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 0);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_commutative_absorb_and_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_AND, utb_temp(1, I32),
                         utb_imm(0, I32), utb_temp(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 0);
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * x op x patterns
 * ======================================================================== */

UT_TEST(test_fold_sub_self_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_SUB, utb_temp(1, I32),
                         utb_temp(0, I32), utb_temp(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 0);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_xor_self_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_XOR, utb_temp(1, I32),
                         utb_temp(0, I32), utb_temp(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 0);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_and_self_self)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_AND, utb_temp(1, I32),
                         utb_temp(0, I32), utb_temp(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_or_self_self)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_OR, utb_temp(1, I32),
                         utb_temp(0, I32), utb_temp(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Bit-complement identity: a | (a ^ -1) = -1, a & (a ^ -1) = 0
 * ======================================================================== */

UT_TEST(test_fold_or_compl_minus_one)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr3(&c, TCCIR_OP_XOR, utb_temp(1, I32),
                 utb_temp(0, I32), utb_imm(-1, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_OR, utb_temp(2, I32),
                         utb_temp(0, I32), utb_temp(1, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, -1);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_and_compl_zero)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr3(&c, TCCIR_OP_XOR, utb_temp(1, I32),
                 utb_temp(0, I32), utb_imm(-1, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_AND, utb_temp(2, I32),
                         utb_temp(0, I32), utb_temp(1, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 0);
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Double-negation collapse
 * ======================================================================== */

UT_TEST(test_fold_double_negation)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32));
  ssa_add_instr3(&c, TCCIR_OP_SUB, utb_temp(1, I32),
                 utb_imm(0, I32), utb_temp(0, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_SUB, utb_temp(2, I32),
                         utb_imm(0, I32), utb_temp(1, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(IROP_VR(utb_src1(c.ir, i)), IROP_VR(utb_temp(0, I32)));
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * const-vreg resolution
 * ======================================================================== */

UT_TEST(test_fold_resolves_const_vreg_same_block)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32),
                         utb_temp(0, I32), utb_imm(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 1);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_src1(c.ir, i).u.imm32, 5);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_no_resolve_const_vreg_cross_block)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/2, /*temps=*/3);
  ssa_ctx_init_manual(&c);
  ssa_ctx_manual_block_range(&c, 0, 0, 1);
  ssa_ctx_manual_block_range(&c, 1, 1, 2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32),
                         utb_temp(0, I32), utb_imm(0, I32));
  c.ctx = tcc_mallocz(sizeof(*c.ctx));
  tcc_ir_ssa_opt_init(c.ctx, c.ir, c.ssa, c.cfg);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ADD);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_no_resolve_const_vreg_non_assign_def)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32),
                 utb_temp(0, I32), utb_imm(1, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(2, I32),
                         utb_temp(1, I32), utb_imm(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ADD);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_no_resolve_const_vreg_multi_def)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(5, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(6, I32));
  int i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32),
                         utb_temp(0, I32), utb_imm(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ADD);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_no_resolve_lval_const)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  IROperand imm = utb_imm(5, I32);
  imm.is_lval = 1;
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), imm);
  int i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_temp(1, I32),
                         utb_temp(0, I32), utb_imm(0, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ADD);
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Structural guards
 * ======================================================================== */

UT_TEST(test_fold_barrel_shift_annotation_blocks_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = fold_emit_binimm(&c, TCCIR_OP_ADD, 0, 3, 5);
  uint8_t *bs = tcc_mallocz((size_t)(c.ir->max_orig_index + 1));
  c.ir->barrel_shifts = bs;
  c.ir->barrel_shifts[i] = 1;
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ADD);
  c.ir->barrel_shifts = NULL;
  tcc_free(bs);
  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_fold_non_temp_dest_no_fold)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/2);
  int i = ssa_add_instr3(&c, TCCIR_OP_ADD, utb_var(0, I32),
                         utb_imm(3, I32), utb_imm(5, I32));
  fold_build_rebuild(&c);
  UT_ASSERT_EQ(ssa_opt_fold(c.ctx), 0);
  UT_ASSERT_EQ(utb_op(c.ir, i), TCCIR_OP_ADD);
  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_SUITE(ssa_opt_fold)
{
  UT_COVERS("ssa:fold");
  UT_RUN(test_fold_add_zero);
  UT_RUN(test_fold_mul_one);
  UT_RUN(test_fold_mul_zero);
  UT_RUN(test_fold_non_immediate);
  UT_RUN(test_fold_add_imm);
  UT_RUN(test_fold_sub_imm);
  UT_RUN(test_fold_mul_imm);
  UT_RUN(test_fold_and_or_xor_imm);
  UT_RUN(test_fold_shl_imm);
  UT_RUN(test_fold_shr_imm);
  UT_RUN(test_fold_sar_imm);
  UT_RUN(test_fold_ror_imm);
  UT_RUN(test_fold_div_udiv_imod_umod_imm);
  UT_RUN(test_fold_div_by_zero_no_fold);
  UT_RUN(test_fold_udiv_by_zero_no_fold);
  UT_RUN(test_fold_imod_by_zero_no_fold);
  UT_RUN(test_fold_umod_by_zero_no_fold);
  UT_RUN(test_fold_int_min_div_minus_one_no_fold);
  UT_RUN(test_fold_shifts_overflow_imm);
  UT_RUN(test_fold_add_imm64);
  UT_RUN(test_fold_sub_mul_and_or_xor_imm64);
  UT_RUN(test_fold_shifts_imm64);
  UT_RUN(test_fold_div64_imm);
  UT_RUN(test_fold_shifts_overflow_imm64);
  UT_RUN(test_fold_ror_imm64_no_fold);
  UT_RUN(test_fold_mul_imm64_widening_result);
  UT_RUN(test_fold_identity_add_zero);
  UT_RUN(test_fold_identity_sub_zero);
  UT_RUN(test_fold_identity_mul_one);
  UT_RUN(test_fold_identity_or_zero);
  UT_RUN(test_fold_identity_xor_zero);
  UT_RUN(test_fold_identity_shifts_zero);
  UT_RUN(test_fold_identity_and_minus_one);
  UT_RUN(test_fold_absorb_mul_zero);
  UT_RUN(test_fold_absorb_and_zero);
  UT_RUN(test_fold_absorb_or_minus_one);
  UT_RUN(test_fold_commutative_add_zero);
  UT_RUN(test_fold_commutative_or_zero);
  UT_RUN(test_fold_commutative_xor_zero);
  UT_RUN(test_fold_commutative_mul_one);
  UT_RUN(test_fold_commutative_and_minus_one);
  UT_RUN(test_fold_commutative_absorb_mul_zero);
  UT_RUN(test_fold_commutative_absorb_and_zero);
  UT_RUN(test_fold_sub_self_zero);
  UT_RUN(test_fold_xor_self_zero);
  UT_RUN(test_fold_and_self_self);
  UT_RUN(test_fold_or_self_self);
  UT_RUN(test_fold_or_compl_minus_one);
  UT_RUN(test_fold_and_compl_zero);
  UT_RUN(test_fold_double_negation);
  UT_RUN(test_fold_resolves_const_vreg_same_block);
  UT_RUN(test_fold_no_resolve_const_vreg_cross_block);
  UT_RUN(test_fold_no_resolve_const_vreg_non_assign_def);
  UT_RUN(test_fold_no_resolve_const_vreg_multi_def);
  UT_RUN(test_fold_no_resolve_lval_const);
  UT_RUN(test_fold_barrel_shift_annotation_blocks_fold);
  UT_RUN(test_fold_non_temp_dest_no_fold);
}

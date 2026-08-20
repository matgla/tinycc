/*
 *  test_opt_sdiv_pow2.c - suite for opt/flat/scalar/sdiv_pow2.c
 *  (tcc_ir_opt_sdiv_pow2).
 *
 *  Signed `x / 2^n` becomes a bias-and-shift sequence.  Two properties are
 *  worth more than the shape itself: a branch that targeted the divide must
 *  still land on the FIRST instruction of the sequence (it targeted the block's
 *  head, and skipping the bias would read an undefined temporary), and every
 *  divisor the identity does not hold for must be left alone.
 */

#include "ir_build.h"

#include "ut.h"

int tcc_ir_opt_sdiv_pow2(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64
#define TOK_LT 0x9c

static TCCIRState *utb_div_ir(void)
{
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 8;
  return ir;
}

/* dest = src / divisor, as the only instruction. */
static TCCIRState *build_one_div(int32_t divisor, IROperand dividend, int btype)
{
  TCCIRState *ir = utb_div_ir();
  utb_emit(ir, TCCIR_OP_DIV, utb_temp(0, btype), dividend, utb_imm(divisor, btype));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, btype), UTB_NONE);
  return ir;
}

/* x / 2 -> bias = x >>u 31; sum = x + bias; dest = sum >> 1 */
UT_TEST(sdiv_pow2_by_two_expands_to_three_ops)
{
  TCCIRState *ir = build_one_div(2, utb_temp(1, I32), I32);
  UT_ASSERT_EQ(tcc_ir_opt_sdiv_pow2(ir), 1);
  UT_ASSERT_EQ(ir->next_instruction_index, 4);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_SHR);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_SAR);
  /* The divide's destination survives on the final shift, so every downstream
   * reference keeps working without renaming. */
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_dest(ir, &ir->compact_instructions[2])),
               irop_get_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_src2(ir, &ir->compact_instructions[2])), 1);
  utb_free(ir);
  return 0;
}

/* x / 8 needs the arithmetic sign-mask step as well: four ops, outer shift 3. */
UT_TEST(sdiv_pow2_by_eight_expands_to_four_ops)
{
  TCCIRState *ir = build_one_div(8, utb_temp(1, I32), I32);
  UT_ASSERT_EQ(tcc_ir_opt_sdiv_pow2(ir), 1);
  UT_ASSERT_EQ(ir->next_instruction_index, 5);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_SAR);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_SHR);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_SAR);
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_src2(ir, &ir->compact_instructions[0])), 31);
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_src2(ir, &ir->compact_instructions[1])), 29);
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_src2(ir, &ir->compact_instructions[3])), 3);
  utb_free(ir);
  return 0;
}

/* The divide starts a block someone branches to.  The branch must still reach
 * the bias computation, not the final shift -- the 981001-1 miscompile. */
UT_TEST(sdiv_pow2_keeps_a_branch_landing_on_the_first_op)
{
  TCCIRState *ir = utb_div_ir();
  IROperand x = utb_temp(1, I32);
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_imm(TOK_LT, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);                /* 1 */
  utb_emit(ir, TCCIR_OP_DIV, utb_temp(0, I32), x, utb_imm(4, I32));               /* 2 */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);       /* 3 */

  UT_ASSERT_EQ(tcc_ir_opt_sdiv_pow2(ir), 1);
  /* Still index 2, and index 2 is now the sign-mask shift that starts the
   * sequence -- not the final shift it would be had the ops been inserted
   * ahead of the divide. */
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_dest(ir, &ir->compact_instructions[0])), 2);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_SAR);
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_src2(ir, &ir->compact_instructions[2])), 31);
  /* And the trailing RETURNVALUE moved down past the appended ops. */
  UT_ASSERT_EQ(utb_op(ir, 6), TCCIR_OP_RETURNVALUE);
  utb_free(ir);
  return 0;
}

/* A jump that pointed PAST the divide must be shifted by the appended ops. */
UT_TEST(sdiv_pow2_retargets_a_jump_past_the_divide)
{
  TCCIRState *ir = utb_div_ir();
  IROperand x = utb_temp(1, I32);
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(2, I32), utb_imm(TOK_LT, I32), UTB_NONE); /* 0 -> the DIV */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);               /* 1 -> past it */
  utb_emit(ir, TCCIR_OP_DIV, utb_temp(0, I32), x, utb_imm(2, I32));               /* 2 */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);       /* 3 */

  UT_ASSERT_EQ(tcc_ir_opt_sdiv_pow2(ir), 1);
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_dest(ir, &ir->compact_instructions[0])), 2);
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_dest(ir, &ir->compact_instructions[1])), 5);
  UT_ASSERT_EQ(utb_op(ir, 5), TCCIR_OP_RETURNVALUE);
  utb_free(ir);
  return 0;
}

/* 0x80000000 is INT_MIN as a signed immediate -- a divisor of -2^31, not
 * +2^31 -- so it must not be read as a shift of 31. */
UT_TEST(sdiv_pow2_refuses_int_min_divisor)
{
  TCCIRState *ir = build_one_div((int32_t)0x80000000, utb_temp(1, I32), I32);
  UT_ASSERT_EQ(tcc_ir_opt_sdiv_pow2(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_DIV);
  utb_free(ir);
  return 0;
}

UT_TEST(sdiv_pow2_refuses_negative_and_non_power_of_two)
{
  int32_t bad[] = {-2, -4, 3, 6, 10, 1, 0};
  for (unsigned k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
  {
    TCCIRState *ir = build_one_div(bad[k], utb_temp(1, I32), I32);
    UT_ASSERT_EQ(tcc_ir_opt_sdiv_pow2(ir), 0);
    UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_DIV);
    utb_free(ir);
  }
  return 0;
}

/* An unsigned dividend has no sign bit to test; the 64-bit divide is a runtime
 * call, not an SDIV.  Both keep the DIV. */
UT_TEST(sdiv_pow2_refuses_unsigned_and_64bit)
{
  TCCIRState *ir = build_one_div(4, utb_unsigned(utb_temp(1, I32)), I32);
  UT_ASSERT_EQ(tcc_ir_opt_sdiv_pow2(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_DIV);
  utb_free(ir);

  ir = build_one_div(4, utb_temp(1, I64), I64);
  UT_ASSERT_EQ(tcc_ir_opt_sdiv_pow2(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_DIV);
  utb_free(ir);
  return 0;
}

/* The dividend is read three times, so a dereference would turn one load into
 * three -- and a load may not be replayed. */
UT_TEST(sdiv_pow2_refuses_a_dereferenced_dividend)
{
  TCCIRState *ir = build_one_div(4, utb_lval(utb_temp(1, I32)), I32);
  UT_ASSERT_EQ(tcc_ir_opt_sdiv_pow2(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_DIV);
  utb_free(ir);

  ir = build_one_div(4, utb_stackoff(-8, 1, 0, 0, I32), I32);
  UT_ASSERT_EQ(tcc_ir_opt_sdiv_pow2(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_DIV);
  utb_free(ir);
  return 0;
}

/* A constant dividend is fold_const_eval's business: one immediate beats a
 * three-op shift chain. */
UT_TEST(sdiv_pow2_refuses_a_constant_dividend)
{
  TCCIRState *ir = build_one_div(4, utb_imm(100, I32), I32);
  UT_ASSERT_EQ(tcc_ir_opt_sdiv_pow2(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_DIV);
  utb_free(ir);
  return 0;
}

UT_TEST(sdiv_pow2_empty_ir_no_crash)
{
  TCCIRState *ir = utb_div_ir();
  UT_ASSERT_EQ(tcc_ir_opt_sdiv_pow2(ir), 0);
  utb_free(ir);
  return 0;
}

UT_COVERS("sdiv_pow2");

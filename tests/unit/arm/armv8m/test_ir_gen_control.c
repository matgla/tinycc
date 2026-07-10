/*
 *  test_ir_gen_control.c - suite for ir/gen/control.c
 *
 *  Exercises tcc_ir_gen_return_value(): verifies it emits a bare
 *  TCCIR_OP_RETURNVALUE instruction carrying the given value as src1,
 *  with no dest and no src2.
 */

#define USING_GLOBALS
#include "ir.h"

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

UT_TEST(test_return_value_emits_returnvalue_op)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);

  SValue val = sv_var(t0);
  tcc_ir_gen_return_value(ir, &val);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  UT_ASSERT_EQ(ir->compact_instructions[0].op, TCCIR_OP_RETURNVALUE);

  IROperand s1 = tcc_ir_op_get_src1(ir, &ir->compact_instructions[0]);
  UT_ASSERT_EQ(irop_get_vreg(s1), t0);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_return_value_carries_const_operand)
{
  TCCIRState *ir = tcc_ir_alloc();

  SValue val = sv_const(42);
  tcc_ir_gen_return_value(ir, &val);

  UT_ASSERT_EQ(ir->compact_instructions[0].op, TCCIR_OP_RETURNVALUE);
  IROperand s1 = tcc_ir_op_get_src1(ir, &ir->compact_instructions[0]);
  UT_ASSERT_EQ(irop_get_tag(s1), IROP_TAG_IMM32);
  UT_ASSERT_EQ(irop_get_imm32(s1), 42);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_return_value_has_no_dest_no_src2)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);

  SValue val = sv_var(t0);
  tcc_ir_gen_return_value(ir, &val);

  UT_ASSERT(!irop_config[TCCIR_OP_RETURNVALUE].has_dest);
  UT_ASSERT(!irop_config[TCCIR_OP_RETURNVALUE].has_src2);
  /* One operand recorded in the pool: src1 only. */
  UT_ASSERT_EQ(ir->iroperand_pool_count, 1);

  tcc_ir_free(ir);
  return 0;
}

UT_SUITE(ir_gen_control)
{
  UT_RUN(test_return_value_emits_returnvalue_op);
  UT_RUN(test_return_value_carries_const_operand);
  UT_RUN(test_return_value_has_no_dest_no_src2);
}

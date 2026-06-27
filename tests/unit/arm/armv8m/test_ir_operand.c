/*
 *  test_ir_operand.c - suite for tccir_operand.c / tccir_operand.h helpers
 *
 *  Exercises IROperand constructors, decoders, negative-vreg encoding,
 *  pool round-trips, SValue conversion, and btype helpers.
 */

#define USING_GLOBALS
#include "ir.h"

#include "ut.h"

static SValue sv_const_int(int v)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = v;
  sv.type.t = VT_INT;
  return sv;
}

static SValue sv_local(int offset)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_LOCAL | VT_LVAL;
  sv.c.i = offset;
  sv.type.t = VT_INT;
  return sv;
}

/* -------------------------------------------------------------------------- */
/* Constructors and simple decoders                                           */
/* -------------------------------------------------------------------------- */

UT_TEST(test_make_vreg_encodes_type_and_position)
{
  int vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 42);
  IROperand op = irop_make_vreg(vreg, IROP_BTYPE_INT32);

  UT_ASSERT_EQ(irop_get_vreg(op), vreg);
  UT_ASSERT_EQ(irop_get_tag(op), IROP_TAG_VREG);
  UT_ASSERT_EQ(irop_get_btype(op), IROP_BTYPE_INT32);
  UT_ASSERT(!irop_is_none(op));
  UT_ASSERT(irop_has_vreg(op));
  return 0;
}

UT_TEST(test_make_imm32_roundtrips)
{
  IROperand op = irop_make_imm32(0, -12345, IROP_BTYPE_INT16);
  UT_ASSERT_EQ(irop_get_tag(op), IROP_TAG_IMM32);
  UT_ASSERT_EQ(irop_get_imm32(op), -12345);
  UT_ASSERT_EQ(irop_get_btype(op), IROP_BTYPE_INT16);
  UT_ASSERT(irop_is_immediate(op));
  return 0;
}

UT_TEST(test_make_stackoff_roundtrips)
{
  int vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 5);
  IROperand op = irop_make_stackoff(vreg, -28, 1, 0, 1, IROP_BTYPE_INT32);

  UT_ASSERT_EQ(irop_get_tag(op), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(irop_get_vreg(op), vreg);
  UT_ASSERT_EQ(irop_get_stack_offset(op), -28);
  UT_ASSERT(irop_op_is_lval(op));
  UT_ASSERT(irop_op_is_local(op));
  UT_ASSERT(!irop_op_is_llocal(op));
  UT_ASSERT(op.is_param);
  return 0;
}

UT_TEST(test_make_none)
{
  IROperand op = irop_make_none();
  UT_ASSERT(irop_is_none(op));
  UT_ASSERT(irop_has_no_vreg(op));
  UT_ASSERT(!irop_has_vreg(op));
  UT_ASSERT_EQ(irop_get_tag(op), IROP_TAG_NONE);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Negative vreg encoding                                                     */
/* -------------------------------------------------------------------------- */

UT_TEST(test_negative_vreg_encoding)
{
  IROperand op = {0};
  irop_set_vreg(&op, -1);
  UT_ASSERT_EQ(irop_get_vreg(op), -1);

  irop_set_vreg(&op, -2);
  UT_ASSERT_EQ(irop_get_vreg(op), -2);

  irop_set_vreg(&op, -16);
  UT_ASSERT_EQ(irop_get_vreg(op), -16);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Type predicates                                                            */
/* -------------------------------------------------------------------------- */

UT_TEST(test_btype_predicates)
{
  IROperand i32 = irop_make_vreg(0, IROP_BTYPE_INT32);
  IROperand i64 = irop_make_vreg(0, IROP_BTYPE_INT64);
  IROperand f64 = irop_make_vreg(0, IROP_BTYPE_FLOAT64);

  UT_ASSERT(!irop_is_64bit(i32));
  UT_ASSERT(irop_is_64bit(i64));
  UT_ASSERT(irop_is_64bit(f64));

  UT_ASSERT(!irop_needs_pair(i32));
  UT_ASSERT(irop_needs_pair(i64));
  UT_ASSERT(irop_needs_pair(f64));
  return 0;
}

UT_TEST(test_btype_to_vt_btype)
{
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_INT8), VT_BYTE);
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_INT16), VT_SHORT);
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_INT64), VT_LLONG);
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_FLOAT32), VT_FLOAT);
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_FLOAT64), VT_DOUBLE);
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_STRUCT), VT_STRUCT);
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_INT32), VT_INT);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Pool round-trips                                                           */
/* -------------------------------------------------------------------------- */

UT_TEST(test_i64_pool_roundtrip)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_state->ir = ir;

  int64_t val = (int64_t)0x123456789ABCDEF0LL;
  uint32_t idx = tcc_ir_pool_add_i64(ir, val);
  IROperand op = irop_make_i64(0, idx, IROP_BTYPE_INT64);
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, op), val);

  tcc_ir_free(ir);
  tcc_state->ir = NULL;
  return 0;
}

UT_TEST(test_f64_pool_roundtrip)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_state->ir = ir;

  uint64_t bits = 0x400921FB54442D18ULL; /* pi as double bits */
  uint32_t idx = tcc_ir_pool_add_f64(ir, bits);
  IROperand op = irop_make_f64(0, idx);
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, op), (int64_t)bits);

  tcc_ir_free(ir);
  tcc_state->ir = NULL;
  return 0;
}

/* -------------------------------------------------------------------------- */
/* SValue <-> IROperand conversion                                            */
/* -------------------------------------------------------------------------- */

UT_TEST(test_svalue_to_iroperand_const)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_state->ir = ir;

  SValue sv = sv_const_int(1234);
  IROperand op = svalue_to_iroperand(ir, &sv);

  UT_ASSERT_EQ(irop_get_tag(op), IROP_TAG_IMM32);
  UT_ASSERT_EQ(irop_get_imm32(op), 1234);
  UT_ASSERT_EQ(irop_get_btype(op), IROP_BTYPE_INT32);

  tcc_ir_free(ir);
  tcc_state->ir = NULL;
  return 0;
}

UT_TEST(test_svalue_to_iroperand_local)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_state->ir = ir;

  SValue sv = sv_local(-32);
  IROperand op = svalue_to_iroperand(ir, &sv);

  UT_ASSERT_EQ(irop_get_tag(op), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(irop_get_stack_offset(op), -32);
  UT_ASSERT(irop_op_is_lval(op));
  UT_ASSERT(irop_op_is_local(op));

  tcc_ir_free(ir);
  tcc_state->ir = NULL;
  return 0;
}

UT_TEST(test_iroperand_to_svalue_const)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_state->ir = ir;

  IROperand op = irop_make_imm32(0, 555, IROP_BTYPE_INT32);
  SValue sv;
  iroperand_to_svalue(ir, op, &sv);

  UT_ASSERT_EQ(sv.r & VT_VALMASK, VT_CONST);
  UT_ASSERT_EQ((int)sv.c.i, 555);

  tcc_ir_free(ir);
  tcc_state->ir = NULL;
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Type size helpers                                                          */
/* -------------------------------------------------------------------------- */

UT_TEST(test_type_size_int32)
{
  IROperand op = irop_make_vreg(0, IROP_BTYPE_INT32);
  UT_ASSERT_EQ(irop_type_size(op), 4);

  int align;
  UT_ASSERT_EQ(irop_type_size_align(op, &align), 4);
  UT_ASSERT_EQ(align, 4);
  return 0;
}

UT_TEST(test_type_size_int64)
{
  IROperand op = irop_make_vreg(0, IROP_BTYPE_INT64);
  UT_ASSERT_EQ(irop_type_size(op), 8);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* SValue comparison                                                          */
/* -------------------------------------------------------------------------- */

UT_TEST(test_compare_svalue_matches_const)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_state->ir = ir;

  SValue sv = sv_const_int(999);
  IROperand op = svalue_to_iroperand(ir, &sv);
  UT_ASSERT_EQ(irop_compare_svalue(ir, &sv, op, "test"), 0);

  tcc_ir_free(ir);
  tcc_state->ir = NULL;
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(ir_operand)
{
  UT_RUN(test_make_vreg_encodes_type_and_position);
  UT_RUN(test_make_imm32_roundtrips);
  UT_RUN(test_make_stackoff_roundtrips);
  UT_RUN(test_make_none);
  UT_RUN(test_negative_vreg_encoding);
  UT_RUN(test_btype_predicates);
  UT_RUN(test_btype_to_vt_btype);
  UT_RUN(test_i64_pool_roundtrip);
  UT_RUN(test_f64_pool_roundtrip);
  UT_RUN(test_svalue_to_iroperand_const);
  UT_RUN(test_svalue_to_iroperand_local);
  UT_RUN(test_iroperand_to_svalue_const);
  UT_RUN(test_type_size_int32);
  UT_RUN(test_type_size_int64);
  UT_RUN(test_compare_svalue_matches_const);
}

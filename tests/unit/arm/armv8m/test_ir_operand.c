/*
 *  test_ir_operand.c - unit tests for IROperand construction/decoding helpers
 *
 *  Phase 2 of the tinycc unit-test framework. Exercises the inline makers
 *  and readers in tccir_operand.h plus the non-inline pool/SValue helpers in
 *  tccir_operand.c.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ut.h"

static TCCIRState *ut_operand_alloc_ir(void)
{
  TCCIRState *ir = tcc_ir_alloc();
  tcc_state->ir = ir;
  return ir;
}

static void ut_operand_free_ir(TCCIRState *ir)
{
  tcc_ir_free(ir);
  tcc_state->ir = NULL;
}

/* -------------------------------------------------------------------------- */

UT_TEST(test_make_vreg_encodes_type_and_position)
{
  int32_t vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 5);
  IROperand op = irop_make_vreg(vreg, IROP_BTYPE_INT32);

  UT_ASSERT_EQ(irop_get_tag(op), IROP_TAG_VREG);
  UT_ASSERT_EQ(irop_get_btype(op), IROP_BTYPE_INT32);
  UT_ASSERT_EQ(irop_get_vreg(op), vreg);
  UT_ASSERT(irop_has_vreg(op));
  UT_ASSERT(!irop_is_none(op));
  UT_ASSERT(!irop_is_immediate(op));
  return 0;
}

UT_TEST(test_make_imm32_roundtrips)
{
  TCCIRState *ir = ut_operand_alloc_ir();
  IROperand op = irop_make_imm32(0, -123456789, IROP_BTYPE_INT32);

  UT_ASSERT_EQ(irop_get_tag(op), IROP_TAG_IMM32);
  UT_ASSERT_EQ(irop_get_imm32(op), -123456789);
  UT_ASSERT_EQ(irop_get_btype(op), IROP_BTYPE_INT32);
  UT_ASSERT(irop_is_immediate(op));
  UT_ASSERT(irop_op_is_const(op));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, op), (int64_t)-123456789);

  ut_operand_free_ir(ir);
  return 0;
}

UT_TEST(test_make_stackoff_roundtrips)
{
  IROperand op = irop_make_stackoff(0, -16, 1, 1, 1, IROP_BTYPE_INT32);

  UT_ASSERT_EQ(irop_get_tag(op), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(irop_get_stack_offset(op), -16);
  UT_ASSERT(irop_op_is_lval(op));
  UT_ASSERT(irop_op_is_local(op));
  UT_ASSERT(irop_op_is_llocal(op));
  UT_ASSERT(op.is_param);
  UT_ASSERT_EQ(irop_get_btype(op), IROP_BTYPE_INT32);
  return 0;
}

UT_TEST(test_make_none)
{
  IROperand op = irop_make_none();

  UT_ASSERT(irop_is_none(op));
  UT_ASSERT(irop_has_no_vreg(op));
  UT_ASSERT_EQ(irop_get_vreg(op), -1);
  UT_ASSERT(!irop_has_vreg(op));
  UT_ASSERT_EQ(irop_get_tag(op), IROP_TAG_NONE);
  return 0;
}

UT_TEST(test_negative_vreg_encoding)
{
  IROperand op1 = irop_make_vreg(-1, IROP_BTYPE_INT32);
  IROperand op2 = irop_make_vreg(-2, IROP_BTYPE_INT32);

  UT_ASSERT(irop_is_neg_vreg(op1));
  UT_ASSERT(irop_is_neg_vreg(op2));
  UT_ASSERT_EQ(irop_get_vreg(op1), -1);
  UT_ASSERT_EQ(irop_get_vreg(op2), -2);
  UT_ASSERT(!irop_has_vreg(op1)); /* -1 means "no vreg" */
  UT_ASSERT(irop_has_vreg(op2));  /* -2 is a real temp local */
  return 0;
}

UT_TEST(test_i64_pool_roundtrip)
{
  TCCIRState *ir = ut_operand_alloc_ir();
  int64_t value = 0x123456789ABCDEF0LL;
  uint32_t idx = tcc_ir_pool_add_i64(ir, value);
  IROperand op = irop_make_i64(0, idx, IROP_BTYPE_INT64);

  UT_ASSERT_EQ(irop_get_tag(op), IROP_TAG_I64);
  UT_ASSERT_EQ(irop_get_pool_idx(op), idx);
  UT_ASSERT(irop_is_64bit(op));
  UT_ASSERT(irop_needs_pair(op));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, op), value);

  ut_operand_free_ir(ir);
  return 0;
}

UT_TEST(test_f64_pool_roundtrip)
{
  TCCIRState *ir = ut_operand_alloc_ir();
  uint64_t bits = 0x3FF0000000000000ULL; /* 1.0 */
  uint32_t idx = tcc_ir_pool_add_f64(ir, bits);
  IROperand op = irop_make_f64(0, idx);

  UT_ASSERT_EQ(irop_get_tag(op), IROP_TAG_F64);
  UT_ASSERT_EQ(irop_get_pool_idx(op), idx);
  UT_ASSERT(irop_is_64bit(op));
  UT_ASSERT_EQ((uint64_t)irop_get_imm64_ex(ir, op), bits);

  ut_operand_free_ir(ir);
  return 0;
}

UT_TEST(test_btype_to_vt_btype)
{
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_INT8), VT_BYTE);
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_INT16), VT_SHORT);
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_INT32), VT_INT);
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_INT64), VT_LLONG);
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_FLOAT32), VT_FLOAT);
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_FLOAT64), VT_DOUBLE);
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_STRUCT), VT_STRUCT);
  UT_ASSERT_EQ(irop_btype_to_vt_btype(IROP_BTYPE_FUNC), VT_FUNC);
  return 0;
}

UT_TEST(test_svalue_to_iroperand_const)
{
  TCCIRState *ir = ut_operand_alloc_ir();
  SValue sv;
  svalue_init(&sv);
  sv.type.t = VT_INT;
  sv.r = VT_CONST;
  sv.c.i = 12345;
  sv.vr = -1;

  IROperand op = svalue_to_iroperand(ir, &sv);

  UT_ASSERT_EQ(irop_get_tag(op), IROP_TAG_IMM32);
  UT_ASSERT_EQ(irop_get_imm32(op), 12345);
  UT_ASSERT_EQ(irop_get_btype(op), IROP_BTYPE_INT32);
  UT_ASSERT(irop_op_is_const(op));
  UT_ASSERT_EQ(irop_compare_svalue(ir, &sv, op, "const"), 0);

  ut_operand_free_ir(ir);
  return 0;
}

UT_TEST(test_svalue_to_iroperand_local)
{
  TCCIRState *ir = ut_operand_alloc_ir();
  SValue sv;
  svalue_init(&sv);
  sv.type.t = VT_INT;
  sv.r = VT_LOCAL;
  sv.c.i = -8;
  sv.vr = -1;

  IROperand op = svalue_to_iroperand(ir, &sv);

  UT_ASSERT_EQ(irop_get_tag(op), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(irop_get_stack_offset(op), -8);
  UT_ASSERT(irop_op_is_local(op));
  UT_ASSERT(!irop_op_is_llocal(op));
  UT_ASSERT_EQ(irop_get_btype(op), IROP_BTYPE_INT32);
  UT_ASSERT_EQ(irop_compare_svalue(ir, &sv, op, "local"), 0);

  ut_operand_free_ir(ir);
  return 0;
}

UT_TEST(test_iroperand_to_svalue_const)
{
  TCCIRState *ir = ut_operand_alloc_ir();
  IROperand op = irop_make_imm32(-1, 42, IROP_BTYPE_INT32);
  SValue out;
  iroperand_to_svalue(ir, op, &out);

  UT_ASSERT_EQ(out.r, VT_CONST);
  UT_ASSERT_EQ(out.c.i, 42);
  UT_ASSERT_EQ(out.type.t & VT_BTYPE, VT_INT);
  UT_ASSERT_EQ(out.vr, -1);

  ut_operand_free_ir(ir);
  return 0;
}

UT_TEST(test_type_size_int32)
{
  IROperand op = irop_make_imm32(0, 0, IROP_BTYPE_INT32);
  int align = 0;

  UT_ASSERT_EQ(irop_type_size(op), 4);
  UT_ASSERT_EQ(irop_type_size_align(op, &align), 4);
  UT_ASSERT_EQ(align, 4);
  return 0;
}

UT_TEST(test_compare_svalue_matches)
{
  TCCIRState *ir = ut_operand_alloc_ir();
  SValue sv;
  svalue_init(&sv);
  sv.type.t = VT_INT;
  sv.r = VT_CONST;
  sv.c.i = 77;
  sv.vr = -1;

  IROperand op = irop_make_imm32(-1, 77, IROP_BTYPE_INT32);

  UT_ASSERT_EQ(irop_compare_svalue(ir, &sv, op, "match"), 0);

  ut_operand_free_ir(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */

UT_SUITE(ir_operand)
{
  UT_RUN(test_make_vreg_encodes_type_and_position);
  UT_RUN(test_make_imm32_roundtrips);
  UT_RUN(test_make_stackoff_roundtrips);
  UT_RUN(test_make_none);
  UT_RUN(test_negative_vreg_encoding);
  UT_RUN(test_i64_pool_roundtrip);
  UT_RUN(test_f64_pool_roundtrip);
  UT_RUN(test_btype_to_vt_btype);
  UT_RUN(test_svalue_to_iroperand_const);
  UT_RUN(test_svalue_to_iroperand_local);
  UT_RUN(test_iroperand_to_svalue_const);
  UT_RUN(test_type_size_int32);
  UT_RUN(test_compare_svalue_matches);
}

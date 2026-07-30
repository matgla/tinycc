/*
 *  test_ir_gen_vla.c - suite for ir/gen/vla.c
 *
 *  Exercises the three VLA IR-emission wrappers:
 *    tcc_ir_gen_vla_alloc()      -> TCCIR_OP_VLA_ALLOC   (src1=size, src2=align)
 *    tcc_ir_gen_vla_sp_save()    -> TCCIR_OP_VLA_SP_SAVE  (dest=stack slot)
 *    tcc_ir_gen_vla_sp_restore() -> TCCIR_OP_VLA_SP_RESTORE (src1=stack slot)
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

UT_TEST(test_vla_alloc_emits_op_with_size_and_align)
{
  TCCIRState *ir = tcc_ir_alloc();
  int t0 = tcc_ir_vreg_alloc_temp(ir);

  SValue size = sv_var(t0);
  tcc_ir_gen_vla_alloc(ir, &size, 8);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  UT_ASSERT_EQ(ir->compact_instructions[0].op, TCCIR_OP_VLA_ALLOC);

  IROperand s1 = tcc_ir_op_get_src1(ir, &ir->compact_instructions[0]);
  UT_ASSERT_EQ(irop_get_vreg(s1), t0);

  IROperand s2 = tcc_ir_op_get_src2(ir, &ir->compact_instructions[0]);
  UT_ASSERT_EQ(irop_get_tag(s2), IROP_TAG_IMM32);
  UT_ASSERT_EQ(irop_get_imm32(s2), 8);

  UT_ASSERT(!irop_config[TCCIR_OP_VLA_ALLOC].has_dest);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_vla_alloc_honors_requested_alignment)
{
  TCCIRState *ir = tcc_ir_alloc();

  SValue size = sv_const(64);
  tcc_ir_gen_vla_alloc(ir, &size, 16);

  IROperand s1 = tcc_ir_op_get_src1(ir, &ir->compact_instructions[0]);
  UT_ASSERT_EQ(irop_get_imm32(s1), 64);

  IROperand s2 = tcc_ir_op_get_src2(ir, &ir->compact_instructions[0]);
  UT_ASSERT_EQ(irop_get_imm32(s2), 16);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_vla_sp_save_emits_op_with_stack_dest)
{
  TCCIRState *ir = tcc_ir_alloc();

  tcc_ir_gen_vla_sp_save(ir, -8);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  UT_ASSERT_EQ(ir->compact_instructions[0].op, TCCIR_OP_VLA_SP_SAVE);

  IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[0]);
  UT_ASSERT_EQ(irop_get_tag(d), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(irop_get_imm32(d), -8);
  UT_ASSERT(d.is_local);

  UT_ASSERT(!irop_config[TCCIR_OP_VLA_SP_SAVE].has_src1);
  UT_ASSERT(!irop_config[TCCIR_OP_VLA_SP_SAVE].has_src2);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_vla_sp_restore_emits_op_with_stack_src)
{
  TCCIRState *ir = tcc_ir_alloc();

  tcc_ir_gen_vla_sp_restore(ir, -16);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  UT_ASSERT_EQ(ir->compact_instructions[0].op, TCCIR_OP_VLA_SP_RESTORE);

  IROperand s1 = tcc_ir_op_get_src1(ir, &ir->compact_instructions[0]);
  UT_ASSERT_EQ(irop_get_tag(s1), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(irop_get_imm32(s1), -16);
  UT_ASSERT(s1.is_local);

  UT_ASSERT(!irop_config[TCCIR_OP_VLA_SP_RESTORE].has_dest);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_vla_save_then_restore_round_trip_same_slot)
{
  TCCIRState *ir = tcc_ir_alloc();

  tcc_ir_gen_vla_sp_save(ir, -24);
  tcc_ir_gen_vla_sp_restore(ir, -24);

  UT_ASSERT_EQ(tcc_ir_count(ir), 2);

  IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[0]);
  IROperand s = tcc_ir_op_get_src1(ir, &ir->compact_instructions[1]);
  UT_ASSERT_EQ(irop_get_imm32(d), irop_get_imm32(s));

  tcc_ir_free(ir);
  return 0;
}

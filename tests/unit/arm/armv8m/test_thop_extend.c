/*
 *  test_thop_extend.c - suite for arch/arm/thumb/thop_extend.c
 *
 *  Tests SXTH, UXTH, SXTB, UXTB (T1 and T2 variants with rotation)
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_extend.h"
#include "source/backend/arch/arm/thumb/thumb.h"
#include "ut.h"

static void setup_armv7m(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m3",
      .feat = (thop_feat){
          .t16 = 1,
          .t32 = 1,
          .it = 1,
          .mod_imm = 1,
          .movw_movt = 1,
          .bfx = 1,
          .clz_rbit = 1,
          .tbb_tbh = 1,
          .cbz = 1,
          .sat = 1,
          .div = 1,
      },
      .is_secure_tz = false,
  };
}

UT_TEST(test_sxth_t1_low_reg)
{
  setup_armv7m();

  thumb_opcode op = th_sxth(0, 1, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xB208);

  op = th_sxth(1, 0, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xB201);

  return 0;
}

UT_TEST(test_sxth_t2_with_rotation)
{
  setup_armv7m();

  thumb_opcode op = th_sxth(R8, R9, (thumb_shift){THUMB_SHIFT_ROR, 0, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa0ff889);

  op = th_sxth(R8, R9, (thumb_shift){THUMB_SHIFT_ROR, 8, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa0ff899);

  op = th_sxth(R9, R8, (thumb_shift){THUMB_SHIFT_ROR, 0, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa0ff988);

  op = th_sxth(R9, R8, (thumb_shift){THUMB_SHIFT_ROR, 8, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa0ff998);

  op = th_sxth(R9, R8, (thumb_shift){THUMB_SHIFT_ROR, 16, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa0ff9a8);

  op = th_sxth(R9, R8, (thumb_shift){THUMB_SHIFT_ROR, 24, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa0ff9b8);

  return 0;
}

UT_TEST(test_uxtb_t1)
{
  setup_armv7m();

  thumb_opcode op = th_uxtb(2, 3, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xB2DA);

  op = th_uxtb(0, 1, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xB2C8);

  return 0;
}

UT_TEST(test_sxtb_t2_with_rotation)
{
  setup_armv7m();

  thumb_opcode op = th_sxtb(R8, R9, (thumb_shift){THUMB_SHIFT_ROR, 0, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa4ff889);

  op = th_sxtb(R8, R9, (thumb_shift){THUMB_SHIFT_ROR, 8, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa4ff899);

  op = th_sxtb(R9, R8, (thumb_shift){THUMB_SHIFT_ROR, 0, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa4ff988);

  return 0;
}

UT_TEST(test_uxth_t2_with_rotation)
{
  setup_armv7m();

  thumb_opcode op = th_uxth(R8, R9, (thumb_shift){THUMB_SHIFT_ROR, 0, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa1ff889);

  op = th_uxth(R8, R9, (thumb_shift){THUMB_SHIFT_ROR, 8, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa1ff899);

  op = th_uxth(R9, R8, (thumb_shift){THUMB_SHIFT_ROR, 0, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa1ff988);

  return 0;
}

UT_TEST(test_uxtb_t2_with_rotation)
{
  setup_armv7m();

  thumb_opcode op = th_uxtb(R8, R9, (thumb_shift){THUMB_SHIFT_ROR, 0, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa5ff889);

  op = th_uxtb(R8, R9, (thumb_shift){THUMB_SHIFT_ROR, 8, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa5ff899);

  op = th_uxtb(R9, R8, (thumb_shift){THUMB_SHIFT_ROR, 0, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa5ff988);

  return 0;
}

UT_TEST(test_extend_enforce_16bit_with_rotation_fails)
{
  setup_armv7m();

  thumb_opcode op = th_sxtb(0, 1, (thumb_shift){THUMB_SHIFT_ROR, 8, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  op = th_sxth(0, 1, (thumb_shift){THUMB_SHIFT_ROR, 8, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

UT_TEST(test_extend_enforce_32bit_always_t2)
{
  setup_armv7m();

  thumb_opcode op = th_sxth(R0, R1, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_32BIT);
  UT_ASSERT_EQ(op.size, 4);

  op = th_uxth(R0, R1, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_32BIT);
  UT_ASSERT_EQ(op.size, 4);

  op = th_sxtb(R0, R1, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_32BIT);
  UT_ASSERT_EQ(op.size, 4);

  op = th_uxtb(R0, R1, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_32BIT);
  UT_ASSERT_EQ(op.size, 4);

  return 0;
}

UT_TEST(test_extend_high_reg_t1_fails)
{
  setup_armv7m();

  thumb_opcode op = th_sxth(R8, R9, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);

  op = th_uxth(R8, R9, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);

  return 0;
}

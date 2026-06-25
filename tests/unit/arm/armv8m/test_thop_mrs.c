/*
 *  test_thop_mrs.c - suite for arch/arm/thumb/thop_mrs.c
 *  Move to/from special register (MRS, MSR)
 *
 *  All expected opcodes verified against arm-none-eabi-as output.
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_mrs.h"
#include "arch/arm/thumb/thumb.h"

#include "ut.h"

/* ------------------------------------------------------------------ setup */

static void setup_armv8m(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m33",
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
          .ldaex = 1,
      },
      .is_secure_tz = false,
  };
}

/* ───── MRS ───── */

UT_TEST(test_th_mrs_basic)
{
  setup_armv8m();

  /* mrs r0, apsr => 0xF3EF8000 (GCC: f3ef 8000) */
  thumb_opcode op = th_mrs(0, 0x00);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3EF8000);

  return 0;
}

UT_TEST(test_th_mrs_ipsr)
{
  setup_armv8m();

  /* mrs r8, ipsr => 0xF3EF8805 (GCC: f3ef 8805) */
  thumb_opcode op = th_mrs(8, 0x05);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3EF8805);

  return 0;
}

UT_TEST(test_th_mrs_primask)
{
  setup_armv8m();

  /* mrs r0, primask => 0xF3EF8010 (GCC: f3ef 8010) */
  thumb_opcode op = th_mrs(0, 0x10);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3EF8010);

  return 0;
}

UT_TEST(test_th_mrs_control)
{
  setup_armv8m();

  /* mrs r8, control => 0xF3EF8814 (GCC: f3ef 8814) */
  thumb_opcode op = th_mrs(8, 0x14);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3EF8814);

  return 0;
}

/* ───── MSR ───── */

UT_TEST(test_th_msr_basic)
{
  setup_armv8m();

  /* msr apsr_nzcvq, r1 => 0xF3818800 (GCC: f381 8800) */
  thumb_opcode op = th_msr(0x00, 1, 0x2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3818800);

  return 0;
}

UT_TEST(test_th_msr_control)
{
  setup_armv8m();

  /* msr control, r2 => 0xF3828814 (GCC: f382 8814) */
  thumb_opcode op = th_msr(0x14, 2, 0x2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3828814);

  return 0;
}

UT_TEST(test_th_msr_primask)
{
  setup_armv8m();

  /* msr primask, r1 => 0xF3818810 (GCC: f381 8810) */
  thumb_opcode op = th_msr(0x10, 1, 0x2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3818810);

  return 0;
}

UT_SUITE(thop_mrs)
{
  UT_RUN(test_th_mrs_basic);
  UT_RUN(test_th_mrs_ipsr);
  UT_RUN(test_th_mrs_primask);
  UT_RUN(test_th_mrs_control);
  UT_RUN(test_th_msr_basic);
  UT_RUN(test_th_msr_control);
  UT_RUN(test_th_msr_primask);
}
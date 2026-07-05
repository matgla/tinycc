/*
 *  test_thop_dsp.c - suite for arch/arm/thumb/thop_dsp.c
 *
 *  Tests DSP/SIMD encodings available on ARMv7E-M / ARMv8-M:
 *  UADD8, USUB8, SEL, PKHBT (with LSL/ASR shifts).
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_dsp.h"
#include "arch/arm/thumb/thumb.h"

#include "ut.h"

/* ------------------------------------------------------------------ setup */

static void setup_armv7em(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m4",
      .feat =
          (thop_feat){
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
              .dsp = 1,
          },
      .is_secure_tz = false,
  };
}

static void setup_no_dsp(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m3",
      .feat =
          (thop_feat){
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
              .dsp = 0,
          },
      .is_secure_tz = false,
  };
}

/* ------------------------------------------------------------------ UADD8 */

UT_TEST(test_uadd8_basic)
{
  setup_armv7em();

  /* uadd8 r0, r1, r2 => base 0xfa80f040 | rd=0<<8 | rn=1<<16 | rm=2
   * = 0xfa81f042 */
  thumb_opcode op = th_uadd8(0, 1, 2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa81f042);

  return 0;
}

UT_TEST(test_uadd8_high_regs)
{
  setup_armv7em();

  /* uadd8 r8, r9, r10 => 0xfa80f040 | 8<<8 | 9<<16 | 10 = 0xfa89f84a */
  thumb_opcode op = th_uadd8(8, 9, 10);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfa89f84a);

  return 0;
}

/* ------------------------------------------------------------------ USUB8 */

UT_TEST(test_usub8_basic)
{
  setup_armv7em();

  /* usub8 r0, r1, r2 => base 0xfac0f040 | rd=0<<8 | rn=1<<16 | rm=2
   * = 0xfac1f042 */
  thumb_opcode op = th_usub8(0, 1, 2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfac1f042);

  return 0;
}

/* ------------------------------------------------------------------ SEL */

UT_TEST(test_sel_basic)
{
  setup_armv7em();

  /* sel r0, r1, r2 => base 0xfaa0f080 | rd=0<<8 | rn=1<<16 | rm=2
   * = 0xfaa1f082 */
  thumb_opcode op = th_sel(0, 1, 2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xfaa1f082);

  return 0;
}

/* ------------------------------------------------------------------ PKHBT */

UT_TEST(test_pkhbt_lsl_basic)
{
  setup_armv7em();

  /* pkhbt r0, r1, r2, lsl #4 => base 0xeac00000 | rd=0<<8 | rn=1<<16 | rm=2
   * shift_n=4 -> imm2=0, imm3=1, tb=0
   * = 0xeac00000 | 0x00010000 | 0x00000002 | 0x00001000 | 0x00000000
   * = 0xeac11002 */
  thumb_shift shift = {THUMB_SHIFT_LSL, 4, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_pkhbt(0, 1, 2, shift);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xeac11002);

  return 0;
}

UT_TEST(test_pkhbt_lsl_imm0)
{
  setup_armv7em();

  /* pkhbt r0, r1, r2 (no shift) => shift_n=0, tb=0
   * = 0xeac10002 */
  thumb_shift shift = {THUMB_SHIFT_LSL, 0, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_pkhbt(0, 1, 2, shift);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xeac10002);

  return 0;
}

UT_TEST(test_pkhbt_asr_basic)
{
  setup_armv7em();

  /* pkhbt r0, r1, r2, asr #8 => tb=1, shift_n=8 -> imm2=0, imm3=2
   * = 0xeac00000 | 0x00010000 | 0x00000002 | 0x00002000 | 0x00000020
   * = 0xeac12022 */
  thumb_shift shift = {THUMB_SHIFT_ASR, 8, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_pkhbt(0, 1, 2, shift);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xeac12022);

  return 0;
}

UT_TEST(test_pkhbt_lsl_max)
{
  setup_armv7em();

  /* pkhbt r0, r1, r2, lsl #31 => shift_n=31 -> imm2=3, imm3=7, tb=0
   * = 0xeac00000 | 0x00010000 | 0x00000002 | 0x00007000 | 0x000000C0
   * = 0xeac170C2 */
  thumb_shift shift = {THUMB_SHIFT_LSL, 31, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_pkhbt(0, 1, 2, shift);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xeac170C2);

  return 0;
}

/* ------------------------------------------------------------------ feature mismatch */

UT_TEST(test_uadd8_no_dsp_fails)
{
  setup_no_dsp();

  thumb_opcode op = th_uadd8(0, 1, 2);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

UT_TEST(test_pkhbt_no_dsp_fails)
{
  setup_no_dsp();

  thumb_shift shift = {THUMB_SHIFT_LSL, 4, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_pkhbt(0, 1, 2, shift);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(thop_dsp)
{
  /* UADD8 / USUB8 / SEL */
  UT_RUN(test_uadd8_basic);
  UT_RUN(test_uadd8_high_regs);
  UT_RUN(test_usub8_basic);
  UT_RUN(test_sel_basic);

  /* PKHBT */
  UT_RUN(test_pkhbt_lsl_basic);
  UT_RUN(test_pkhbt_lsl_imm0);
  UT_RUN(test_pkhbt_asr_basic);
  UT_RUN(test_pkhbt_lsl_max);

  /* Feature mismatch */
  UT_RUN(test_uadd8_no_dsp_fails);
  UT_RUN(test_pkhbt_no_dsp_fails);
}

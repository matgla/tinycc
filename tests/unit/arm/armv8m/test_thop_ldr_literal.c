/*
 *  test_thop_ldr_literal.c - suite for arch/arm/thumb/thop_ldr_literal.c
 *  LDR (literal) PC-relative encoding (ARMv8-M)
 *
 *  All expected opcodes verified against arm-none-eabi-as output.
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_ldr_literal.h"
#include "source/backend/arch/arm/thumb/thumb.h"

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

/* ───── T1: LDR <Rt>, [PC, #<imm8*4>] — 16-bit, low reg only ───── */

UT_TEST(test_ldr_literal_t1_basic)
{
  setup_armv8m();

  /* ldr r0, [pc, #4]  => 0x4801  (GCC: 4801) */
  thumb_opcode op = th_ldr_literal(0, 4, 1);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4801);

  /* ldr r7, [pc, #0x1C]  => 0x4F07  (GCC: 4f07) */
  op = th_ldr_literal(7, 0x1C, 1);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4F07);

  return 0;
}

UT_TEST(test_ldr_literal_t1_max_offset)
{
  setup_armv8m();

  /* ldr r7, [pc, #0x3FC] (max offset for T1, imm8*4 = 0xFF*4 = 0x3FC)  => 0x4FFF  (GCC: 4fff) */
  thumb_opcode op = th_ldr_literal(7, 0x3FC, 1);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4FFF);

  return 0;
}

UT_TEST(test_ldr_literal_t1_high_reg_rejected)
{
  setup_armv8m();

  /* T1 requires low register (r0-r7). High registers fall through to T32.
   * r8 is a high register, so T1 should not match. */
  thumb_opcode op = th_ldr_literal(8, 4, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8DF8004);

  return 0;
}

/* ───── T32: LDR.W <Rt>, [PC, #+/-<imm12>] — 32-bit, rt != PC ───── */

UT_TEST(test_ldr_literal_t32_positive)
{
  setup_armv8m();

  /* ldr.w r8, [pc, #256]  => 0xF8DF8100  (GCC: f8df 8100)
   * base: 0xF85F0000 | add=1 (bit 23=1) | rt=8<<12 | imm=0x100
   *        = 0xF85F0000 | 0x00800000 | 0x00008100 = 0xF8DF8100 */
  thumb_opcode op = th_ldr_literal(8, 0x100, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8DF8100);

  /* ldr.w r0, [pc, #0x400]  => 0xF8DF0400  (GCC: f8df 0400) */
  op = th_ldr_literal(0, 0x400, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8DF0400);

  return 0;
}

UT_TEST(test_ldr_literal_t32_negative)
{
  setup_armv8m();

  /* ldr.w r8, [pc, #-256]  => 0xF85F8100  (GCC: f85f 8100)
   * base: 0xF85F0000 | add=0 (bit 23=0) | rt=8<<12 | imm=0x100
   *        = 0xF85F0000 | 0x00000000 | 0x00008100 = 0xF85F8100 */
  thumb_opcode op = th_ldr_literal(8, 0x100, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF85F8100);

  /* ldr.w r12, [pc, #-0x800]  => 0xF85FC800  (GCC: f85f c800) */
  op = th_ldr_literal(12, 0x800, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF85FC800);

  return 0;
}

UT_TEST(test_ldr_literal_t32_pc_rejected)
{
  setup_armv8m();

  /* rt=PC is rejected in custom emitter - returns size=0 */
  thumb_opcode op = th_ldr_literal(R_PC, 0x100, 1);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

UT_TEST(test_ldr_literal_t32_imm_overflow_rejected)
{
  setup_armv8m();

  /* imm > 0xFFF cannot be encoded in T32 - returns size=0 */
  thumb_opcode op = th_ldr_literal(0, 0x2000, 1);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

UT_TEST(test_ldr_literal_t32_max_imm)
{
  setup_armv8m();

  /* T32 max imm = 0xFFF */
  thumb_opcode op = th_ldr_literal(5, 0xFFF, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8DF5FFF);

  /* ldr.w r5, [pc, #0xFFF]  => 0xF8DF5FFF  (GCC: f8df 5fff) */
  return 0;
}

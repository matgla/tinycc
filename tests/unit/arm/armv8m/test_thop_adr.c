/*
 *  test_thop_adr.c - suite for arch/arm/thumb/thop_adr.c ADR encoding
 *
 *  Tests T1 (16-bit, low reg, imm8*4 positive), T3 (32-bit, any reg,
 *  imm12 positive, IMM_PACK_3_8_1), and T4 (32-bit, any reg, imm12
 *  negative, IMM_PACK_3_8_1 with is_signed).
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_adr.h"
#include "arch/arm/thumb/thumb.h"

#include "ut.h"

/* ------------------------------------------------------------------ setup */

/* Configure arm_target_dependent for ARMv7-M baseline (ADR requires t16+t32) */
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

/* ------------------------------------------------------------------ tests */

UT_TEST(test_adr_imm_t1_low_reg_positive)
{
  setup_armv7m();

  /* T1: rd=0, imm=4 => 0xA000 | (0<<8) | (4>>2) = 0xA000 | 0x0001 = 0xA001 */
  thumb_opcode op = th_adr_imm(0, 4, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xA001);

  /* T1: rd=7 (highest low reg), imm=0 => 0xA000 | (7<<8) | 0 = 0xA700 */
  op = th_adr_imm(7, 0, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xA700);

 /* T1: rd=7, imm=252 (max 8-bit scaled by 4) => 0xA000 | (7<<8) | (252>>2) = 0xA73F */
   op = th_adr_imm(7, 252, ENFORCE_ENCODING_NONE);
   UT_ASSERT_EQ(op.size, 2);
   UT_ASSERT_EQ(op.opcode, 0xA73F);
   return 0;
}

UT_TEST(test_adr_imm_t3_any_reg_positive)
{
  setup_armv7m();

  /* T3: rd=8, imm=0x123 => 0xF20F0000 | (8<<8) | th_packimm_3_8_1(0x123)
   * th_packimm_3_8_1(0x123):
   *   imm8 = 0x123 & 0xff = 0x23
   *   imm3 = (0x123 >> 8) & 7 = 0x1
   *   i    = (0x123 >> 11) & 1 = 0
   *   imm4 = (0x123 >> 12) & 0xf = 0
   *   packed = (0<<26) | (0<<16) | (1<<12) | 0x23 = 0x1023
   * opcode = 0xF20F0000 | (8<<8) | 0x1023 = 0xF20F1823
   */
  thumb_opcode op = th_adr_imm(8, 0x123, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF20F1823);

  /* T3: rd=R15 is not allowed (REG_NOT_PC on T3), should fail */
 op = th_adr_imm(15, 0x123, ENFORCE_ENCODING_NONE);
   UT_ASSERT_EQ(op.size, 0);
   UT_ASSERT_EQ(op.opcode, 0);
   return 0;
}

UT_TEST(test_adr_imm_t4_negative)
{
  setup_armv7m();

  /* T4: rd=8, imm=-0x123 => base 0xF2AF0000 | (8<<8) | th_packimm_3_8_1(0x123)
   * Same packed immediate as T3 positive case
   * opcode = 0xF2AF0000 | (8<<8) | 0x1023 = 0xF2AF1823
   */
  thumb_opcode op = th_adr_imm(8, -0x123, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF2AF1823);

  /* T4: negative immediate with low reg - still T3/T4 because sign bit set */
   op = th_adr_imm(0, -4, ENFORCE_ENCODING_NONE);
   UT_ASSERT_EQ(op.size, 4);
   /* 0xF2AF0000 | 0 | th_packimm_3_8_1(4) = 0xF2AF0000 | 0x0004 = 0xF2AF0004 */
   UT_ASSERT_EQ(op.opcode, 0xF2AF0004);
   return 0;
}

UT_TEST(test_adr_imm_enforce_16bit_high_reg_fails)
{
  setup_armv7m();

 /* enforce 16-bit with high register (R8) - T1 requires low reg, so fails */
   thumb_opcode op = th_adr_imm(R8, 4, ENFORCE_ENCODING_16BIT);
   UT_ASSERT_EQ(op.size, 0);
   UT_ASSERT_EQ(op.opcode, 0);
   return 0;
}

UT_TEST(test_adr_imm_imm_zero)
{
  setup_armv7m();

 /* imm=0 with low reg -> T1 */
   thumb_opcode op = th_adr_imm(3, 0, ENFORCE_ENCODING_NONE);
   UT_ASSERT_EQ(op.size, 2);
   /* 0xA000 | (3<<8) | 0 = 0xA300 */
   UT_ASSERT_EQ(op.opcode, 0xA300);
   return 0;
}

UT_TEST(test_adr_imm_rd_low_reg_only)
{
  setup_armv7m();

/* T1: low reg (r0-r7) with various values */
   for (int i = 0; i <= 7; i++) {
     thumb_opcode op = th_adr_imm(i, 8, ENFORCE_ENCODING_NONE);
     UT_ASSERT_EQ(op.size, 2);
     /* 0xA000 | (i<<8) | (8>>2) = 0xA000 | (i<<8) | 2 */
     UT_ASSERT_EQ(op.opcode, 0xA000 | (i << 8) | 2);
   }
   return 0;
}

UT_TEST(test_adr_imm_variant_selection_t1_preferred)
{
  setup_armv7m();

  /* Same register and positive offset - should prefer T1 over T3 */
  thumb_opcode op_t1 = th_adr_imm(5, 12, ENFORCE_ENCODING_NONE); /* imm=12 is divisible by 4 */
  UT_ASSERT_EQ(op_t1.size, 2); /* T1 should be selected */

/* Negative offset forces T3/T4 even with low reg */
   thumb_opcode op_neg = th_adr_imm(5, -12, ENFORCE_ENCODING_NONE);
   UT_ASSERT_EQ(op_neg.size, 4); /* T4 is selected for negative */
   return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(thop_adr)
{
  UT_RUN(test_adr_imm_t1_low_reg_positive);
  UT_RUN(test_adr_imm_t3_any_reg_positive);
  UT_RUN(test_adr_imm_t4_negative);
  UT_RUN(test_adr_imm_enforce_16bit_high_reg_fails);
  UT_RUN(test_adr_imm_imm_zero);
  UT_RUN(test_adr_imm_rd_low_reg_only);
  UT_RUN(test_adr_imm_variant_selection_t1_preferred);
}

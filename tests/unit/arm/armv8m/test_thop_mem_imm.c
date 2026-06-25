/*
 *  test_thop_mem_imm.c - suite for arch/arm/thumb/thop_mem_imm.c
 *  LDR/STR family with immediate offsets (T16/T32)
 *
 *  All expected opcodes verified against arm-none-eabi-as output.
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_mem_imm.h"
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

/* ───── T16: LDR imm4 (word) ───── */

UT_TEST(test_ldr_imm_t16_basic)
{
  setup_armv8m();

  /* ldr r0, [r1, #4]  => 0x6848  (GCC: 6848) */
  thumb_opcode op = th_ldr_imm(0, 1, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x6848);

  /* ldr r3, [r5, #8]  => 0x68AB  (GCC: 68ab) */
  op = th_ldr_imm(3, 5, 8, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x68AB);

  /* ldr r7, [r0, #0]  => 0x6807  (GCC: 6807) */
  op = th_ldr_imm(7, 0, 0, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x6807);

  return 0;
}

/* ───── T16: STR imm4 (word) ───── */

UT_TEST(test_str_imm_t16_basic)
{
  setup_armv8m();

  /* str r0, [r1, #4]  => 0x6048  (GCC: 6048) */
  thumb_opcode op = th_str_imm(0, 1, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x6048);

  /* str r3, [r5, #8]  => 0x60AB  (GCC: 60ab) */
  op = th_str_imm(3, 5, 8, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x60AB);

  return 0;
}

/* ───── T16: LDRB imm0 (byte) ───── */

UT_TEST(test_ldrb_imm_t16_basic)
{
  setup_armv8m();

  /* ldrb r0, [r1, #4]  => 0x7908  (GCC: 7908) */
  thumb_opcode op = th_ldrb_imm(0, 1, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x7908);

  /* ldrb r3, [r2, #1]  => 0x7853  (GCC: 7853) */
  op = th_ldrb_imm(3, 2, 1, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x7853);

  return 0;
}

/* ───── T16: STRB imm0 (byte) ───── */

UT_TEST(test_strb_imm_t16_basic)
{
  setup_armv8m();

  /* strb r0, [r1, #1]  => 0x7048  (GCC: 7048) */
  thumb_opcode op = th_strb_imm(0, 1, 1, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x7048);

  /* strb r3, [r2, #3]  => 0x70D3  (GCC: 70d3) */
  op = th_strb_imm(3, 2, 3, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x70D3);

  return 0;
}

/* ───── T16: LDRH imm1 (half) ───── */

UT_TEST(test_ldrh_imm_t16_basic)
{
  setup_armv8m();

  /* ldrh r0, [r1, #4]  => 0x8888  (GCC: 8888) */
  thumb_opcode op = th_ldrh_imm(0, 1, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x8888);

  /* ldrh r3, [r2, #6]  => 0x88D3  (GCC: 88d3) */
  op = th_ldrh_imm(3, 2, 6, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x88D3);

  return 0;
}

/* ───── T16: STRH imm1 (half) ───── */

UT_TEST(test_strh_imm_t16_basic)
{
  setup_armv8m();

  /* strh r0, [r1, #4]  => 0x8088  (GCC: 8088) */
  thumb_opcode op = th_strh_imm(0, 1, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x8088);

  /* strh r3, [r2, #6]  => 0x80D3  (GCC: 80d3) */
  op = th_strh_imm(3, 2, 6, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x80D3);

  return 0;
}

/* ───── T16: LDR SP-relative ───── */

UT_TEST(test_ldr_imm_t16_sp_relative)
{
  setup_armv8m();

  /* ldr r0, [sp, #4]  => 0x9801  (GCC: 9801) */
  thumb_opcode op = th_ldr_imm(0, 13, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x9801);

  /* ldr r3, [sp, #8]  => 0x9B02  (GCC: 9b02) */
  op = th_ldr_imm(3, 13, 8, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x9B02);

  return 0;
}

/* ───── T16: STR SP-relative ───── */

UT_TEST(test_str_imm_t16_sp_relative)
{
  setup_armv8m();

  /* str r0, [sp, #4]  => 0x9001  (GCC: 9001) */
  thumb_opcode op = th_str_imm(0, 13, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x9001);

  /* str r3, [sp, #8]  => 0x9302  (GCC: 9302) */
  op = th_str_imm(3, 13, 8, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x9302);

  return 0;
}

/* ───── T16 high-register fails (falls to T32) ───── */

UT_TEST(test_ldr_imm_t16_high_reg_falls_to_t32)
{
  setup_armv8m();

  /* ldr r8, [r1, #4] - r8 is high, so T16 can't be used
   * Falls to T32: ldr.w r8, [r1, #4] => 0xF8D18004 */
  thumb_opcode op = th_ldr_imm(8, 1, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8D18004);

  /* str r8, [r1, #4] - falls to T32: str.w r8, [r1, #4] => 0xF8C18004 */
  op = th_str_imm(8, 1, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8C18004);

  return 0;
}

/* ───── T16 enforce-16bit with high reg fails ───── */

UT_TEST(test_ldr_imm_enforce_16bit_high_reg_fails)
{
  setup_armv8m();

  /* Request T16 but r8 is high -> fails to match any variant */
  thumb_opcode op = th_ldr_imm(8, 1, 4, 6, ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  op = th_str_imm(8, 1, 4, 6, ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

/* ───── T32: LDR positive offset ───── */

UT_TEST(test_ldr_imm_t32_positive)
{
  setup_armv8m();

  /* ldr r8, [r1, #0x100] => 0xF8D18100  (GCC: f8d1 8100) */
  thumb_opcode op = th_ldr_imm(8, 1, 0x100, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8D18100);

  /* ldr.w r0, [r1, #0x80] => 0xF8D10080  (GCC: f8d1 0080) */
  op = th_ldr_imm(0, 1, 0x80, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8D10080);

  return 0;
}

/* ───── T32: STR positive offset ───── */

UT_TEST(test_str_imm_t32_positive)
{
  setup_armv8m();

  /* str r8, [r1, #0x100] => 0xF8C18100  (GCC: f8c1 8100) */
  thumb_opcode op = th_str_imm(8, 1, 0x100, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8C18100);

  /* str.w r0, [r1, #0x80] => 0xF8C10080  (GCC: f8c1 0080) */
  op = th_str_imm(0, 1, 0x80, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8C10080);

  return 0;
}

/* ───── T32: LDRB/STRB positive offset ───── */

UT_TEST(test_ldrb_strb_imm_t32_positive)
{
  setup_armv8m();

  /* ldrb r8, [r1, #4]  => 0xF8918004  (GCC: f891 8004) */
  thumb_opcode op = th_ldrb_imm(8, 1, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8918004);

  /* strb r8, [r1, #4]  => 0xF8818004  (GCC: f881 8004) */
  op = th_strb_imm(8, 1, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8818004);

  /* ldrb r8, [r1, #0x100] => 0xF8918100  (GCC: f891 8100) */
  op = th_ldrb_imm(8, 1, 0x100, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8918100);

  /* strb r8, [r1, #0x100] => 0xF8818100  (GCC: f881 8100) */
  op = th_strb_imm(8, 1, 0x100, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8818100);

  return 0;
}

/* ───── T32: LDRH/STRH positive offset ───── */

UT_TEST(test_ldrh_strh_imm_t32_positive)
{
  setup_armv8m();

  /* ldrh r8, [r1, #4]  => 0xF8B18004  (GCC: f8b1 8004) */
  thumb_opcode op = th_ldrh_imm(8, 1, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8B18004);

  /* strh r8, [r1, #4]  => 0xF8A18004  (GCC: f8a1 8004) */
  op = th_strh_imm(8, 1, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8A18004);

  /* ldrh r8, [r1, #0x100] => 0xF8B18100  (GCC: f8b1 8100) */
  op = th_ldrh_imm(8, 1, 0x100, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8B18100);

  /* strh r8, [r1, #0x100] => 0xF8A18100  (GCC: f8a1 8100) */
  op = th_strh_imm(8, 1, 0x100, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8A18100);

  return 0;
}

/* ───── T32: LDRSB/LDRSH positive offset ───── */

UT_TEST(test_ldrsb_ldrsh_imm_t32_positive)
{
  setup_armv8m();

  /* ldrsb r8, [r1, #4]  => 0xF9918004  (GCC: f991 8004) */
  thumb_opcode op = th_ldrsb_imm(8, 1, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF9918004);

  /* ldrsh r8, [r1, #4]  => 0xF9B18004  (GCC: f9b1 8004) */
  op = th_ldrsh_imm(8, 1, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF9B18004);

  /* ldrsb r8, [r1, #0x100] => 0xF9918100  (GCC: f991 8100) */
  op = th_ldrsb_imm(8, 1, 0x100, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF9918100);

  /* ldrsh r8, [r1, #0x100] => 0xF9B18100  (GCC: f9b1 8100) */
  op = th_ldrsh_imm(8, 1, 0x100, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF9B18100);

  return 0;
}

/* ───── T32: PC-relative positive ───── */

UT_TEST(test_ldr_imm_t32_pc_positive)
{
  setup_armv8m();

  /* ldr r8, [pc, #0x100] => 0xF8DF8100  (GCC: f8df 8100) */
  thumb_opcode op = th_ldr_imm(8, 15, 0x100, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8DF8100);

  return 0;
}

/* ───── T32: PC-relative negative ───── */

UT_TEST(test_ldr_imm_t32_pc_negative)
{
  setup_armv8m();

  /* ldr r8, [pc, #-0x100] => 0xF85F8100  (GCC: f85f 8100) */
  thumb_opcode op = th_ldr_imm(8, 15, 0x100, 4, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF85F8100);

  return 0;
}

/* ───── T32: LDRB/STRB PC-relative ───── */

UT_TEST(test_ldrb_strb_imm_t32_pc)
{
  setup_armv8m();

  /* ldrb r8, [pc, #0x100] => 0xF89F8100  (GCC: f89f 8100) */
  thumb_opcode op = th_ldrb_imm(8, 15, 0x100, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF89F8100);

  /* ldrb r8, [pc, #-0x100] => 0xF81F8100  (GCC: f81f 8100) */
  op = th_ldrb_imm(8, 15, 0x100, 4, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF81F8100);

  return 0;
}

/* ───── T32: LDRH/STRH PC-relative ───── */

UT_TEST(test_ldrh_strh_imm_t32_pc)
{
  setup_armv8m();

  /* ldrh r8, [pc, #0x100] => 0xF8BF8100  (GCC: f8bf 8100) */
  thumb_opcode op = th_ldrh_imm(8, 15, 0x100, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8BF8100);

  /* ldrh r8, [pc, #-0x100] => 0xF83F8100  (GCC: f83f 8100) */
  op = th_ldrh_imm(8, 15, 0x100, 4, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF83F8100);

  return 0;
}

/* ───── T32: LDRSB/STRB PC-relative ───── */

UT_TEST(test_ldrsb_imm_t32_pc)
{
  setup_armv8m();

  /* ldrsb r8, [pc, #0x100] => 0xF99F8100  (GCC: f99f 8100) */
  thumb_opcode op = th_ldrsb_imm(8, 15, 0x100, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF99F8100);

  /* ldrsh r8, [pc, #0x100] => 0xF9BF8100  (GCC: f9bf 8100) */
  op = th_ldrsh_imm(8, 15, 0x100, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF9BF8100);

  return 0;
}

/* ───── T32 indexed: post-increment (PUW=2) ───── */

UT_TEST(test_ldr_imm_t32_post_inc)
{
  setup_armv8m();

  /* ldr r0, [r1], #4 => 0xF8510B04  (GCC: f851 0b04) */
  thumb_opcode op = th_ldr_imm(0, 1, 4, 3, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8510B04);

  /* str r0, [r1], #4 => 0xF8410B04  (GCC: f841 0b04) */
  op = th_str_imm(0, 1, 4, 3, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8410B04);

  return 0;
}

/* ───── T32 indexed: pre-index (PUW=7) ───── */

UT_TEST(test_ldr_imm_t32_pre_index)
{
  setup_armv8m();

  /* ldr r0, [r1, #4]! => 0xF8510F04  (GCC: f851 0f04) */
  thumb_opcode op = th_ldr_imm(0, 1, 4, 7, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8510F04);

  /* str r0, [r1, #4]! => 0xF8410F04  (GCC: f841 0f04) */
  op = th_str_imm(0, 1, 4, 7, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8410F04);

  return 0;
}

/* ───── T32 indexed: negative offset (PUW=4) ───── */

UT_TEST(test_ldr_imm_t32_negative_offset)
{
  setup_armv8m();

  /* ldrb r0, [r1, #-4] => 0xF8110C04  (GCC: f811 0c04) */
  thumb_opcode op = th_ldrb_imm(0, 1, 4, 4, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8110C04);

  /* strb r0, [r1, #-4] => 0xF8010C04  (GCC: f801 0c04) */
  op = th_strb_imm(0, 1, 4, 4, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8010C04);

  /* ldrh r0, [r1, #-4] => 0xF8310C04  (GCC: f831 0c04) */
  op = th_ldrh_imm(0, 1, 4, 4, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8310C04);

  /* strh r0, [r1, #-4] => 0xF8210C04  (GCC: f821 0c04) */
  op = th_strh_imm(0, 1, 4, 4, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8210C04);

  return 0;
}

/* ───── T32 indexed: LDRSB/LDRSH ───── */

UT_TEST(test_ldrsb_ldrsh_imm_t32_indexed)
{
  setup_armv8m();

  /* ldrsb r0, [r1], #4 => 0xF9110B04  (GCC: f911 0b04) */
  thumb_opcode op = th_ldrsb_imm(0, 1, 4, 3, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF9110B04);

  /* ldrsh r0, [r1], #4 => 0xF9310B04  (GCC: f931 0b04) */
  op = th_ldrsh_imm(0, 1, 4, 3, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF9310B04);

  return 0;
}

/* ───── T32: SP as base is not allowed (falls to T32 POS) ───── */

UT_TEST(test_ldr_str_sp_base_t32)
{
  setup_armv8m();

  /* ldr r0, [sp, #4] - SP not allowed in T32 POS, but rt is low so T16 is used */
  /* Actually rt=0 is low, so T16 SP-relative form should be used: 0x9801 */
  thumb_opcode op = th_ldr_imm(0, 13, 4, 6, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x9801);

  return 0;
}

/* ───── suite ───── */

UT_SUITE(thop_mem_imm)
{
  UT_RUN(test_ldr_imm_t16_basic);
  UT_RUN(test_str_imm_t16_basic);
  UT_RUN(test_ldrb_imm_t16_basic);
  UT_RUN(test_strb_imm_t16_basic);
  UT_RUN(test_ldrh_imm_t16_basic);
  UT_RUN(test_strh_imm_t16_basic);
  UT_RUN(test_ldr_imm_t16_sp_relative);
  UT_RUN(test_str_imm_t16_sp_relative);
  UT_RUN(test_ldr_imm_t16_high_reg_falls_to_t32);
  UT_RUN(test_ldr_imm_enforce_16bit_high_reg_fails);
  UT_RUN(test_ldr_imm_t32_positive);
  UT_RUN(test_str_imm_t32_positive);
  UT_RUN(test_ldrb_strb_imm_t32_positive);
  UT_RUN(test_ldrh_strh_imm_t32_positive);
  UT_RUN(test_ldrsb_ldrsh_imm_t32_positive);
  UT_RUN(test_ldr_imm_t32_pc_positive);
  UT_RUN(test_ldr_imm_t32_pc_negative);
  UT_RUN(test_ldrb_strb_imm_t32_pc);
  UT_RUN(test_ldrh_strh_imm_t32_pc);
  UT_RUN(test_ldrsb_imm_t32_pc);
  UT_RUN(test_ldr_imm_t32_post_inc);
  UT_RUN(test_ldr_imm_t32_pre_index);
  UT_RUN(test_ldr_imm_t32_negative_offset);
  UT_RUN(test_ldrsb_ldrsh_imm_t32_indexed);
  UT_RUN(test_ldr_str_sp_base_t32);
}
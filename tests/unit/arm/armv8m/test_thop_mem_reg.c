/*
 *  test_thop_mem_reg.c - suite for arch/arm/thumb/thop_mem_reg.c
 *  LDR/STR register-offset (T16/T32)
 *
 *  All expected opcodes verified against arm-none-eabi-as output.
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_mem_reg.h"
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

/* ───── T16: ldr <Rt>, [<Rn>, <Rm>] ───── */

UT_TEST(test_ldr_reg_t16_basic)
{
  setup_armv8m();

  /* ldr r0, [r1, r2] => 0x5888 (GCC: 5888) */
  thumb_opcode op = th_ldr_reg(0, 1, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x5888);

  /* ldr r3, [r5, r2] => 0x58AB (GCC: 58ab) */
  op = th_ldr_reg(3, 5, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x58AB);

  /* ldr r7, [r0, r7] => 0x59C7 (GCC: 59c7) */
  op = th_ldr_reg(7, 0, 7, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x59C7);

  return 0;
}

/* ───── T16: ldrb <Rt>, [<Rn>, <Rm>] ───── */

UT_TEST(test_ldrb_reg_t16_basic)
{
  setup_armv8m();

  /* ldrb r0, [r1, r2] => 0x5C88 (GCC: 5c88) */
  thumb_opcode op = th_ldrb_reg(0, 1, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x5C88);

  /* ldrb r3, [r5, r2] => 0x5CAB (GCC: 5cab) */
  op = th_ldrb_reg(3, 5, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x5CAB);

  return 0;
}

/* ───── T16: ldrh <Rt>, [<Rn>, <Rm>] ───── */

UT_TEST(test_ldrh_reg_t16_basic)
{
  setup_armv8m();

  /* ldrh r0, [r1, r2] => 0x5A88 (GCC: 5a88) */
  thumb_opcode op = th_ldrh_reg(0, 1, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x5A88);

  /* ldrh r3, [r5, r2] => 0x5AAB (GCC: 5aab) */
  op = th_ldrh_reg(3, 5, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x5AAB);

  return 0;
}

/* ───── T16: ldrsb <Rt>, [<Rn>, <Rm>] ───── */

UT_TEST(test_ldrsb_reg_t16_basic)
{
  setup_armv8m();

  /* ldrsb r0, [r1, r2] => 0x5688 (GCC: 5688) */
  thumb_opcode op = th_ldrsb_reg(0, 1, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x5688);

  /* ldrsb r3, [r5, r2] => 0x56AB (GCC: 56ab) */
  op = th_ldrsb_reg(3, 5, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x56AB);

  return 0;
}

/* ───── T16: ldrsh <Rt>, [<Rn>, <Rm>] ───── */

UT_TEST(test_ldrsh_reg_t16_basic)
{
  setup_armv8m();

  /* ldrsh r0, [r1, r2] => 0x5E88 (GCC: 5e88) */
  thumb_opcode op = th_ldrsh_reg(0, 1, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x5E88);

  /* ldrsh r3, [r5, r2] => 0x5EAB (GCC: 5eab) */
  op = th_ldrsh_reg(3, 5, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x5EAB);

  return 0;
}

/* ───── T16: str <Rt>, [<Rn>, <Rm>] ───── */

UT_TEST(test_str_reg_t16_basic)
{
  setup_armv8m();

  /* str r0, [r1, r2] => 0x5088 (GCC: 5088) */
  thumb_opcode op = th_str_reg(0, 1, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x5088);

  /* str r3, [r5, r2] => 0x50AB (GCC: 50ab) */
  op = th_str_reg(3, 5, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x50AB);

  return 0;
}

/* ───── T16: strb <Rt>, [<Rn>, <Rm>] ───── */

UT_TEST(test_strb_reg_t16_basic)
{
  setup_armv8m();

  /* strb r0, [r1, r2] => 0x5488 (GCC: 5488) */
  thumb_opcode op = th_strb_reg(0, 1, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x5488);

  /* strb r3, [r5, r2] => 0x54AB (GCC: 54ab) */
  op = th_strb_reg(3, 5, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x54AB);

  return 0;
}

/* ───── T16: strh <Rt>, [<Rn>, <Rm>] ───── */

UT_TEST(test_strh_reg_t16_basic)
{
  setup_armv8m();

  /* strh r0, [r1, r2] => 0x5288 (GCC: 5288) */
  thumb_opcode op = th_strh_reg(0, 1, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x5288);

  /* strh r3, [r5, r2] => 0x52AB (GCC: 52ab) */
  op = th_strh_reg(3, 5, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x52AB);

  return 0;
}

/* ───── T16 high-register falls to T32 ───── */

UT_TEST(test_ldr_reg_t16_high_reg_falls_to_t32)
{
  setup_armv8m();

  /* ldr r8, [r1, r2] - r8 is high, T16 can't be used
   * Falls to T32: ldr.w r8, [r1, r2] => 0xF8518002 */
  thumb_opcode op = th_ldr_reg(8, 1, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8518002);

  return 0;
}

/* ───── T16 enforce-16bit with high reg fails ───── */

UT_TEST(test_ldr_reg_enforce_16bit_high_reg_fails)
{
  setup_armv8m();

  /* Request T16 but r8 is high -> fails to match any variant */
  thumb_opcode op = th_ldr_reg(8, 1, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  /* Also test other store/load variants */
  op = th_ldrb_reg(8, 1, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  op = th_str_reg(8, 1, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

/* ───── T32: ldr <Rt>, [<Rn>, <Rm>{, LSL #<imm>}] ───── */

UT_TEST(test_ldr_reg_t32_lsl)
{
  setup_armv8m();

  /* ldr.w r8, [r1, r2, lsl #1] => 0xF8518012 (GCC: f851 8012) */
  thumb_opcode op = th_ldr_reg(8, 1, 2, (thumb_shift){THUMB_SHIFT_LSL, 1, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8518012);

  /* ldr.w r8, [r1, r2, lsl #2] => 0xF8518022 (GCC: f851 8022) */
  op = th_ldr_reg(8, 1, 2, (thumb_shift){THUMB_SHIFT_LSL, 2, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8518022);

  /* ldr.w r8, [r1, r2] (no shift, defaults to LSL #0) => 0xF8518002 */
  op = th_ldr_reg(8, 1, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8518002);

  return 0;
}

/* ───── T32: str <Rt>, [<Rn>, <Rm>{, LSL #<imm>}] ───── */

UT_TEST(test_str_reg_t32_lsl)
{
  setup_armv8m();

  /* str.w r8, [r1, r2, lsl #2] => 0xF8418022 (GCC: f841 8022) */
  thumb_opcode op = th_str_reg(8, 1, 2, (thumb_shift){THUMB_SHIFT_LSL, 2, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8418022);

  return 0;
}

/* ───── T32: ldrb <Rt>, [<Rn>, <Rm>{, LSL #<imm>}] ───── */

UT_TEST(test_ldrb_reg_t32_lsl)
{
  setup_armv8m();

  /* ldrb.w r8, [r1, r2, lsl #1] => 0xF8118012 (GCC: f811 8012) */
  thumb_opcode op = th_ldrb_reg(8, 1, 2, (thumb_shift){THUMB_SHIFT_LSL, 1, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8118012);

  /* strb.w r8, [r1, r2, lsl #2] => 0xF8018022 (GCC: f801 8022) */
  op = th_strb_reg(8, 1, 2, (thumb_shift){THUMB_SHIFT_LSL, 2, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8018022);

  return 0;
}

/* ───── T32: ldrh <Rt>, [<Rn>, <Rm>{, LSL #<imm>}] ───── */

UT_TEST(test_ldrh_reg_t32_lsl)
{
  setup_armv8m();

  /* ldrh.w r8, [r1, r2, lsl #2] => 0xF8318022 (GCC: f831 8022) */
  thumb_opcode op = th_ldrh_reg(8, 1, 2, (thumb_shift){THUMB_SHIFT_LSL, 2, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8318022);

  /* strh.w r8, [r1, r2, lsl #2] => 0xF8218022 (GCC: f821 8022) */
  op = th_strh_reg(8, 1, 2, (thumb_shift){THUMB_SHIFT_LSL, 2, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8218022);

  return 0;
}

/* ───── T32: invalid shift type (LSR/ASR/ROR) rejected ───── */

UT_TEST(test_ldr_reg_t32_invalid_shift_type)
{
  setup_armv8m();

  /* T32 only allows LSL; LSR should fail (size=0) */
  thumb_opcode op = th_ldr_reg(8, 1, 2, (thumb_shift){THUMB_SHIFT_LSR, 2, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  op = th_ldr_reg(8, 1, 2, (thumb_shift){THUMB_SHIFT_ASR, 2, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  op = th_ldr_reg(8, 1, 2, (thumb_shift){THUMB_SHIFT_ROR, 2, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

/* ───── T32: ldrsb/ldrsh <Rt>, [<Rn>, <Rm>{, LSL #<imm>}] ───── */

UT_TEST(test_ldrsb_ldrsh_reg_t32)
{
  setup_armv8m();

  /* ldrsb.w r8, [r1, r2, lsl #2] => 0xF9118022 (GCC: f911 8022) */
  thumb_opcode op = th_ldrsb_reg(8, 1, 2, (thumb_shift){THUMB_SHIFT_LSL, 2, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF9118022);

  /* ldrsh.w r8, [r1, r2, lsl #2] => 0xF9318022 (GCC: f931 8022) */
  op = th_ldrsh_reg(8, 1, 2, (thumb_shift){THUMB_SHIFT_LSL, 2, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF9318022);

  return 0;
}

/* ───── T32: register constraint SP not allowed ───── */

UT_TEST(test_ldr_reg_t32_sp_constraint)
{
  setup_armv8m();

  /* rm=SP not allowed in T32 register offset */
  thumb_opcode op = th_ldr_reg(8, 1, 13, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  /* rt=SP not allowed */
  op = th_ldr_reg(13, 1, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

/* ───── T32: register constraint PC not allowed ───── */

UT_TEST(test_ldr_reg_t32_pc_constraint)
{
  setup_armv8m();

  /* rm=PC not allowed in T32 register offset */
  thumb_opcode op = th_ldr_reg(8, 1, 15, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  /* rn=PC not allowed */
  op = th_ldr_reg(8, 15, 2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

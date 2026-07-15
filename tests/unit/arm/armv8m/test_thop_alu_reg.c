/*
 *  test_thop_alu_reg.c - suite for arch/arm/thumb/thop_alu_reg.c
 *
 *  Tests T1 (16-bit, low reg3), T1 rdn-rm (ADC/SBC/AND/BIC/ORR/EOR),
 *  ADD-SP-reg, ADD-high-reg (T2), and T3 (32-bit wide with shift) for:
 *  ADD, SUB, RSB, ADC, SBC, AND, BIC, ORR, ORN, EOR.
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_alu_reg.h"
#include "source/backend/arch/arm/thumb/thumb.h"

#include "ut.h"

/* ------------------------------------------------------------------ setup */

static void setup_armv7m(void)
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
          },
      .is_secure_tz = false,
  };
}

/* ------------------------------------------------------------------ ADD */

UT_TEST(test_add_reg_t16_low_reg3)
{
  setup_armv7m();

  /* T1: rd=0, rn=1, rm=2 => 0x1800 | (0<<3) | (1<<6) | (2<<9) = 0x1888 */
  /* T1: rd=0, rn=1, rm=2 => 0x1800 | (0<<3) | (1<<6) | (2<<9) = 0x1888 */
  thumb_opcode op = th_add_reg(0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x1888);

  /* T1: rd=7, rn=6, rm=5 => 0x1800 | (7<<3) | (6<<6) | (5<<9) = 0x1977 */
  op = th_add_reg(7, 6, 5, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x1977);
  return 0;
}

UT_TEST(test_add_reg_t16_sp_reg)
{
  setup_armv7m();

  /* ADD SP, R0, R0 — rd==rm==R0, rn==SP. DN:Rd split: dn=0, rd_low=0.\n   * Base 0x4400 | (0<<3) [rn=SP at bits 3-6] =
   * 0x4468 */
  thumb_opcode op = th_add_reg(R0, R_SP, R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4468);

  /* ADD SP, R7, R7 — dn=0, rd_low=7. Base 0x4400 | (13<<3) [rn=SP] + low Rd bits 7 => 0x446F */
  op = th_add_reg(R7, R_SP, R7, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x446F);

  return 0;
}

UT_TEST(test_add_reg_t16_high_reg)
{
  setup_armv7m();

  /* ADD R8, R8, R9 — rd==rn==R8. DN:Rd split: dn=1 (R8>>3), rd_low=0. rm=R9=9.
   * Base 0x4400 | (1<<7) [DN] | (0<<0) [rd_low] | (9<<3) [rm] = 0x44C8 */
  thumb_opcode op = th_add_reg(R8, R8, R9, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x44C8);

  return 0;
}

UT_TEST(test_add_reg_t32_with_shift)
{
  setup_armv7m();

  /* T3: rd=0, rn=1, rm=2, flags=SET, shift={LSL,1}.
   * Base 0xEB000000 | (0<<8) [rd] | (1<<16) [rn] | (2<<0) [rm] |
   * (1<<20) [S] | (0<<4) [shift_type=LSL] | (1<<6) [imm2] | (0<<12) [imm3]
   * = 0xEB000000 | 0x00010000 | 0x00000002 | 0x00100000 | 0x00000000 | 0x00000040 | 0x00000000
   * = 0xEB110042 */
  thumb_shift shift = {THUMB_SHIFT_LSL, 1, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_add_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEB110042);

  /* LSR shift */
  shift = (thumb_shift){THUMB_SHIFT_LSR, 8, THUMB_SHIFT_IMMEDIATE};
  op = th_add_reg(5, 4, 3, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* base=0xEB000000 | S=1<<20=0x00100000 => 0xEB100000
   * | rd=5<<8=0x00000500 | rn=4<<16=0x00040000 | rm=3=0x00000003
   * | shift_type=LSR=1 at bits 4-5 => 0x10 | imm2=8&3=0 | imm3=(8>>2)&7=2 at bits 12-14 => 0x2000
   * = 0xEB142513 */
  UT_ASSERT_EQ(op.opcode, 0xEB142513);

  return 0;
}

UT_TEST(test_add_reg_enforce_16bit_with_shift_fails)
{
  setup_armv7m();

  /* Enforce T16 with shift — no T16 variant supports shift, so fails */
  thumb_shift shift = {THUMB_SHIFT_LSL, 1, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_add_reg(0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, shift, ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

UT_TEST(test_add_reg_enforce_32bit_low_regs)
{
  setup_armv7m();

  /* Enforce T32 with low regs — should produce T3 encoding */
  thumb_shift shift = THUMB_SHIFT_DEFAULT;
  thumb_opcode op = th_add_reg(0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, shift, ENFORCE_ENCODING_32BIT);
  UT_ASSERT_EQ(op.size, 4);
  /* base=0xEB000000 | rd=0<<8=0 | rn=1<<16=0x00010000 | rm=2=0x00000002
   * = 0xEB010002 */
  UT_ASSERT_EQ(op.opcode, 0xEB010002);

  return 0;
}

/* ------------------------------------------------------------------ SUB */

UT_TEST(test_sub_reg_t16_low_reg3)
{
  setup_armv7m();

  /* T1: rd=0, rn=1, rm=2 => 0x1A00 | (0<<3) | (1<<6) | (2<<9) = 0x1A88 */
  thumb_opcode op = th_sub_reg(0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x1A88);

  return 0;
}

UT_TEST(test_sub_reg_t32_with_shift)
{
  setup_armv7m();

  /* T3: rd=0, rn=1, rm=2, flags=SET, shift={LSL,1}.
   * Base 0xEBA00000 | (0<<8) | (1<<16) | (2<<0) | (1<<20) [S] | (2<<4) [LSL] | (1&3<<6)
   * = 0xEBA00000 | 0x00010000 | 0x00000004 | 0x00100000 | 0x00000010 | 0x00000000
   * = 0xEBB10002 */
  thumb_shift shift = {THUMB_SHIFT_LSL, 1, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_sub_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEBB10042);

  return 0;
}

/* ------------------------------------------------------------------ RSB */

UT_TEST(test_rsb_reg_t32_only)
{
  setup_armv7m();

  /* RSB has no T16 variant — always T32 */
  thumb_opcode op = th_rsb_reg(0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* Base 0xEBC00000 | (0<<8) | (1<<16) | (2<<0) = 0xEBC10002 */
  UT_ASSERT_EQ(op.opcode, 0xEBC10002);

  return 0;
}

UT_TEST(test_rsb_reg_t32_with_shift)
{
  setup_armv7m();

  /* T3: RSB with ASR shift */
  thumb_shift shift = {THUMB_SHIFT_ASR, 4, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_rsb_reg(3, 2, 1, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* base=0xEBC00000 | S=1<<20=0x00100000 => 0xEBD00000
   * | rd=3<<8=0x00000300 | rn=2<<16=0x00020000 | rm=1=0x00000001
   * | shift_type=ASR=2 at bits 4-5 => 0x20 | imm2=4&3=0 | imm3=(4>>2)&7=1 at bits 12-14 => 0x1000
   * = 0xEBD21321 */
  UT_ASSERT_EQ(op.opcode, 0xEBD21321);

  return 0;
}

/* ------------------------------------------------------------------ ADC */

UT_TEST(test_adc_reg_t16_rdn_rm)
{
  setup_armv7m();

  /* T1: rd==rn==R0, rm=R1 => base 0x4140 | (1<<3) = 0x4148 */
  thumb_opcode op = th_adc_reg(0, 0, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4148);

  /* T1: rd==rn==R7, rm=R6 => base 0x4140 | (6<<3) = 0x4177 */
  op = th_adc_reg(7, 7, 6, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4177);

  return 0;
}

UT_TEST(test_adc_reg_t32_with_shift)
{
  setup_armv7m();

  /* T3: ADC with ROR shift */
  thumb_shift shift = {THUMB_SHIFT_ROR, 5, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_adc_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* base=0xEB400000 | S=1<<20=0x00100000 => 0xEB500000
   * | rd=0<<8=0 | rn=1<<16=0x00010000 | rm=2=0x00000002
   * | shift_type=ROR=3 at bits 4-5 => 0x30 | imm2=5&3=1 at bits 6-7 => 0x40 | imm3=(5>>2)&7=1 at bits 12-14 => 0x1000
   * = 0xEB511072 */
  UT_ASSERT_EQ(op.opcode, 0xEB511072);

  return 0;
}

/* ------------------------------------------------------------------ SBC */

UT_TEST(test_sbc_reg_t16_rdn_rm)
{
  setup_armv7m();

  /* T1: rd==rn==R3, rm=R4 => base 0x4180 | (3<<0) | (4<<3) = 0x41A3 */
  thumb_opcode op = th_sbc_reg(3, 3, 4, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x41A3);

  return 0;
}

UT_TEST(test_sbc_reg_t32_with_shift)
{
  setup_armv7m();

  /* T3: SBC with RRX */
  thumb_shift shift = {THUMB_SHIFT_RRX, 0, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_sbc_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* base=0xEB600000 | S=1<<20=0x00100000 => 0xEB700000
   * | rd=0<<8=0 | rn=1<<16=0x00010000 | rm=2=0x00000002
   * | shift_type=RRX=3 at bits 4-5 => 0x30 | imm2=0 | imm3=0
   * = 0xEB710032 */
  UT_ASSERT_EQ(op.opcode, 0xEB710032);

  return 0;
}

/* ------------------------------------------------------------------ AND */

UT_TEST(test_and_reg_t16_rdn_rm)
{
  setup_armv7m();

  /* T1: rd==rn==R5, rm=R3 => base 0x4000 | (5<<0) | (3<<3) = 0x401D */
  thumb_opcode op = th_and_reg(5, 5, 3, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x401D);

  return 0;
}

UT_TEST(test_and_reg_t32_with_shift)
{
  setup_armv7m();

  /* T3: AND with LSL shift */
  thumb_shift shift = {THUMB_SHIFT_LSL, 16, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_and_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* base=0xEA000000 | S=1<<20=0x00100000 => 0xEA100000
   * | rd=0<<8=0 | rn=1<<16=0x00010000 | rm=2=0x00000002
   * | shift_type=LSL=0 at bits 4-5 => 0 | imm2=16&3=0 | imm3=(16>>2)&7=4 at bits 12-14 => 0x4000
   * = 0xEA114002 */
  UT_ASSERT_EQ(op.opcode, 0xEA114002);

  return 0;
}

/* ------------------------------------------------------------------ BIC */

UT_TEST(test_bic_reg_t16_rdn_rm)
{
  setup_armv7m();

  /* T1: rd==rn==R2, rm=R5 => base 0x4380 | (2<<0) | (5<<3) = 0x43AA */
  thumb_opcode op = th_bic_reg(2, 2, 5, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x43AA);

  return 0;
}

UT_TEST(test_bic_reg_t32_with_shift)
{
  setup_armv7m();

  /* T3: BIC with LSR shift */
  thumb_shift shift = {THUMB_SHIFT_LSR, 2, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_bic_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* base=0xEA200000 | S=1<<20=0x00100000 => 0xEA300000
   * | rd=0<<8=0 | rn=1<<16=0x00010000 | rm=2=0x00000002
   * | shift_type=LSR=1 at bits 4-5 => 0x10 | imm2=2&3=2 at bits 6-7 => 0x80 | imm3=(2>>2)&7=0
   * = 0xEA310092 */
  UT_ASSERT_EQ(op.opcode, 0xEA310092);

  return 0;
}

/* ------------------------------------------------------------------ ORR */

UT_TEST(test_orr_reg_t16_rdn_rm)
{
  setup_armv7m();

  /* T1: rd==rn==R4, rm=R0 => base 0x4300 | (4<<0) | (0<<3) = 0x4304 */
  thumb_opcode op = th_orr_reg(4, 4, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4304);

  return 0;
}

UT_TEST(test_orr_reg_t32_with_shift)
{
  setup_armv7m();

  /* T3: ORR with ASR shift */
  thumb_shift shift = {THUMB_SHIFT_ASR, 1, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_orr_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* base=0xEA400000 | S=1<<20=0x00100000 => 0xEA500000
   * | rd=0<<8=0 | rn=1<<16=0x00010000 | rm=2=0x00000002
   * | shift_type=ASR=2 at bits 4-5 => 0x20 | imm2=1&3=1 at bits 6-7 => 0x40 | imm3=(1>>2)&7=0
   * = 0xEA510062 */
  UT_ASSERT_EQ(op.opcode, 0xEA510062);

  return 0;
}

/* ------------------------------------------------------------------ ORN (T3 only) */

UT_TEST(test_orn_reg_t32_only)
{
  setup_armv7m();

  /* ORN has no T16 variant — always T32 */
  thumb_opcode op = th_orn_reg(0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* Base 0xEA600000 | (0<<8) | (1<<16) | (2<<0) = 0xEA610002 */
  UT_ASSERT_EQ(op.opcode, 0xEA610002);

  return 0;
}

UT_TEST(test_orn_reg_t32_with_shift)
{
  setup_armv7m();

  /* T3: ORN with ROR shift */
  thumb_shift shift = {THUMB_SHIFT_ROR, 3, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_orn_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* base=0xEA600000 | rd=0<<8=0 | rn=1<<16=0x00010000 | rm=2=0x00000002\n   * S=1<<20=0x00100000 | shift_type=ROR=5 at
   * bits 4-5 => 0x50 | imm2=3&3=3 at bits 6-7 => 0xC0 | imm3=(3>>2)&7=0\n   * = 0xEA600000 | 0x00010000 | 0x00000002 |
   * 0x00100000 | 0x00000050 | 0x000000C0\n   * = 0xEA7100F2 */
  UT_ASSERT_EQ(op.opcode, 0xEA7100F2);

  return 0;
}

/* ------------------------------------------------------------------ EOR */

UT_TEST(test_eor_reg_t16_rdn_rm)
{
  setup_armv7m();

  /* T1: rd==rn==R6, rm=R1 => base 0x4040 | (6<<0) | (1<<3) = 0x404E */
  thumb_opcode op = th_eor_reg(6, 6, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x404E);

  return 0;
}

UT_TEST(test_eor_reg_t32_with_shift)
{
  setup_armv7m();

  /* T3: EOR with RRX */
  thumb_shift shift = {THUMB_SHIFT_RRX, 0, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_eor_reg(0, 1, 2, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* base=0xEA800000 | S=1<<20=0x00100000 => 0xEA900000
   * | rd=0<<8=0 | rn=1<<16=0x00010000 | rm=2=0x00000002
   * | shift_type=RRX=3 at bits 4-5 => 0x30 | imm2=0 | imm3=0
   * = 0xEA910032 */
  UT_ASSERT_EQ(op.opcode, 0xEA910032);

  return 0;
}

/* ------------------------------------------------------------------ constraint failures */

UT_TEST(test_add_reg_high_reg_falls_to_t3)
{
  setup_armv7m();

  /* T16 ADD requires low regs (REG_LOW_ONLY). R8 is high reg, so falls to T32. */
  thumb_opcode op = th_add_reg(R8, R9, R10, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* Falls to T32: base=0xEB000000 | rd=8<<8=0x00000800 | rn=9<<16=0x00090000 | rm=10=0x0000000A
   * = 0xEB09080A */
  UT_ASSERT_EQ(op.opcode, 0xEB09080A);

  return 0;
}

UT_TEST(test_adc_reg_high_reg_fails)
{
  setup_armv7m();

  /* T16 ADC requires rd==rn and low regs. R8 is high reg — fails both constraints. */
  thumb_opcode op = th_adc_reg(R8, R8, R9, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* Falls to T32: base=0xEB400000 | rd=8<<8=0x00000800 | rn=8<<16=0x00080000 | rm=9=0x00000009
   * = 0xEB480809 */
  UT_ASSERT_EQ(op.opcode, 0xEB480809);

  return 0;
}

UT_TEST(test_add_reg_enforce_16bit_high_reg_fails)
{
  setup_armv7m();

  thumb_opcode op = th_add_reg(R8, R8, R9, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x44C8);

  return 0;
}

UT_TEST(test_adc_reg_rd_ne_rn_fails_t1)
{
  setup_armv7m();

  /* T1 ADC requires rd==rn. R0!=R1 so falls to T32. */
  thumb_opcode op = th_adc_reg(0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* base=0xEB400000 | rd=0<<8=0 | rn=1<<16=0x00010000 | rm=2=0x00000002
   * = 0xEB410002 */
  UT_ASSERT_EQ(op.opcode, 0xEB410002);

  return 0;
}

UT_TEST(test_add_reg_sp_in_rm_fails_t3)
{
  setup_armv7m();

  /* T3 ADD requires rm != SP (REG_NOT_SP). R13=SP is rejected. */
  thumb_opcode op = th_add_reg(0, 1, R_SP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

UT_TEST(test_add_reg_pc_in_rn_fails)
{
  setup_armv7m();

  /* T3 ADD requires rn != PC (REG_NOT_PC). R15=PC is rejected. */
  thumb_opcode op = th_add_reg(0, R_PC, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

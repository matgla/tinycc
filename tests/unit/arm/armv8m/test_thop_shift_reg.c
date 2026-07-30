/*
 *  test_thop_shift_reg.c - suite for arch/arm/thumb/thop_shift_reg.c
 *
 *  Tests T1 (16-bit, low regs, rd==rn) and T3 (32-bit wide) for:
 *  LSL, LSR, ASR, ROR.
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_shift_reg.h"
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

/* ------------------------------------------------------------------ T1 tests */

UT_TEST(test_th_lsl_reg_t1_low)
{
  setup_armv7m();

  /* T1: lsls r0, r0, r1 => base 0x4080 | (1<<3) | 0 = 0x4088 */
  thumb_opcode op = th_lsl_reg(0, 0, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4088);

  return 0;
}

UT_TEST(test_th_lsr_reg_t1_low)
{
  setup_armv7m();

  /* T1: lsrs r1, r1, r2 => base 0x40C0 | (2<<3) | 1 = 0x40D1 */
  thumb_opcode op = th_lsr_reg(1, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x40D1);

  return 0;
}

UT_TEST(test_th_asr_reg_t1_low)
{
  setup_armv7m();

  /* T1: asrs r3, r3, r4 => base 0x4100 | (4<<3) | 3 = 0x4123 */
  thumb_opcode op = th_asr_reg(3, 3, 4, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4123);

  return 0;
}

/* ------------------------------------------------------------------ T3 tests */

UT_TEST(test_th_lsl_reg_t3_high)
{
  setup_armv7m();

  /* T3: lsl.w r8, r9, r10
   * base 0xFA00F000 | rn=9<<16 | rd=8<<8 | rm=10
   * = 0xFA09F80A */
  thumb_opcode op = th_lsl_reg(8, 9, 10, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFA09F80A);

  return 0;
}

UT_TEST(test_th_lsr_reg_t3_high)
{
  setup_armv7m();

  /* T3: lsr.w r8, r9, r10
   * base 0xFA20F000 | rn=9<<16 | rd=8<<8 | rm=10
   * = 0xFA29F80A */
  thumb_opcode op = th_lsr_reg(8, 9, 10, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFA29F80A);

  return 0;
}

UT_TEST(test_th_asr_reg_t3_high)
{
  setup_armv7m();

  /* T3: asr.w r8, r9, r10
   * base 0xFA40F000 | rn=9<<16 | rd=8<<8 | rm=10
   * = 0xFA49F80A */
  thumb_opcode op = th_asr_reg(8, 9, 10, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFA49F80A);

  return 0;
}

UT_TEST(test_th_ror_reg_t3_only)
{
  setup_armv7m();

  /* ROR has no T1 variant — always T3
   * T3: ror.w r8, r9, r10
   * base 0xFA60F000 | rn=9<<16 | rd=8<<8 | rm=10
   * = 0xFA69F80A */
  thumb_opcode op = th_ror_reg(8, 9, 10, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFA69F80A);

  return 0;
}

UT_TEST(test_th_ror_reg_t3_low)
{
  setup_armv7m();

  /* T3: ror.w r0, r1, r2
   * base 0xFA60F000 | rn=1<<16 | rd=0<<8 | rm=2
   * = 0xFA61F002 */
  thumb_opcode op = th_ror_reg(0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFA61F002);

  return 0;
}

/* ------------------------------------------------------------------ flags */

UT_TEST(test_th_lsl_reg_t3_set_flags)
{
  setup_armv7m();

  /* T3: lsls.w r8, r9, r10
   * base 0xFA00F000 | S=1<<20 | rn=9<<16 | rd=8<<8 | rm=10
   * = 0xFA19F80A */
  thumb_opcode op = th_lsl_reg(8, 9, 10, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFA19F80A);

  return 0;
}

UT_TEST(test_th_lsr_reg_t3_set_flags)
{
  setup_armv7m();

  /* T3: lsrs.w r8, r9, r10
   * base 0xFA20F000 | S=1<<20 | rn=9<<16 | rd=8<<8 | rm=10
   * = 0xFA39F80A */
  thumb_opcode op = th_lsr_reg(8, 9, 10, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFA39F80A);

  return 0;
}

/* ------------------------------------------------------------------ constraint failures */

UT_TEST(test_th_lsl_reg_t1_rd_ne_rn_falls_to_t3)
{
  setup_armv7m();

  /* T1 requires rd==rn. rd=0, rn=1 fails T1, falls to T3. */
  thumb_opcode op = th_lsl_reg(0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* T3: base 0xFA00F000 | rn=1<<16 | rd=0<<8 | rm=2 = 0xFA01F002 */
  UT_ASSERT_EQ(op.opcode, 0xFA01F002);

  return 0;
}

UT_TEST(test_th_lsl_reg_enforce_16bit_rd_ne_rn_fails)
{
  setup_armv7m();

  /* Enforce T16 with rd!=rn — T1 constraint fails, no other T16 variant */
  thumb_opcode op = th_lsl_reg(0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

UT_TEST(test_th_lsl_reg_enforce_32bit_low_regs)
{
  setup_armv7m();

  /* Enforce T32 with low regs — should produce T3 encoding */
  thumb_opcode op = th_lsl_reg(0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_32BIT);
  UT_ASSERT_EQ(op.size, 4);
  /* T3: base 0xFA00F000 | rn=1<<16 | rd=0<<8 | rm=2 = 0xFA01F002 */
  UT_ASSERT_EQ(op.opcode, 0xFA01F002);

  return 0;
}

UT_TEST(test_th_lsl_reg_t1_high_reg_falls_to_t3)
{
  setup_armv7m();

  /* T1 requires low regs. R8 is high reg, so falls to T3. */
  thumb_opcode op = th_lsl_reg(R8, R8, R9, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  /* T3: base 0xFA00F000 | rn=8<<16 | rd=8<<8 | rm=9 = 0xFA08F809 */
  UT_ASSERT_EQ(op.opcode, 0xFA08F809);

  return 0;
}

UT_TEST(test_th_lsl_reg_pc_in_rd_fails)
{
  setup_armv7m();

  /* T3 requires rd != PC. R15=PC is rejected. */
  thumb_opcode op = th_lsl_reg(R_PC, R1, R2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

UT_TEST(test_th_lsl_reg_sp_in_rm_fails)
{
  setup_armv7m();

  /* T3 requires rm != SP. R13=SP is rejected. */
  thumb_opcode op = th_lsl_reg(R0, R1, R_SP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

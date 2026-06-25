/*
 *  test_thop_mvn.c - suite for arch/arm/thumb/thop_mvn.c
 *
 *  Tests MVN (Move NOT) with immediate and register operands.
 *  MVN immediate uses modified immediate (T32 only).
 *  MVN register has T1 (low regs, rd==rn, implicit S) and T3 (wide, any reg with shift).
 *
 *  All expected opcodes verified against arm-none-eabi-as output.
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_mvn.h"
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

/* ───── MVN register T1 (low regs, rd==rn, implicit S) ───── */

UT_TEST(test_mvn_reg_t1_basic)
{
  setup_armv8m();

  /* mvns r0, r1 — rd=0, rm=1 => 0x43C0 | 0 | 0x08 = 0x43C8 */
  thumb_opcode op = th_mvn_reg(0, 0, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x43C8);

  /* mvns r5, r3 — rd=5, rm=3 => 0x43C0 | 5 | (3<<3) = 0x43C0 | 0x05 | 0x18 = 0x43DD */
  op = th_mvn_reg(5, 5, 3, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x43DD);

  return 0;
}

UT_TEST(test_mvn_reg_t1_rd_ne_rn_falls_to_t3)
{
  setup_armv8m();

  /* MVN T1 requires rd==rn. r0!=r1 -> falls to T3. */
  /* mvn.w r0, r1 => 0xEA6F0001 */
  thumb_opcode op = th_mvn_reg(0, 1, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEA6F0001);

  return 0;
}

/* ───── MVN register T3 (wide, any reg with shift) ───── */

UT_TEST(test_mvn_reg_t3_basic)
{
  setup_armv8m();

  /* mvn.w r0, r1 — no shift => 0xEA6F0001 */
  thumb_opcode op = th_mvn_reg(0, 1, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEA6F0001);

  return 0;
}

UT_TEST(test_mvn_reg_t3_high_reg)
{
  setup_armv8m();

  /* mvn.w r5, r7 — rd=5, rm=7 => base 0xEA6F0000 | rd=5<<8=0x0500 | rm=7 = 0xEA6F0507 */
  thumb_opcode op = th_mvn_reg(5, 7, 7, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEA6F0507);

  return 0;
}

UT_TEST(test_mvn_reg_t3_with_shift_lsl)
{
  setup_armv8m();

  /* mvns.w r8, r9, lsl #1 => 0xEA7F0849 */
  thumb_shift shift = {THUMB_SHIFT_LSL, 1, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_mvn_reg(8, 8, 9, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEA7F0849);

  return 0;
}

UT_TEST(test_mvn_reg_t3_with_shift_lsr)
{
  setup_armv8m();

  /* mvns r6, r8, lsr #2 => base=0xEA6F0000 | S=1<<20=0x100000 | rd=6<<8=0x0600 | rm=8=0x08
   * | shift_type=LSR=1<<4=0x10 | imm2=2<<6=0x80 | imm3=(2>>2)<<12=0
   * = 0xEA7F0698 */
  thumb_shift shift = {THUMB_SHIFT_LSR, 2, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_mvn_reg(6, 6, 8, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEA7F0698);

  return 0;
}

UT_TEST(test_mvn_reg_t3_with_shift_asr)
{
  setup_armv8m();

  /* mvns r1, r2, asr #3 => base=0xEA6F0000 | S=1<<20=0x100000 | rd=1<<8=0x0100 | rm=2=0x02
   * | shift_type=ASR=2<<4=0x20 | imm2=3<<6=0xC0 | imm3=(3>>2)<<12=0
   * = 0xEA7F01E2 */
  thumb_shift shift = {THUMB_SHIFT_ASR, 3, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_mvn_reg(1, 1, 2, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEA7F01E2);

  return 0;
}

UT_TEST(test_mvn_reg_t3_with_shift_ror)
{
  setup_armv8m();

  /* mvns r6, r8, ror #4 => base=0xEA6F0000 | S=1<<20=0x100000 | rd=6<<8=0x0600 | rm=8=0x08
   * | shift_type=ROR=3<<4=0x30 | imm2=(4&3)<<6=0 | imm3=(4>>2)<<12=0x1000
   * = 0xEA7F1638 */
  thumb_shift shift = {THUMB_SHIFT_ROR, 4, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_mvn_reg(6, 6, 8, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEA7F1638);

  return 0;
}

UT_TEST(test_mvn_reg_t3_with_rrx)
{
  setup_armv8m();

  /* mvns r1, r2, rrx => base=0xEA6F0000 | S=1<<20=0x100000 | rd=1<<8=0x0100 | rm=2=0x02
   * | shift_type=RRX=3<<4=0x30 | imm2=0 | imm3=0
   * = 0xEA7F0132 */
  thumb_shift shift = {THUMB_SHIFT_RRX, 0, THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_mvn_reg(1, 1, 2, FLAGS_BEHAVIOUR_SET, shift, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEA7F0132);

  return 0;
}

UT_TEST(test_mvn_reg_t3_setflags)
{
  setup_armv8m();

  /* mvns.w r1, r2 — S bit set */
  /* mvn r1, r2 (no shift, no S) => 0xEA6F0102
   * with S=1 => 0xEA7F0102 */
  thumb_opcode op = th_mvn_reg(1, 2, 2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEA7F0102);

  return 0;
}

UT_TEST(test_mvn_reg_t3_enforce_16bit_fails)
{
  setup_armv8m();

  /* MVN T1 only works with low regs and rd==rn. High reg fails T1. */
  thumb_opcode op = th_mvn_reg(8, 8, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

/* ───── MVN immediate T3 (modified immediate, always 32-bit) ───── */

UT_TEST(test_mvn_imm_basic)
{
  setup_armv8m();

  /* mvn r0, #0 => 0xF06F0000 */
  thumb_opcode op = th_mvn_imm(0, 0, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF06F0000);

  /* mvn r0, #1 => 0xF06F0001 */
  op = th_mvn_imm(0, 0, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF06F0001);

  /* mvn r0, #0xff => 0xF06F00FF */
  op = th_mvn_imm(0, 0, 0xFF, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF06F00FF);

  /* mvn r0, #18 => 0xF06F0012 */
  op = th_mvn_imm(0, 0, 18, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF06F0012);

  return 0;
}

UT_TEST(test_mvn_imm_with_flags)
{
  setup_armv8m();

  /* mvns r0, #0xff => 0xF07F00FF (S bit set) */
  thumb_opcode op = th_mvn_imm(0, 0, 0xFF, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF07F00FF);

  /* mvns r0, #1 => 0xF07F0001 */
  op = th_mvn_imm(0, 0, 1, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF07F0001);

  return 0;
}

UT_TEST(test_mvn_imm_high_reg)
{
  setup_armv8m();

  /* mvn r8, #0xb => 0xF06F080B */
  thumb_opcode op = th_mvn_imm(8, 0, 0xB, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF06F080B);

  return 0;
}

UT_TEST(test_mvn_imm_enforce_16bit_fails)
{
  setup_armv8m();

  /* MVN immediate is T32 only, so 16-bit enforcement must fail */
  thumb_opcode op = th_mvn_imm(0, 0, 0xFF, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

/* ───── constraint failures ───── */

UT_TEST(test_mvn_reg_t1_high_reg_fails)
{
  setup_armv8m();

  /* MVN T1 requires low regs only */
  thumb_opcode op = th_mvn_reg(8, 8, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEA6F0801);

  return 0;
}

UT_TEST(test_mvn_reg_t3_pc_in_rm_fails)
{
  setup_armv8m();

  /* T3 MVN: rm != PC */
  thumb_opcode op = th_mvn_reg(0, 0, 15, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

UT_TEST(test_mvn_reg_t3_sp_in_rm_fails)
{
  setup_armv8m();

  /* T3 MVN: rm != SP */
  thumb_opcode op = th_mvn_reg(0, 0, 13, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

/* ───── suite ───── */

UT_SUITE(thop_mvn)
{
  /* MVN register T1 */
  UT_RUN(test_mvn_reg_t1_basic);
  UT_RUN(test_mvn_reg_t1_rd_ne_rn_falls_to_t3);

  /* MVN register T3 */
  UT_RUN(test_mvn_reg_t3_basic);
  UT_RUN(test_mvn_reg_t3_high_reg);
  UT_RUN(test_mvn_reg_t3_with_shift_lsl);
  UT_RUN(test_mvn_reg_t3_with_shift_lsr);
  UT_RUN(test_mvn_reg_t3_with_shift_asr);
  UT_RUN(test_mvn_reg_t3_with_shift_ror);
  UT_RUN(test_mvn_reg_t3_with_rrx);
  UT_RUN(test_mvn_reg_t3_setflags);
  UT_RUN(test_mvn_reg_t3_enforce_16bit_fails);

  /* MVN immediate T3 */
  UT_RUN(test_mvn_imm_basic);
  UT_RUN(test_mvn_imm_with_flags);
  UT_RUN(test_mvn_imm_high_reg);
  UT_RUN(test_mvn_imm_enforce_16bit_fails);

  /* Constraint failures */
  UT_RUN(test_mvn_reg_t1_high_reg_fails);
  UT_RUN(test_mvn_reg_t3_pc_in_rm_fails);
  UT_RUN(test_mvn_reg_t3_sp_in_rm_fails);
}
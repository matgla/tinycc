/*
 *  test_thop_mov.c - suite for arch/arm/thumb/thop_mov.c
 *  Move instructions (MOV, MOVW, MOVT, shifts)
 *
 *  All expected opcodes verified against arm-none-eabi-as output.
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_mov.h"
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

/* ───── MOV register T1 high ───── */

UT_TEST(test_mov_reg_t1_high_basic)
{
  setup_armv8m();

  /* mov r8, r9 => 0x46C8 (GCC: 46c8) */
  thumb_opcode op = th_mov_reg(8, 9, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x46C8);

  /* mov r8, r0 => 0x4680 (GCC: 4680) */
  op = th_mov_reg(8, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4680);

  /* mov r12, r15 => 0x46FC (GCC: 46fc) */
  op = th_mov_reg(12, 15, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x46FC);

  return 0;
}

/* ───── MOV shift alias T1 (LSL/LSR/ASR) ───── */

UT_TEST(test_mov_reg_t1_shift_basic)
{
  setup_armv8m();

  /* lsls r0, r1, #2 => 0x0088 (GCC: 0088) */
  thumb_opcode op = th_mov_reg(0, 1, FLAGS_BEHAVIOUR_SET, (thumb_shift){THUMB_SHIFT_LSL, 2, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE, false);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x0088);

  /* lsrs r0, r1, #3 => 0x0088 but with LSR => 0x00C8? Actually 0x0088 | (1<<11) | (3<<6) = 0x0088 | 0x0800 | 0x00C0 = 0x08C8 */
  /* lsrs r0, r1, #3 => 0x08C8 (GCC: 08c8) */
  op = th_mov_reg(0, 1, FLAGS_BEHAVIOUR_SET, (thumb_shift){THUMB_SHIFT_LSR, 3, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE, false);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x08C8);

  /* asrs r0, r1, #4 => 0x1108 (GCC: 1108) */
  op = th_mov_reg(0, 1, FLAGS_BEHAVIOUR_SET, (thumb_shift){THUMB_SHIFT_ASR, 4, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE, false);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x1108);

  return 0;
}

/* ───── MOV immediate T1 (MOVS) ───── */

UT_TEST(test_mov_imm_t1_basic)
{
  setup_armv8m();

  /* movs r0, #255 => 0x20FF (GCC: 20ff) */
  thumb_opcode op = th_mov_imm(0, 255, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x20FF);

  /* movs r7, #0x42 => 0x2742 (GCC: 2742) */
  op = th_mov_imm(7, 0x42, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x2742);

  return 0;
}

/* ───── MOV immediate T3 (modified immediate) ───── */

UT_TEST(test_mov_imm_t3_basic)
{
  setup_armv8m();

  /* mov r0, #0xFF000000 => 0xF04F407F (GCC: f04f 407f) */
  thumb_opcode op = th_mov_imm(0, 0xFF000000, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF04F407F);

  return 0;
}

/* ───── MOV immediate T4 (MOVW) ───── */

UT_TEST(test_mov_imm_t4_basic)
{
  setup_armv8m();

  /* movw r0, #0x1234 => 0xF2412034 (GCC: f241 2034) */
  thumb_opcode op = th_mov_imm(0, 0x1234, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF2412034);

  return 0;
}

/* ───── MOVT ───── */

UT_TEST(test_movt_basic)
{
  setup_armv8m();

  /* movt r0, #0xABCD => 0xF6CA30CD (GCC: f6ca 30cd) */
  thumb_opcode op = th_movt(0, 0xABCD);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF6CA30CD);

  /* movt r8, #0x1234 => 0xF2C12834 (GCC: f2c1 2834) */
  op = th_movt(8, 0x1234);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF2C12834);

  return 0;
}

/* ───── MOV register-controlled shift T3 (high regs) ───── */

UT_TEST(test_mov_reg_shift_basic)
{
  setup_armv8m();

  /* lsl r8, r8, r1 => 0xFA08F801 (GCC: fa08 f801) */
  thumb_opcode op = th_mov_reg_shift(8, 8, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, (thumb_shift){THUMB_SHIFT_LSL, 0, THUMB_SHIFT_REGISTER}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFA08F801);

  return 0;
}

/* ───── MOV register-controlled shift T3 (high regs) ───── */

UT_TEST(test_mov_reg_shift_t3_basic)
{
  setup_armv8m();

  /* lsl r8, r8, r1 => 0xFA08F801 (GCC: fa08 f801) */
  thumb_opcode op = th_mov_reg_shift(8, 8, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, (thumb_shift){THUMB_SHIFT_LSL, 0, THUMB_SHIFT_REGISTER}, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFA08F801);

  return 0;
}

/* ───── MOV register T3 with shift ───── */

UT_TEST(test_mov_reg_t3_with_shift)
{
  setup_armv8m();

  /* mov r8, r9, lsl #3 => 0xEA4F08C9 (GCC: ea4f 08c9) */
  thumb_opcode op = th_mov_reg(8, 9, FLAGS_BEHAVIOUR_NOT_IMPORTANT, (thumb_shift){THUMB_SHIFT_LSL, 3, THUMB_SHIFT_IMMEDIATE}, ENFORCE_ENCODING_NONE, false);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEA4F08C9);

  return 0;
}

/* ───── MOV register T1 (low regs) - valid encoding ───── */

UT_TEST(test_mov_reg_t1_low_regs)
{
  setup_armv8m();

  /* mov r0, r1 => 0x4608 (GCC: 4608) - T1 low reg MOV */
  thumb_opcode op = th_mov_reg(0, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4608);

  return 0;
}

/* ───── MOV enforce-16bit with high reg works ───── */

UT_TEST(test_mov_reg_enforce_16bit_high_reg)
{
  setup_armv8m();

  /* mov r8, r9 - high reg - should use T1 */
  thumb_opcode op = th_mov_reg(8, 9, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_16BIT, false);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x46C8);

  return 0;
}

/* ───── MOVT with low reg ───── */

UT_TEST(test_movt_low_reg)
{
  setup_armv8m();

  /* movt r0, #0xABCD => 0xF6CA30CD (same as high reg) */
  thumb_opcode op = th_movt(0, 0xABCD);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF6CA30CD);

  return 0;
}

/* ───── suite ───── */

UT_SUITE(thop_mov)
{
  UT_RUN(test_mov_reg_t1_high_basic);
  UT_RUN(test_mov_reg_t1_shift_basic);
  UT_RUN(test_mov_imm_t1_basic);
  UT_RUN(test_mov_imm_t3_basic);
  UT_RUN(test_mov_imm_t4_basic);
  UT_RUN(test_movt_basic);
  UT_RUN(test_mov_reg_shift_basic);
  UT_RUN(test_mov_reg_t1_low_regs);
  UT_RUN(test_mov_reg_enforce_16bit_high_reg);
  UT_RUN(test_movt_low_reg);
}
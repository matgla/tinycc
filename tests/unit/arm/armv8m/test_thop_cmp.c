/*
 *  test_thop_cmp.c - suite for arch/arm/thumb/thop_cmp.c
 *
 *  Tests CMP, CMN, TST, TEQ (T16 & T32 variants)
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_cmp.h"
#include "arch/arm/thumb/thumb.h"
#include "ut.h"

/* ------------------------------------------------------------------ setup */

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

/* ------------------------------------------------------------------ CMP/CMN/TST/TEQ T16 */

UT_TEST(test_th_cmp_imm_t16)
{
  setup_armv7m();

  /* CMP R0, #0xFF => 0x28FF */
  thumb_opcode op = th_cmp_imm(R0, 0xFF, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x28FF);

  return 0;
}

UT_TEST(test_th_cmp_reg_t1)
{
  setup_armv7m();

  /* CMP R0, R1 => 0x4288 (base 0x4280 | rm<<3 | rn = 0x4280 | 0x8 | 0 = 0x4288) */
  thumb_opcode op = th_cmp_reg(0, R0, R1, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4288);

  return 0;
}

UT_TEST(test_th_cmp_reg_t2)
{
  setup_armv7m();

  /* CMP R8, R9 => 0x45C8 (per manual calculation in doc) */
  thumb_opcode op = th_cmp_reg(0, R8, R9, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x45C8);

  return 0;
}

/* ------------------------------------------------------------------ CMP/CMN/TST/TEQ T32 */

UT_TEST(test_th_cmp_imm_t32)
{
  setup_armv7m();

  /* CMP R1, #0xFF000000 => size 4 */
  thumb_opcode op = th_cmp_imm(R1, 0xFF000000, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);

  return 0;
}

UT_TEST(test_th_tst_imm_t32)
{
  setup_armv7m();

  /* TST R1, #0xFF => size 4 */
  thumb_opcode op = th_tst_imm(R1, 0xFF, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);

  return 0;
}

UT_TEST(test_th_cmp_reg_t32)
{
  setup_armv7m();

  /* CMP R1, R2 with LSL #1 => size 4 */
  thumb_opcode op = th_cmp_reg(0, R1, R2, FLAGS_BEHAVIOUR_SET,
                                (thumb_shift){THUMB_SHIFT_LSL, 1, THUMB_SHIFT_IMMEDIATE},
                                ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);

  return 0;
}

UT_TEST(test_th_teq_reg_t32)
{
  setup_armv7m();

  /* TEQ R1, R2 => 0xEA910F02 */
  thumb_opcode op = th_teq_reg(R1, R2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEA910F02);

  return 0;
}

/* ------------------------------------------------------------------ CMN register */

UT_TEST(test_th_cmn_reg_t16)
{
  setup_armv7m();

  /* CMN R1, R2 => 0x42D1 */
  thumb_opcode op = th_cmn_reg(R1, R2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x42D1);

  return 0;
}

UT_TEST(test_th_cmn_reg_t32_with_shift)
{
  setup_armv7m();

  /* CMN R1, R2, LSR #1 => 0xEB110F52 */
  thumb_opcode op = th_cmn_reg(R1, R2, FLAGS_BEHAVIOUR_SET,
                                (thumb_shift){THUMB_SHIFT_LSR, 1, THUMB_SHIFT_IMMEDIATE},
                                ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEB110F52);

  return 0;
}

/* ------------------------------------------------------------------ TST register */

UT_TEST(test_th_tst_reg_t16)
{
  setup_armv7m();

  /* TST R2, R3 => 0x421A */
  thumb_opcode op = th_tst_reg(R2, R3, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x421A);

  return 0;
}

UT_TEST(test_th_tst_reg_t32)
{
  setup_armv7m();

  /* TST R2, R3 with LSL #1 => T32 0xEA120F43 */
  thumb_opcode op = th_tst_reg(R2, R3, FLAGS_BEHAVIOUR_SET,
                                (thumb_shift){THUMB_SHIFT_LSL, 1, THUMB_SHIFT_IMMEDIATE},
                                ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEA120F43);

  return 0;
}

/* ------------------------------------------------------------------ TST/TST CMN immediate T32 - exact opcodes */

UT_TEST(test_th_cmn_imm_t32)
{
  setup_armv7m();

  /* CMN R1, #0x10 => 0xF1110F10 */
  thumb_opcode op = th_cmn_imm(R1, 0x10, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF1110F10);

  return 0;
}

UT_TEST(test_th_tst_imm_t32_exact)
{
  setup_armv7m();

  /* TST R2, #1 => 0xF0120F01 */
  thumb_opcode op = th_tst_imm(R2, 0x01, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF0120F01);

  return 0;
}

/* ------------------------------------------------------------------ TEQ register T32 - additional variants */

UT_TEST(test_th_teq_reg_t32_no_shift)
{
  setup_armv7m();

  /* TEQ R1, R2 => 0xEA910F02 */
  thumb_opcode op = th_teq_reg(R1, R2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEA910F02);

  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(thop_cmp)
{
  UT_RUN(test_th_cmp_imm_t16);
  UT_RUN(test_th_cmp_reg_t1);
  UT_RUN(test_th_cmp_reg_t2);
  UT_RUN(test_th_cmp_imm_t32);
  UT_RUN(test_th_tst_imm_t32);
  UT_RUN(test_th_cmp_reg_t32);
  UT_RUN(test_th_teq_reg_t32);
  UT_RUN(test_th_teq_reg_t32_no_shift);
  UT_RUN(test_th_cmn_reg_t16);
  UT_RUN(test_th_cmn_reg_t32_with_shift);
  UT_RUN(test_th_tst_reg_t16);
  UT_RUN(test_th_tst_reg_t32);
  UT_RUN(test_th_cmn_imm_t32);
  UT_RUN(test_th_tst_imm_t32_exact);
}

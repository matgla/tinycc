/*
 *  test_thop_branch.c - suite for arch/arm/thumb/thop_branch.c
 *
 *  Tests BX/BLX reg, BL (T32 custom emit), B conditional (T16/T3),
 *  B unconditional (T2/T4), and CBZ/CBNZ (T16 custom emit).
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_branch.h"
#include "source/backend/arch/arm/thumb/thumb.h"

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

static void setup_no_cbz(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m0",
      .feat = (thop_feat){.t16 = 1},
      .is_secure_tz = false,
  };
}

/* ------------------------------------------------------------------ BX */

UT_TEST(test_bx_reg_basic)
{
  setup_armv7m();

  /* T16: bx lr => 0x4700 | (14<<3) = 0x4770 */
  thumb_opcode op = th_bx_reg(R_LR);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4770);

  /* T16: bx r0 => 0x4700 | (0<<3) = 0x4700 */
  op = th_bx_reg(R0);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4700);

  return 0;
}

/* ------------------------------------------------------------------ BLX reg */

UT_TEST(test_blx_reg_basic)
{
  setup_armv7m();

  /* T16: blx lr => 0x4780 | (14<<3) = 0x47F0 */
  thumb_opcode op = th_blx_reg(R_LR);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x47F0);

  /* T16: blx r3 => 0x4780 | (3<<3) = 0x4798 */
  op = th_blx_reg(R3);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4798);

  return 0;
}

/* ------------------------------------------------------------------ BL */

UT_TEST(test_bl_t1_zero_offset)
{
  setup_armv7m();

  /* BL with imm=0 => hi=0xF000, lo=0xF800 => 0xF000F800 */
  thumb_opcode op = th_bl_t1(0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF000F800);

  return 0;
}

UT_TEST(test_bl_t1_all_bits_set)
{
  setup_armv7m();

  /* BL with imm=0x1FFCFFC (s=1, imm10=0x3FF, j1=1, j2=1, imm11=0x7FE)
   * => hi=0xF7FF, lo=0xFFFE => 0xF7FFFFFE
   */
  thumb_opcode op = th_bl_t1(0x1FFCFFC);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF7FFFFFE);

  return 0;
}

/* ------------------------------------------------------------------ B cond T16 */

UT_TEST(test_b_t1_conditional)
{
  setup_armv7m();

  /* T16: cond=0x0 (EQ), imm=0x10
   * => 0xD000 | (0x0<<8) | 0x10 = 0xD010
   */
  thumb_opcode op = th_b_t1(0x0, 0x10);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xD010);

  /* T16: cond=0xE (AL), imm=0x20
   * => 0xD000 | (0xE<<8) | 0x20 = 0xDE20
   */
  op = th_b_t1(0xE, 0x20);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xDE20);

  return 0;
}

/* ------------------------------------------------------------------ B cond T3 */

UT_TEST(test_b_t3_conditional_zero_imm)
{
  setup_armv7m();

  /* T3: cond=0xC (GT), imm=0
   * => 0xF0008000 | (0xC<<22) = 0xF3008000
   */
  thumb_opcode op = th_b_t3(0xC, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3008000);

  return 0;
}

UT_TEST(test_b_t3_conditional_nonzero_imm)
{
  setup_armv7m();

  /* T3: cond=0xC (GT), imm=0x12343
   * Verified against arm-none-eabi-as for bgt.w .+0x2468a
   * => 0xF3248343
   */
  thumb_opcode op = th_b_t3(0xC, 0x12343);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF3248343);

  return 0;
}

/* ------------------------------------------------------------------ B T4 (unconditional wide) */

UT_TEST(test_b_t4_zero_offset)
{
  setup_armv7m();

  /* T4: imm=0 => hi=0xF000, lo=0xB800 => 0xF000B800 */
  thumb_opcode op = th_b_t4(0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF000B800);

  return 0;
}

/* ------------------------------------------------------------------ B T2 (unconditional narrow) */

UT_TEST(test_b_t2_basic)
{
  setup_armv7m();

  /* T2: imm=0x200 => i=0x100 => 0xE000 | 0x100 = 0xE100 */
  thumb_opcode op = th_b_t2(0x200);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xE100);

  /* T2: imm=0x400 => i=0x200 => 0xE000 | 0x200 = 0xE200 */
  op = th_b_t2(0x400);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xE200);

  return 0;
}

UT_TEST(test_b_t2_out_of_range)
{
  setup_armv7m();

  /* T2: imm=0x1000 => i=0x800 = 2048 > 1023 => fails */
  thumb_opcode op = th_b_t2(0x1000);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  /* T2: imm=-0x1000 => i=-0x800 = -2048 < -1024 => fails */
  op = th_b_t2(-0x1000);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

/* ------------------------------------------------------------------ CBZ / CBNZ */

UT_TEST(test_cbz_basic)
{
  setup_armv7m();

  /* CBZ: rn=0, imm=0x42 => i=0, imm5=1 => 0xB100 | 0 | (1<<3) | 0 = 0xB108 */
  thumb_opcode op = th_cbz(0, 0x42, 0);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xB108);

  return 0;
}

UT_TEST(test_cbnz_basic)
{
  setup_armv7m();

  /* CBNZ: rn=1, imm=0x42 => i=0, imm5=1 => 0xB900 | 0 | (1<<3) | 1 = 0xB909 */
  thumb_opcode op = th_cbz(1, 0x42, 1);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xB909);

  return 0;
}

UT_TEST(test_cbz_various_offsets)
{
  setup_armv7m();

  /* CBZ: rn=0, imm=0x40 => i=0, imm5=0 => 0xB100 */
  thumb_opcode op = th_cbz(0, 0x40, 0);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xB100);

  /* CBZ: rn=7, imm=0x42 => i=0, imm5=1 => 0xB10F */
  op = th_cbz(7, 0x42, 0);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xB10F);

  /* CBNZ: rn=0, imm=0x40 => i=0, imm5=0 => 0xB900 */
  op = th_cbz(0, 0x40, 1);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xB900);

  return 0;
}

UT_TEST(test_cbz_blocked_without_feat)
{
  setup_no_cbz();

  /* CBZ requires .cbz=1; cortex-m0 has only .t16=1 */
  thumb_opcode op = th_cbz(0, 0x42, 0);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

UT_TEST(test_cbnz_blocked_without_feat)
{
  setup_no_cbz();

  /* CBNZ also requires .cbz=1 */
  thumb_opcode op = th_cbz(1, 0x42, 1);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

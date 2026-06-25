/*
 *  test_thop_ldrd.c - suite for arch/arm/thumb/thop_ldrd.c LDRD/STRD encoding
 *
 *  Tests LDRD and STRD with immediate offset (T32 only). All expected
 *  opcodes verified against arm-none-eabi-as output.
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_ldrd.h"
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

/* ------------------------------------------------------------------ LDRD tests */

/* Arguments mirror what arm-thumb-asm.c passes to the wrapper */
UT_TEST(test_ldrd_pre_indexed_add)
{
  setup_armv7m();

  /* ldrd r0, r1, [r2, #4]  => 0xE9D20101  (GCC: e9d2 0101) */
  thumb_opcode op = th_ldrd_imm(0, 1, 2, 4, 0x6);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9D20101);

  /* ldrd r3, r4, [r5, #32] => 0xE9D53408  (GCC: e9d5 3408) */
  op = th_ldrd_imm(3, 4, 5, 32, 0x6);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9D53408);

  return 0;
}

UT_TEST(test_ldrd_pre_indexed_sub)
{
  setup_armv7m();

  /* ldrd r0, r1, [r2, #-4]  => 0xE9520101  (GCC: e952 0101) */
  thumb_opcode op = th_ldrd_imm(0, 1, 2, 4, 0x4);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9520101);

  /* ldrd r3, r4, [r5, #-32] => 0xE9553408  (GCC: e955 3408) */
  op = th_ldrd_imm(3, 4, 5, 32, 0x4);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9553408);

  return 0;
}

UT_TEST(test_ldrd_writeback)
{
  setup_armv7m();

  /* ldrd r0, r1, [r2, #4]!  => 0xE9F20101  (GCC: e9f2 0101) */
  thumb_opcode op = th_ldrd_imm(0, 1, 2, 4, 0x7);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9F20101);

  /* ldrd r1, r2, [r3, #32]! => 0xE9F31208  (GCC: e9f3 1208) */
  op = th_ldrd_imm(1, 2, 3, 32, 0x7);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9F31208);

  return 0;
}

UT_TEST(test_ldrd_post_indexed)
{
  setup_armv7m();

  /* ldrd r0, r1, [r2], #4  => 0xE8F20101  (GCC: e8f2 0101) */
  thumb_opcode op = th_ldrd_imm(0, 1, 2, 4, 0x3);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8F20101);

  /* ldrd r10, r11, [r3], #8 => 0xE8F3AB02  (GCC: e8f3 ab02) */
  op = th_ldrd_imm(10, 11, 3, 8, 0x3);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8F3AB02);

  return 0;
}

UT_TEST(test_ldrd_post_indexed_neg)
{
  setup_armv7m();

  /* ldrd r0, r1, [r2], #-4  => 0xE8720101  (GCC: e872 0101) */
  thumb_opcode op = th_ldrd_imm(0, 1, 2, 4, 0x1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8720101);

  /* ldrd r2, r3, [r0], #-8  => 0xE8702302  (GCC: e870 2302) */
  op = th_ldrd_imm(2, 3, 0, 8, 0x1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8702302);

  return 0;
}

UT_TEST(test_ldrd_zero_offset)
{
  setup_armv7m();

  /* ldrd r0, r1, [r2]      => 0xE9D20100  (GCC: e9d2 0100) */
  thumb_opcode op = th_ldrd_imm(0, 1, 2, 0, 0x6);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9D20100);

  /* ldrd r10, r11, [r3]    => 0xE9D3AB00  (GCC: e9d3 ab00) */
  op = th_ldrd_imm(10, 11, 3, 0, 0x6);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9D3AB00);

  return 0;
}

UT_TEST(test_ldrd_max_offset)
{
  setup_armv7m();

  /* ldrd r12, r14, [r11, #1020] => 0xE9DBCEFF  (GCC: e9db ceff) */
  thumb_opcode op = th_ldrd_imm(12, 14, 11, 1020, 0x6);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9DBCEFF);

  return 0;
}

/* ------------------------------------------------------------------ STRD tests */

UT_TEST(test_strd_pre_indexed_add)
{
  setup_armv7m();

  /* strd r0, r1, [r2, #4]  => 0xE9C20101  (GCC: e9c2 0101) */
  thumb_opcode op = th_strd_imm(0, 1, 2, 4, 0x6);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9C20101);

  /* strd r3, r4, [r5, #32] => 0xE9C53408  (GCC: e9c5 3408) */
  op = th_strd_imm(3, 4, 5, 32, 0x6);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9C53408);

  return 0;
}

UT_TEST(test_strd_pre_indexed_sub)
{
  setup_armv7m();

  /* strd r0, r1, [r2, #-4]  => 0xE9420101  (GCC: e942 0101) */
  thumb_opcode op = th_strd_imm(0, 1, 2, 4, 0x4);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9420101);

  /* strd r4, r5, [r3, #-8] => 0xE9434502  (GCC: e943 4502) */
  op = th_strd_imm(4, 5, 3, 8, 0x4);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9434502);

  return 0;
}

UT_TEST(test_strd_writeback)
{
  setup_armv7m();

  /* strd r0, r1, [r2, #4]!  => 0xE9E20101  (GCC: e9e2 0101) */
  thumb_opcode op = th_strd_imm(0, 1, 2, 4, 0x7);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9E20101);

  /* strd r3, r4, [r5, #-32]! => 0xE9653408  (GCC: e965 3408) */
  op = th_strd_imm(3, 4, 5, 32, 0x5);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9653408);

  return 0;
}

UT_TEST(test_strd_post_indexed)
{
  setup_armv7m();

  /* strd r0, r1, [r2], #4  => 0xE8E20101  (GCC: e8e2 0101) */
  thumb_opcode op = th_strd_imm(0, 1, 2, 4, 0x3);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8E20101);

  /* strd r10, r11, [r3], #8 => 0xE8E3AB02  (GCC: e8e3 ab02) */
  op = th_strd_imm(10, 11, 3, 8, 0x3);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8E3AB02);

  return 0;
}

UT_TEST(test_strd_post_indexed_neg)
{
  setup_armv7m();

  /* strd r0, r1, [r2], #-4  => 0xE8620101  (GCC: e862 0101) */
  thumb_opcode op = th_strd_imm(0, 1, 2, 4, 0x1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8620101);

  /* strd r2, r3, [r0], #-8  => 0xE8602302  (GCC: e860 2302) */
  op = th_strd_imm(2, 3, 0, 8, 0x1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8602302);

  return 0;
}

UT_TEST(test_strd_zero_offset)
{
  setup_armv7m();

  /* strd r0, r1, [r2]      => 0xE9C20100  (GCC: e9c2 0100) */
  thumb_opcode op = th_strd_imm(0, 1, 2, 0, 0x6);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9C20100);

  /* strd r10, r11, [r3]    => 0xE9C3AB00  (GCC: e9c3 ab00) */
  op = th_strd_imm(10, 11, 3, 0, 0x6);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9C3AB00);

  return 0;
}

UT_TEST(test_strd_max_offset)
{
  setup_armv7m();

  /* strd r12, r14, [r11, #1020] => 0xE9CBCEFF  (GCC: e9cb ceff) */
  thumb_opcode op = th_strd_imm(12, 14, 11, 1020, 0x6);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE9CBCEFF);

  return 0;
}

/* ------------------------------------------------------------------ imm scaling */

UT_TEST(test_imm_scaling)
{
  setup_armv7m();

  /* Wrapper scales imm by >>2: imm=4  => encoded 1 */
  thumb_opcode op = th_ldrd_imm(0, 1, 2, 4, 0x6);
  UT_ASSERT_EQ(op.opcode & 0xFF, 1);

  /* imm=1020 => encoded 0xFF */
  op = th_ldrd_imm(0, 1, 2, 1020, 0x6);
  UT_ASSERT_EQ(op.opcode & 0xFF, 0xFF);

  /* imm=0 => encoded 0 */
  op = th_ldrd_imm(0, 1, 2, 0, 0x6);
  UT_ASSERT_EQ(op.opcode & 0xFF, 0);

  return 0;
}

/* ------------------------------------------------------------------ base opcode difference */

UT_TEST(test_ldrd_vs_strd_base)
{
  setup_armv7m();

  /* Same registers/imm/puw: LDRD base 0xE85..., STRD base 0xE84... */
  thumb_opcode l = th_ldrd_imm(0, 1, 2, 4, 0x6);
  thumb_opcode s = th_strd_imm(0, 1, 2, 4, 0x6);

  /* LDRD has bit 24 set (0x1000000 more than STRD) */
  UT_ASSERT_EQ(l.opcode - s.opcode, 0x100000);

  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(thop_ldrd)
{
  UT_RUN(test_ldrd_pre_indexed_add);
  UT_RUN(test_ldrd_pre_indexed_sub);
  UT_RUN(test_ldrd_writeback);
  UT_RUN(test_ldrd_post_indexed);
  UT_RUN(test_ldrd_post_indexed_neg);
  UT_RUN(test_ldrd_zero_offset);
  UT_RUN(test_ldrd_max_offset);
  UT_RUN(test_strd_pre_indexed_add);
  UT_RUN(test_strd_pre_indexed_sub);
  UT_RUN(test_strd_writeback);
  UT_RUN(test_strd_post_indexed);
  UT_RUN(test_strd_post_indexed_neg);
  UT_RUN(test_strd_zero_offset);
  UT_RUN(test_strd_max_offset);
  UT_RUN(test_imm_scaling);
  UT_RUN(test_ldrd_vs_strd_base);
}

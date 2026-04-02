/*
 *  test_thop_ldrex.c - suite for arch/arm/thumb/thop_ldrex.c
 *  LDREX/STREX/LDREXB/LDREXH/STREXB/STREXH encoding (ARMv8-M)
 *
 *  All expected opcodes verified against arm-none-eabi-as output.
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_ldrex.h"
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

/* ───── LDREX (T32) ───── */

UT_TEST(test_ldrex_basic)
{
  setup_armv8m();

  /* ldrex r8, [r1, #4]  => 0xE8518F01  (GCC: e851 8f01) */
  thumb_opcode op = th_ldrex(8, 1, 4);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8518F01);

  /* ldrex r9, [r10, #8] => 0xE85A9F02  (GCC: e85a 9f02) */
  op = th_ldrex(9, 10, 8);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE85A9F02);

  /* ldrex r3, [r5, #0]  => 0xE8553F00  (GCC: e855 3f00) */
  op = th_ldrex(3, 5, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8553F00);

  return 0;
}

/* ───── STREX (T32) ───── */

UT_TEST(test_strex_basic)
{
  setup_armv8m();

  /* strex r0, r8, [r1, #8]  => 0xE8418002  (GCC: e841 8002) */
  thumb_opcode op = th_strex(0, 8, 1, 8);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8418002);

  /* strex r3, r5, [r9, #4]  => 0xE8495301  (GCC: e849 5301) */
  op = th_strex(3, 5, 9, 4);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8495301);

  /* strex r12, r0, [r6, #0] => 0xE8460C00  (GCC: e846 0c00) */
  op = th_strex(12, 0, 6, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8460C00);

  return 0;
}

/* ───── LDREXB (T32) ───── */

UT_TEST(test_ldrexb_basic)
{
  setup_armv8m();

  /* ldrexb r4, [r2]  => 0xE8D24F4F  (GCC: e8d2 4f4f) */
  thumb_opcode op = th_ldrexb(4, 2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D24F4F);

  /* ldrexb r7, [r11] => 0xE8DB7F4F  (GCC: e8db 7f4f) */
  op = th_ldrexb(7, 11);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8DB7F4F);

  return 0;
}

/* ───── LDREXH (T32) ───── */

UT_TEST(test_ldrexh_basic)
{
  setup_armv8m();

  /* ldrexh r4, [r2]  => 0xE8D24F5F  (GCC: e8d2 4f5f) */
  thumb_opcode op = th_ldrexh(4, 2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D24F5F);

  /* ldrexh r10, [r7] => 0xE8D7AF5F  (GCC: e8d7 af5f) */
  op = th_ldrexh(10, 7);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D7AF5F);

  return 0;
}

/* ───── STREXB (T32) ───── */

UT_TEST(test_strexb_basic)
{
  setup_armv8m();

  /* strexb r0, r6, [r3]  => 0xE8C36F40  (GCC: e8c3 6f40) */
  thumb_opcode op = th_strexb(0, 6, 3);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C36F40);

  /* strexb r4, r11, [r6] => 0xE8C6BF44  (GCC: e8c6 bf44) */
  op = th_strexb(4, 11, 6);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C6BF44);

  return 0;
}

/* ───── STREXH (T32) ───── */

UT_TEST(test_strexh_basic)
{
  setup_armv8m();

  /* strexh r0, r6, [r3]  => 0xE8C36F50  (GCC: e8c3 6f50) */
  thumb_opcode op = th_strexh(0, 6, 3);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C36F50);

  /* strexh r7, r12, [r3] => 0xE8C3CF57  (GCC: e8c3 cf57) */
  op = th_strexh(7, 12, 3);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C3CF57);

  return 0;
}

/* ───── Verify size=4 for all T32 forms ───── */

UT_TEST(test_ldrex_all_size_4)
{
  setup_armv8m();

  UT_ASSERT_EQ(th_ldrex(0, 0, 0).size, 4);
  UT_ASSERT_EQ(th_strex(0, 0, 0, 0).size, 4);
  UT_ASSERT_EQ(th_ldrexb(0, 0).size, 4);
  UT_ASSERT_EQ(th_ldrexh(0, 0).size, 4);
  UT_ASSERT_EQ(th_strexb(0, 0, 0).size, 4);
  UT_ASSERT_EQ(th_strexh(0, 0, 0).size, 4);

  return 0;
}

/* ───── Register encoding: base opcode differences ───── */

UT_TEST(test_ldrex_vs_strex_base_diff)
{
  setup_armv8m();

  /* LDREX base: 0xE8500F00  |  STREX base: 0xE8400000
   * Bits 20-23: LDREX has 0x5, STREX has 0x4 (diff = 0x100000) */
  thumb_opcode l = th_ldrex(0, 1, 0);
  thumb_opcode s = th_strex(0, 0, 1, 0);

  uint32_t diff = (l.opcode & 0x00F00000) - (s.opcode & 0x00F00000);
  UT_ASSERT_EQ(diff, 0x100000);

  return 0;
}

UT_TEST(test_ldrexb_vs_strexb_base_diff)
{
  setup_armv8m();

  /* LDREXB base: 0xE8D00F4F  |  STREXB base: 0xE8C00F40
   * Bits 20-23: LDREXB has 0xD, STREXB has 0xC (diff = 0x100000) */
  thumb_opcode l = th_ldrexb(0, 1);
  thumb_opcode s = th_strexb(0, 0, 1);

  uint32_t diff = (l.opcode & 0x00F00000) - (s.opcode & 0x00F00000);
  UT_ASSERT_EQ(diff, 0x100000);

  return 0;
}

UT_TEST(test_ldrexh_vs_strexh_base_diff)
{
  setup_armv8m();

  /* LDREXH base: 0xE8D00F5F  |  STREXH base: 0xE8C00F50
   * Bits 20-23: LDREXH has 0xD, STREXH has 0xC (diff = 0x100000) */
  thumb_opcode l = th_ldrexh(0, 1);
  thumb_opcode s = th_strexh(0, 0, 1);

  uint32_t diff = (l.opcode & 0x00F00000) - (s.opcode & 0x00F00000);
  UT_ASSERT_EQ(diff, 0x100000);

  return 0;
}

/* ───── suite ───── */

UT_SUITE(thop_ldrex)
{
  UT_RUN(test_ldrex_basic);
  UT_RUN(test_strex_basic);
  UT_RUN(test_ldrexb_basic);
  UT_RUN(test_ldrexh_basic);
  UT_RUN(test_strexb_basic);
  UT_RUN(test_strexh_basic);
  UT_RUN(test_ldrex_all_size_4);
  UT_RUN(test_ldrex_vs_strex_base_diff);
  UT_RUN(test_ldrexb_vs_strexb_base_diff);
  UT_RUN(test_ldrexh_vs_strexh_base_diff);
}
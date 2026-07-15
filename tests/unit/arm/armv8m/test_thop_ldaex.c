/*
 *  test_thop_ldaex.c - suite for arch/arm/thumb/thop_ldaex.c
 *  LDAEX/LDAEXB/LDAEXH/STLEX/STLEXB/STLEXH encoding (ARMv8-M)
 *
 *  All expected opcodes verified against arm-none-eabi-as output.
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_ldaex.h"
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

/* ───── LDAEX (T32) ───── */

UT_TEST(test_ldaex_basic)
{
  setup_armv8m();

  /* ldaex r8, [r1]  => 0xE8D18FEF  (GCC: e8d1 8fef) */
  thumb_opcode op = th_ldaex(8, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D18FEF);

  /* ldaex r9, [r10] => 0xE8DA9FEF  (GCC: e8da 9fef) */
  op = th_ldaex(9, 10);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8DA9FEF);

  /* ldaex r1, [r11] => 0xE8DB1FEF  (GCC: e8db 1fef) */
  op = th_ldaex(1, 11);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8DB1FEF);

  return 0;
}

/* ───── LDAEXB (T32) ───── */

UT_TEST(test_ldaexb_basic)
{
  setup_armv8m();

  /* ldaexb r5, [r3]  => 0xE8D35FCF  (GCC: e8d3 5fcf) */
  thumb_opcode op = th_ldaexb(5, 3);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D35FCF);

  /* ldaexb r0, [r12] => 0xE8DC0FCF  (GCC: e8dc 0fcf) */
  op = th_ldaexb(0, 12);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8DC0FCF);

  return 0;
}

/* ───── LDAEXH (T32) ───── */

UT_TEST(test_ldaexh_basic)
{
  setup_armv8m();

  /* ldaexh r4, [r2]  => 0xE8D24FDF  (GCC: e8d2 4fdf) */
  thumb_opcode op = th_ldaexh(4, 2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D24FDF);

  /* ldaexh r10, [r7] => 0xE8D7AFDF  (GCC: e8d7 afdf) */
  op = th_ldaexh(10, 7);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D7AFDF);

  return 0;
}

/* ───── STLEX (T32) ───── */

UT_TEST(test_stlex_basic)
{
  setup_armv8m();

  /* stlex r0, r8, [r1]  => 0xE8C18FE0  (GCC: e8c1 8fe0) */
  thumb_opcode op = th_stlex(0, 8, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C18FE0);

  /* stlex r3, r5, [r9]  => 0xE8C95FE3  (GCC: e8c9 5fe3) */
  op = th_stlex(3, 5, 9);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C95FE3);

  return 0;
}

/* ───── STLEXB (T32) ───── */

UT_TEST(test_stlexb_basic)
{
  setup_armv8m();

  /* stlexb r0, r8, [r1]  => 0xE8C18FC0  (GCC: e8c1 8fc0) */
  thumb_opcode op = th_stlexb(0, 8, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C18FC0);

  /* stlexb r4, r11, [r6]  => 0xE8C6BFC4  (GCC: e8c6 bfc4) */
  op = th_stlexb(4, 11, 6);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C6BFC4);

  return 0;
}

/* ───── STLEXH (T32) ───── */

UT_TEST(test_stlexh_basic)
{
  setup_armv8m();

  /* stlexh r2, r4, [r5]  => 0xE8C54FD2  (GCC: e8c5 4fd2) */
  thumb_opcode op = th_stlexh(2, 4, 5);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C54FD2);

  /* stlexh r7, r12, [r3]  => 0xE8C3CFD7  (GCC: e8c3 cfd7) */
  op = th_stlexh(7, 12, 3);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C3CFD7);

  return 0;
}

/* ───── Feature gate: ldaex=0 blocks all ───── */

UT_TEST(test_ldaex_feature_gate_off)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m3",
      .feat = (thop_feat){
          .t16 = 1,
          .t32 = 1,
          .ldaex = 0,
      },
      .is_secure_tz = false,
  };

  thumb_opcode op = th_ldaex(8, 1);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  op = th_stlex(0, 8, 1);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  op = th_ldaexb(5, 3);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  op = th_ldaexh(4, 2);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

/* ───── Verify size=4 for all T32 forms ───── */

UT_TEST(test_ldaex_all_size_4)
{
  setup_armv8m();

  UT_ASSERT_EQ(th_ldaex(0, 0).size, 4);
  UT_ASSERT_EQ(th_ldaexb(0, 0).size, 4);
  UT_ASSERT_EQ(th_ldaexh(0, 0).size, 4);
  UT_ASSERT_EQ(th_stlex(0, 0, 0).size, 4);
  UT_ASSERT_EQ(th_stlexb(0, 0, 0).size, 4);
  UT_ASSERT_EQ(th_stlexh(0, 0, 0).size, 4);

  return 0;
}

/* ───── Register encoding: base opcode differences ───── */

UT_TEST(test_ldaex_vs_stlex_base_diff)
{
  setup_armv8m();

  /* LDAEX and STLEX share same register layout but different base opcode
   * LDAEX base: 0xE8D00FEF  |  STLEX base: 0xE8C00FE0
   * Difference: 0x1000F (bit 20 = D vs C) */
  thumb_opcode l = th_ldaex(0, 1);
  thumb_opcode s = th_stlex(0, 0, 1);

  uint32_t diff = (l.opcode & 0x00F00000) - (s.opcode & 0x00F00000);
  UT_ASSERT_EQ(diff, 0x100000);

  return 0;
}

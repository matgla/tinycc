/*
 *  test_thop_mem_exclusive.c - suite for arch/arm/thumb/thop_mem_exclusive.c
 *  LDA/LDAB/LDAH/STL/STLB/STLH encoding (ARMv8-M)
 *
 *  All expected opcodes verified against arm-none-eabi-as output.
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_mem_exclusive.h"
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

/* ───── LDA (T32) ───── */

UT_TEST(test_lda_basic)
{
  setup_armv8m();

  /* lda r8, [r1]  => 0xE8D18FAF  (GCC: e8d1 8faf) */
  thumb_opcode op = th_lda(8, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D18FAF);

  /* lda r0, [r2]  => 0xE8D20FAF  (GCC: e8d2 0faf) */
  op = th_lda(0, 2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D20FAF);

  /* lda r12, [r7] => 0xE8D7CFAF  (GCC: e8d7 cfaf) */
  op = th_lda(12, 7);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D7CFAF);

  return 0;
}

/* ───── LDAB (T32) ───── */

UT_TEST(test_ldab_basic)
{
  setup_armv8m();

  /* ldab r8, [r1]  => 0xE8D18F8F  (GCC: e8d1 8f8f) */
  thumb_opcode op = th_ldab(8, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D18F8F);

  /* ldab r3, [r5]  => 0xE8D53F8F  (GCC: e8d5 3f8f) */
  op = th_ldab(3, 5);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D53F8F);

  return 0;
}

/* ───── LDAH (T32) ───── */

UT_TEST(test_ldah_basic)
{
  setup_armv8m();

  /* ldah r8, [r1]  => 0xE8D18F9F  (GCC: e8d1 8f9f) */
  thumb_opcode op = th_ldah(8, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D18F9F);

  /* ldah r10, [r6] => 0xE8D6AF9F  (GCC: e8d6 af9f) */
  op = th_ldah(10, 6);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D6AF9F);

  return 0;
}

/* ───── STL (T32) ───── */

UT_TEST(test_stl_basic)
{
  setup_armv8m();

  /* stl r8, [r1]  => 0xE8C18FAF  (GCC: e8c1 8faf) */
  thumb_opcode op = th_stl(8, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C18FAF);

  /* stl r0, [r2]  => 0xE8C20FAF  (GCC: e8c2 0faf) */
  op = th_stl(0, 2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C20FAF);

  return 0;
}

/* ───── STLB (T32) ───── */

UT_TEST(test_stlb_basic)
{
  setup_armv8m();

  /* stlb r0, [r2]  => 0xE8C20F8F  (GCC: e8c2 0f8f) */
  thumb_opcode op = th_stlb(0, 2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C20F8F);

  /* stlb r8, [r1]  => 0xE8C18F8F  (GCC: e8c1 8f8f) */
  op = th_stlb(8, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C18F8F);

  return 0;
}

/* ───── STLH (T32) ───── */

UT_TEST(test_stlh_basic)
{
  setup_armv8m();

  /* stlh r8, [r1]  => 0xE8C18F9F  (GCC: e8c1 8f9f) */
  thumb_opcode op = th_stlh(8, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C18F9F);

  /* stlh r5, [r4]  => 0xE8C45F9F  (GCC: e8c4 5f9f) */
  op = th_stlh(5, 4);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C45F9F);

  return 0;
}

/* ───── Feature gate: ldaex=0 does NOT block these ───── */

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

  thumb_opcode op = th_lda(8, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8D18FAF);

  op = th_stl(8, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE8C18FAF);

  return 0;
}

/* ───── Verify size=4 for all T32 forms ───── */

UT_TEST(test_all_size_4)
{
  setup_armv8m();

  UT_ASSERT_EQ(th_lda(0, 0).size, 4);
  UT_ASSERT_EQ(th_ldab(0, 0).size, 4);
  UT_ASSERT_EQ(th_ldah(0, 0).size, 4);
  UT_ASSERT_EQ(th_stl(0, 0).size, 4);
  UT_ASSERT_EQ(th_stlb(0, 0).size, 4);
  UT_ASSERT_EQ(th_stlh(0, 0).size, 4);

  return 0;
}

/* ───── Base opcode differences: LDA vs STL ───── */

UT_TEST(test_lda_vs_stl_base_diff)
{
  setup_armv8m();

  /* LDA base: 0xE8D00FAF  |  STL base: 0xE8C00FAF
   * Bits 20-23: LDA has 0xD, STL has 0xC (diff = 0x100000) */
  thumb_opcode l = th_lda(0, 1);
  thumb_opcode s = th_stl(0, 1);

  uint32_t diff = (l.opcode & 0x00F00000) - (s.opcode & 0x00F00000);
  UT_ASSERT_EQ(diff, 0x100000);

  return 0;
}

UT_TEST(test_ldab_vs_stlb_base_diff)
{
  setup_armv8m();

  /* LDAB base: 0xE8D00F8F  |  STLB base: 0xE8C00F8F
   * Bits 20-23: LDAB has 0xD, STLB has 0xC (diff = 0x100000) */
  thumb_opcode l = th_ldab(0, 1);
  thumb_opcode s = th_stlb(0, 1);

  uint32_t diff = (l.opcode & 0x00F00000) - (s.opcode & 0x00F00000);
  UT_ASSERT_EQ(diff, 0x100000);

  return 0;
}

UT_TEST(test_ldah_vs_stlh_base_diff)
{
  setup_armv8m();

  /* LDAH base: 0xE8D00F9F  |  STLH base: 0xE8C00F9F
   * Bits 20-23: LDAH has 0xD, STLH has 0xC (diff = 0x100000) */
  thumb_opcode l = th_ldah(0, 1);
  thumb_opcode s = th_stlh(0, 1);

  uint32_t diff = (l.opcode & 0x00F00000) - (s.opcode & 0x00F00000);
  UT_ASSERT_EQ(diff, 0x100000);

  return 0;
}

/* ───── suite ───── */

UT_SUITE(thop_mem_exclusive)
{
  UT_RUN(test_lda_basic);
  UT_RUN(test_ldab_basic);
  UT_RUN(test_ldah_basic);
  UT_RUN(test_stl_basic);
  UT_RUN(test_stlb_basic);
  UT_RUN(test_stlh_basic);
  UT_RUN(test_ldaex_feature_gate_off);
  UT_RUN(test_all_size_4);
  UT_RUN(test_lda_vs_stl_base_diff);
  UT_RUN(test_ldab_vs_stlb_base_diff);
  UT_RUN(test_ldah_vs_stlh_base_diff);
}
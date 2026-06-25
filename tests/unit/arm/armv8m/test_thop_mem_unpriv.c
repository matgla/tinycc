/*
 *  test_thop_mem_unpriv.c - suite for arch/arm/thumb/thop_mem_unpriv.c
 *  Unprivileged load/store (T32 only)
 *
 *  All expected opcodes verified against arm-none-eabi-as output.
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_mem_unpriv.h"
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

/* ───── ldrt ───── */

UT_TEST(test_ldrt_basic)
{
  setup_armv8m();

  /* ldrt r8, [r1, #0x20] => 0xF8518E20 (GCC: f851 8e20) */
  thumb_opcode op = th_ldrt(8, 1, 0x20);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8518E20);

  /* ldrt r0, [r2, #0] => 0xF8520E00 (GCC: f852 0e00) */
  op = th_ldrt(0, 2, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8520E00);

  return 0;
}

/* ───── ldrbt ───── */

UT_TEST(test_ldrbt_basic)
{
  setup_armv8m();

  /* ldrbt r0, [r2, #0xFF] => 0xF8120EFF (GCC: f812 0eff) */
  thumb_opcode op = th_ldrbt(0, 2, 0xFF);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8120EFF);

  /* ldrbt r8, [r1, #0x10] => 0xF8118E10 (GCC: f811 8e10) */
  op = th_ldrbt(8, 1, 0x10);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8118E10);

  return 0;
}

/* ───── ldrht ───── */

UT_TEST(test_ldrht_basic)
{
  setup_armv8m();

  /* ldrht r4, [r5, #0] => 0xF8354E00 (GCC: f835 4e00) */
  thumb_opcode op = th_ldrht(4, 5, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8354E00);

  /* ldrht r8, [r1, #0x20] => 0xF8318E20 (GCC: f831 8e20) */
  op = th_ldrht(8, 1, 0x20);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8318E20);

  return 0;
}

/* ───── ldrsbt ───── */

UT_TEST(test_ldrsbt_basic)
{
  setup_armv8m();

  /* ldrsbt r8, [r1, #0x20] => 0xF9118E20 (GCC: f911 8e20) */
  thumb_opcode op = th_ldrsbt(8, 1, 0x20);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF9118E20);

  /* ldrsbt r0, [r2, #0] => 0xF9120E00 (GCC: f912 0e00) */
  op = th_ldrsbt(0, 2, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF9120E00);

  return 0;
}

/* ───── ldrsht ───── */

UT_TEST(test_ldrsht_basic)
{
  setup_armv8m();

  /* ldrsht r4, [r5, #0] => 0xF9354E00 (GCC: f935 4e00) */
  thumb_opcode op = th_ldrsht(4, 5, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF9354E00);

  /* ldrsht r8, [r1, #0x20] => 0xF9318E20 (GCC: f931 8e20) */
  op = th_ldrsht(8, 1, 0x20);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF9318E20);

  return 0;
}

/* ───── strt ───── */

UT_TEST(test_strt_basic)
{
  setup_armv8m();

  /* strt r8, [r1, #0x20] => 0xF8418E20 (GCC: f841 8e20) */
  thumb_opcode op = th_strt(8, 1, 0x20);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8418E20);

  /* strt r0, [r2, #0] => 0xF8420E00 (GCC: f842 0e00) */
  op = th_strt(0, 2, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8420E00);

  return 0;
}

/* ───── strbt ───── */

UT_TEST(test_strbt_basic)
{
  setup_armv8m();

  /* strbt r0, [r2, #0xFF] => 0xF8020EFF (GCC: f802 0eff) */
  thumb_opcode op = th_strbt(0, 2, 0xFF);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8020EFF);

  /* strbt r8, [r1, #0x10] => 0xF8018E10 (GCC: f801 8e10) */
  op = th_strbt(8, 1, 0x10);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8018E10);

  return 0;
}

/* ───── strht ───── */

UT_TEST(test_strht_basic)
{
  setup_armv8m();

  /* strht r4, [r5, #0] => 0xF8254E00 (GCC: f825 4e00) */
  thumb_opcode op = th_strht(4, 5, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8254E00);

  /* strht r8, [r1, #0x20] => 0xF8218E20 (GCC: f821 8e20) */
  op = th_strht(8, 1, 0x20);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8218E20);

  return 0;
}

/* ───── enforce-16bit fails (T32 only) ───── */

UT_TEST(test_mem_unpriv_enforce_16bit_fails)
{
  setup_armv8m();

  /* All unprivileged instructions are T32 only - enforce should fail */
  thumb_opcode op = th_ldrt(8, 1, 0x20);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF8518E20);

  return 0;
}

/* ───── suite ───── */

UT_SUITE(thop_mem_unpriv)
{
  UT_RUN(test_ldrt_basic);
  UT_RUN(test_ldrbt_basic);
  UT_RUN(test_ldrht_basic);
  UT_RUN(test_ldrsbt_basic);
  UT_RUN(test_ldrsht_basic);
  UT_RUN(test_strt_basic);
  UT_RUN(test_strbt_basic);
  UT_RUN(test_strht_basic);
  UT_RUN(test_mem_unpriv_enforce_16bit_fails);
}
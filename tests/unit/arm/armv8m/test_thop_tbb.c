/*
 *  test_thop_tbb.c - suite for arch/arm/thumb/thop_tbb.c
 *
 *  Tests TBB, TBH (halfword variant), TT with various A/T flag combinations,
 *  and feature-gate blocking when .tbb_tbh=0.
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thop_tbb.h"
#include "arch/arm/thumb/thumb.h"

#include "ut.h"

/* ------------------------------------------------------------------ setup */

static void setup_armv8m(void)
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

static void setup_no_tbb(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m0",
      .feat = (thop_feat){.t16 = 1},
      .is_secure_tz = false,
  };
}

/* ------------------------------------------------------------------ TBB */

UT_TEST(test_th_tbb_basic)
{
  setup_armv8m();

   /* TBB: rn=R0, rm=R1, h=0 => base 0xE8D0F000 | (rn<<16) | (rm<<0)
    * = 0xE8D0F000 | (0<<16) | (1<<0) = 0xE8D0F001
    */
   thumb_opcode op = th_tbb(0, 1, 0);
   UT_ASSERT_EQ(op.size, 4);
   UT_ASSERT_EQ(op.opcode, 0xE8D0F001);

  return 0;
}

UT_TEST(test_th_tbb_rn_rm_variants)
{
  setup_armv8m();

    /* TBB: rn=R7, rm=R3 => 0xE8D0F000 | (7<<16) | (3<<0) = 0xE8D7F003 */
    thumb_opcode op = th_tbb(7, 3, 0);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xE8D7F003);

    /* TBB: rn=R12, rm=R12 => 0xE8D0F000 | (12<<16) | (12<<0) = 0xE8DCF00C */
    op = th_tbb(12, 12, 0);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xE8DCF00C);


  return 0;
}

/* ------------------------------------------------------------------ TBH */

UT_TEST(test_th_tbh_basic)
{
  setup_armv8m();

    /* TBH: rn=R0, rm=R1, h=1 => base 0xE8D0F010 | (rn<<16) | (rm<<0)
     * = 0xE8D0F010 | (0<<16) | (1<<0) = 0xE8D0F011
     */
    thumb_opcode op = th_tbb(0, 1, 1);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xE8D0F011);


  return 0;
}

UT_TEST(test_th_tbh_rn_rm_variants)
{
  setup_armv8m();

    /* TBH: rn=R5, rm=R2 => 0xE8D0F010 | (5<<16) | (2<<0) = 0xE8D5F012 */
    thumb_opcode op = th_tbb(5, 2, 1);
    UT_ASSERT_EQ(op.size, 4);
    UT_ASSERT_EQ(op.opcode, 0xE8D5F012);


  return 0;
}

/* ------------------------------------------------------------------ TT */

UT_TEST(test_th_tt_basic)
{
  setup_armv8m();

  /* TT: rd=R0, rn=R1, a=0, t=0 => base 0xE840F000 | (rn<<16) | (rd<<8)
   * = 0xE840F000 | (1<<16) | (0<<8) = 0xE841F000
   */
  thumb_opcode op = th_tt(0, 1, 0, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE841F000);

  return 0;
}

UT_TEST(test_th_tt_with_a)
{
  setup_armv8m();

  /* TT: rd=R0, rn=R1, a=1, t=0 => A bit set (bit 7 = 0x80)
   * = 0xE841F000 | 0x80 = 0xE841F080
   */
  thumb_opcode op = th_tt(0, 1, 1, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE841F080);

  return 0;
}

UT_TEST(test_th_tt_with_t)
{
  setup_armv8m();

  /* TT: rd=R0, rn=R1, a=0, t=1 => T bit set (bit 6 = 0x40)
   * = 0xE841F000 | 0x40 = 0xE841F040
   */
  thumb_opcode op = th_tt(0, 1, 0, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE841F040);

  return 0;
}

UT_TEST(test_th_tt_with_a_and_t)
{
  setup_armv8m();

  /* TT: rd=R0, rn=R1, a=1, t=1 => A|T bits set = 0xE841F0C0 */
  thumb_opcode op = th_tt(0, 1, 1, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE841F0C0);

  return 0;
}

UT_TEST(test_th_tt_various_regs)
{
  setup_armv8m();

  /* TT: rd=R8, rn=R10 => 0xE840F000 | (10<<16) | (8<<8) = 0xE84AF800 */
  thumb_opcode op = th_tt(8, 10, 0, 0);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE84AF800);

  /* TT: rd=R2, rn=R1, a=1, t=1 => 0xE840F000 | (1<<16) | (2<<8) | 0xC0
   * = 0xE841F2C0
   */
  op = th_tt(2, 1, 1, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xE841F2C0);

  return 0;
}

/* ------------------------------------------------------------------ feature gate */

UT_TEST(test_th_tbb_blocked_without_feat)
{
  setup_no_tbb();

  /* TBB requires .tbb_tbh=1; cortex-m0 has only .t16=1 */
  thumb_opcode op = th_tbb(0, 1, 0);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

UT_TEST(test_th_tbh_blocked_without_feat)
{
  setup_no_tbb();

  /* TBH also requires .tbb_tbh=1 */
  thumb_opcode op = th_tbb(5, 2, 1);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(thop_tbb)
{
  UT_RUN(test_th_tbb_basic);
  UT_RUN(test_th_tbb_rn_rm_variants);
  UT_RUN(test_th_tbh_basic);
  UT_RUN(test_th_tbh_rn_rm_variants);
  UT_RUN(test_th_tt_basic);
  UT_RUN(test_th_tt_with_a);
  UT_RUN(test_th_tt_with_t);
  UT_RUN(test_th_tt_with_a_and_t);
  UT_RUN(test_th_tt_various_regs);
  UT_RUN(test_th_tbb_blocked_without_feat);
  UT_RUN(test_th_tbh_blocked_without_feat);
}

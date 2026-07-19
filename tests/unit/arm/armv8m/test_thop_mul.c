/*
 *  test_thop_mul.c - suite for arch/arm/thumb/thop_mul.c
 *
 *  Tests MUL T16 (low reg, rd==rm), MUL T32, MLA, MLS, UMULL, UMLAL,
 *  SMULL, SMLAL, UDIV, SDIV.
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_mul.h"
#include "source/backend/arch/arm/thumb/thumb.h"

#include "ut.h"

/* ------------------------------------------------------------------ setup */

static void setup_armv7m(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m3",
      .feat =
          (thop_feat){
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

static void setup_no_div(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m3",
      .feat =
          (thop_feat){
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
              .div = 0,
          },
      .is_secure_tz = false,
  };
}

/* ------------------------------------------------------------------ MUL T16 */

UT_TEST(test_mul_t16_rd0_rm0)
{
  setup_armv7m();

  /* muls r0, r1 — rd=0, rm=1 => 0x4340 | 0 | (1<<3) = 0x4348 */
  thumb_opcode op = th_mul(0, 1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4348);

  return 0;
}

UT_TEST(test_mul_t16_rd5_rm5)
{
  setup_armv7m();

  /* muls r5, r5 — rd=5, rm=5 => 0x4340 | 5 | (5<<3) = 0x436D */
  /* T16 requires rd==rm, so call th_mul(5, 5, 5) */
  thumb_opcode op = th_mul(5, 5, 5, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x436D);

  return 0;
}

/* ------------------------------------------------------------------ MUL T32 */

UT_TEST(test_mul_t32_low_regs)
{
  setup_armv7m();

  /* mul.w r1, r2, r3 — base 0xFB00F000 | rd=1<<8 | rn=2<<16 | rm=3
   * = 0xFB00F000 | 0x00000100 | 0x00020000 | 0x00000003 = 0xFB02F103 */
  thumb_opcode op = th_mul(1, 2, 3, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFB02F103);

  return 0;
}

UT_TEST(test_mul_t32_high_reg)
{
  setup_armv7m();

  /* mul.w r8, r1, r2 — rd=8, rn=1, rm=2 => base 0xFB00F000 | 8<<8 | 1<<16 | 2
   * = 0xFB00F000 | 0x00000800 | 0x00010000 | 0x00000002 = 0xFB01F802 */
  thumb_opcode op = th_mul(8, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFB01F802);

  return 0;
}

/* ------------------------------------------------------------------ MUL wrapper auto-selection */

UT_TEST(test_mul_t16_auto_selection)
{
  setup_armv7m();

  /* rd=rm=0, all low regs -> T16 */
  thumb_opcode op = th_mul(0, 1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);

  /* rd=rm=7, all low regs -> T16 */
  op = th_mul(7, 6, 7, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);

  return 0;
}

UT_TEST(test_mul_t32_auto_selection_high_reg)
{
  setup_armv7m();

  /* R8 is high reg -> falls to T32 */
  thumb_opcode op = th_mul(8, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);

  return 0;
}

UT_TEST(test_mul_t32_auto_selection_rd_ne_rm)
{
  setup_armv7m();

  /* rd != rm -> T32 even though both are low regs */
  thumb_opcode op = th_mul(0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFB01F002);

  return 0;
}

UT_TEST(test_mul_t16_commutative_rd_eq_rn)
{
  setup_armv7m();

  /* MUL is commutative, so rd == rn is encodable as T16 too: the operand that
     is not the destination plays Rn.  muls r0, r1 => 0x4348 */
  thumb_opcode op = th_mul(0, 0, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4348);

  /* muls r7, r6 => 0x4340 | 7 | (6<<3) = 0x4377 */
  op = th_mul(7, 7, 6, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x4377);

  return 0;
}

UT_TEST(test_mul_t16_rejected_when_flags_must_be_preserved)
{
  setup_armv7m();

  /* T16 MULS has an implicit S bit, so it must not be selected when the caller
     needs NZCV preserved - fall back to the flag-transparent T32 encoding. */
  thumb_opcode op = th_mul(0, 1, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFB01F000);

  op = th_mul(0, 0, 1, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFB00F001);

  return 0;
}

UT_TEST(test_mul_t32_cannot_set_flags)
{
  setup_armv7m();

  /* MULS exists only as T16; a flag-setting multiply with three distinct
     registers is not encodable and must be rejected rather than silently
     dropping the S bit. */
  thumb_opcode op = th_mul(0, 1, 2, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);

  return 0;
}

UT_TEST(test_mul_enforce_32bit_low_regs)
{
  setup_armv7m();

  /* ENFORCE_32BIT with low regs -> T32 */
  thumb_opcode op = th_mul(0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_32BIT);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFB01F002);

  return 0;
}

/* ------------------------------------------------------------------ MLA */

UT_TEST(test_mla_basic)
{
  setup_armv7m();

  /* mla r2, r3, r8, sl — rd=2, rn=3, rm=8, ra=10
   * base 0xFB000000 | rn=3<<16 | rd=2<<8 | ra=10<<12 | rm=8
   * = 0xFB000000 | 0x00030000 | 0x00000200 | 0x0000A000 | 0x00000008
   * = 0xFB03A208 */
  thumb_opcode op = th_mla(2, 3, 8, 10);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFB03A208);

  return 0;
}

/* ------------------------------------------------------------------ MLS */

UT_TEST(test_mls_basic)
{
  setup_armv7m();

  /* mls r2, r3, r8, sl — rd=2, rn=3, rm=8, ra=10
   * base 0xFB000010 | rn=3<<16 | rd=2<<8 | ra=10<<12 | rm=8
   * = 0xFB000010 | 0x00030000 | 0x00000200 | 0x0000A000 | 0x00000008
   * = 0xFB03A218 */
  thumb_opcode op = th_mls(2, 3, 8, 10);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFB03A218);

  return 0;
}

/* ------------------------------------------------------------------ UMULL */

UT_TEST(test_umull_basic)
{
  setup_armv7m();

  /* umull r0, r1, r2, r3 — rdhi=1, rn=2, rm=3, rdlo=0
   * base 0xFBA00000 | rdhi=1<<8 | rn=2<<16 | rm=3 | rdlo=0<<12
   * = 0xFBA00000 | 0x00000100 | 0x00020000 | 0x00000003 | 0
   * = 0xFBA20103 */
  thumb_opcode op = th_umull(0, 1, 2, 3);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFBA20103);

  return 0;
}

/* ------------------------------------------------------------------ UMLAL */

UT_TEST(test_umlal_basic)
{
  setup_armv7m();

  /* umlal r0, r1, r2, r3 — rdhi=1, rn=2, rm=3, rdlo=0
   * base 0xFBE00000 | rdhi=1<<8 | rn=2<<16 | rm=3 | rdlo=0<<12
   * = 0xFBE00000 | 0x00000100 | 0x00020000 | 0x00000003 | 0
   * = 0xFBE20103 */
  thumb_opcode op = th_umlal(0, 1, 2, 3);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFBE20103);

  return 0;
}

/* ------------------------------------------------------------------ SMULL */

UT_TEST(test_smull_basic)
{
  setup_armv7m();

  /* smull r0, r1, r2, r3 — rdhi=1, rn=2, rm=3, rdlo=0
   * base 0xFB800000 | rdhi=1<<8 | rn=2<<16 | rm=3 | rdlo=0<<12
   * = 0xFB800000 | 0x00000100 | 0x00020000 | 0x00000003 | 0
   * = 0xFB820103 */
  thumb_opcode op = th_smull(0, 1, 2, 3);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFB820103);

  return 0;
}

/* ------------------------------------------------------------------ SMLAL */

UT_TEST(test_smlal_basic)
{
  setup_armv7m();

  /* smlal r0, r1, r2, r3 — rdhi=1, rn=2, rm=3, rdlo=0
   * base 0xFBC00000 | rdhi=1<<8 | rn=2<<16 | rm=3 | rdlo=0<<12
   * = 0xFBC00000 | 0x00000100 | 0x00020000 | 0x00000003 | 0
   * = 0xFBC20103 */
  thumb_opcode op = th_smlal(0, 1, 2, 3);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFBC20103);

  return 0;
}

/* ------------------------------------------------------------------ UDIV */

UT_TEST(test_udiv_basic)
{
  setup_armv7m();

  /* udiv r0, r1, r2 — rd=0, rn=1, rm=2
   * base 0xFBB0F0F0 | rd=0<<8 | rn=1<<16 | rm=2
   * = 0xFBB0F0F0 | 0 | 0x00010000 | 0x00000002
   * = 0xFBB1F0F2 */
  thumb_opcode op = th_udiv(0, 1, 2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFBB1F0F2);

  return 0;
}

UT_TEST(test_udiv_no_div_feature)
{
  setup_no_div();

  /* div feature disabled -> should fail */
  thumb_opcode op = th_udiv(0, 1, 2);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

/* ------------------------------------------------------------------ SDIV */

UT_TEST(test_sdiv_basic)
{
  setup_armv7m();

  /* sdiv r0, r1, r2 — rd=0, rn=1, rm=2
   * base 0xFB90F0F0 | rd=0<<8 | rn=1<<16 | rm=2
   * = 0xFB90F0F0 | 0 | 0x00010000 | 0x00000002
   * = 0xFB91F0F2 */
  thumb_opcode op = th_sdiv(0, 1, 2);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xFB91F0F2);

  return 0;
}

UT_TEST(test_sdiv_no_div_feature)
{
  setup_no_div();

  /* div feature disabled -> should fail */
  thumb_opcode op = th_sdiv(0, 1, 2);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

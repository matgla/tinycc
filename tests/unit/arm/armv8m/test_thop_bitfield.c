/*
 *  test_thop_bitfield.c - suite for arch/arm/thumb/thop_bitfield.c
 *
 *  Tests BFC, BFI, SBFX (T32, bfx=1) and SSAT/USAT (T32, sat=1).
 *  BFX instructions use split immediate placement (imm3+imm2 for lsb,
 *  imm2_place for width/msb).  SSAT/USAT have LSL and ASR variants
 *  with shift amount encoding.
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_bitfield.h"
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

static void setup_no_bfx(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m0",
      .feat = (thop_feat){.t16 = 1, .t32 = 1},
      .is_secure_tz = false,
  };
}

static void setup_no_sat(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m0",
      .feat = (thop_feat){.t16 = 1, .t32 = 1},
      .is_secure_tz = false,
  };
}

/* ------------------------------------------------------------------ tests */

UT_TEST(test_bfc_basic)
{
  setup_armv7m();

  /* bfc r0, #8, #4
   *   base = 0xf36f0000, rd=0 → bits [11:8] = 0
   *   lsb = 8  → imm3:imm2 = 010_00 → 0x2000
   *   width-1 = 3 → imm2_place = 0x03
   *   expected: 0xf36f200b
   */
  thumb_opcode op = th_bfc(R0, 8, 4);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xf36f200b);
  return 0;
}

UT_TEST(test_bfc_high_reg)
{
  setup_armv7m();

  /* bfc r10, #2, #20
   *   base = 0xf36f0000, rd=10 → bits [11:8] = 10 → 0x0a00
   *   lsb = 2  → imm3:imm2 = 000_10 → 0x0080
   *   width-1 = 21 → imm2_place = 0x15
   *   expected: 0xf36f0a95
   */
  thumb_opcode op = th_bfc(R10, 2, 20);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xf36f0a95);
  return 0;
}

UT_TEST(test_bfi_basic)
{
  setup_armv7m();

  /* bfi r0, r1, #8, #4
   *   base = 0xf3600000, rd=0 → 0, rn=1 → 0x10000
   *   lsb = 8  → imm3:imm2 = 010_00 → 0x2000
   *   width-1 = 3 → imm2_place = 0x03
   *   expected: 0xf361200b
   */
  thumb_opcode op = th_bfi(R0, R1, 8, 4);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xf361200b);
  return 0;
}

UT_TEST(test_sbfx_basic)
{
  setup_armv7m();

  /* sbfx r0, r1, #8, #4
   *   base = 0xf3400000, rd=0 → 0, rn=1 → 0x10000
   *   lsb = 8  → imm3:imm2 = 010_00 → 0x2000
   *   width-1 = 3 → imm2_place = 0x03
   *   expected: 0xf3412003
   */
  thumb_opcode op = th_sbfx(R0, R1, 8, 4);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xf3412003);
  return 0;
}

UT_TEST(test_ssat_lsl)
{
  setup_armv7m();

  /* ssat r0, #16, r1, lsl #2
   *   base = 0xf3000000 (LSL variant)
   *   rd=0 → 0, rn=1 → 0x10000
   *   imm2 = 16-1 = 15 → 0x0f
   *   shift_imm2 = 2 → bits [7:6] = 2 → 0x80
   *   shift_imm3 = 0 → bits [14:12] = 0
   *   expected: 0xf301008f
   */
  thumb_opcode op = th_ssat(R0, 16, R1,
                            (thumb_shift){.type = THUMB_SHIFT_LSL,
                                          .value = 2,
                                          .mode = THUMB_SHIFT_IMMEDIATE});
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xf301008f);
  return 0;
}

UT_TEST(test_ssat_asr)
{
  setup_armv7m();

  /* ssat r0, #16, r1, asr #3
   *   base = 0xf3200000 (ASR variant)
   *   rd=0 → 0, rn=1 → 0x10000
   *   imm2 = 16-1 = 15 → 0x0f
   *   shift_imm2 = 3 → bits [7:6] = 3 → 0xc0
   *   shift_imm3 = 0 → bits [14:12] = 0
   *   expected: 0xf32100cf
   */
  thumb_opcode op = th_ssat(R0, 16, R1,
                            (thumb_shift){.type = THUMB_SHIFT_ASR,
                                          .value = 3,
                                          .mode = THUMB_SHIFT_IMMEDIATE});
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xf32100cf);
  return 0;
}

UT_TEST(test_ssat_no_shift)
{
  setup_armv7m();

  /* ssat r0, #1, r1
   *   base = 0xf3000000 (LSL variant, shift=NONE is allowed)
   *   rd=0 → 0, rn=1 → 0x10000
   *   imm2 = 1-1 = 0 → 0x00
   *   expected: 0xf3010000
   */
  thumb_opcode op = th_ssat(R0, 1, R1, THUMB_SHIFT_DEFAULT);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xf3010000);
  return 0;
}

UT_TEST(test_usat_no_shift)
{
  setup_armv7m();

  /* usat r0, #8, r1
   *   base = 0xf3800000 (LSL variant)
   *   rd=0 → 0, rn=1 → 0x10000
   *   imm2 = 8 → 0x08
   *   expected: 0xf3810008
   */
  thumb_opcode op = th_usat(R0, 8, R1, THUMB_SHIFT_DEFAULT);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xf3810008);
  return 0;
}

UT_TEST(test_usat_lsl)
{
  setup_armv7m();

  /* usat r0, #8, r1, lsl #1
   *   base = 0xf3800000 (LSL variant)
   *   rd=0 → 0, rn=1 → 0x10000
   *   imm2 = 8 → 0x08
   *   shift_imm2 = 1 → bits [7:6] = 1 → 0x40
   *   expected: 0xf3810048
   */
  thumb_opcode op = th_usat(R0, 8, R1,
                            (thumb_shift){.type = THUMB_SHIFT_LSL,
                                          .value = 1,
                                          .mode = THUMB_SHIFT_IMMEDIATE});
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xf3810048);
  return 0;
}

UT_TEST(test_usat_asr)
{
  setup_armv7m();

  /* usat r0, #8, r1, asr #2
   *   base = 0xf3a00000 (ASR variant)
   *   rd=0 → 0, rn=1 → 0x10000
   *   imm2 = 8 → 0x08
   *   shift_imm2 = 2 → bits [7:6] = 2 → 0x80
   *   expected: 0xf3a10088
   */
  thumb_opcode op = th_usat(R0, 8, R1,
                            (thumb_shift){.type = THUMB_SHIFT_ASR,
                                          .value = 2,
                                          .mode = THUMB_SHIFT_IMMEDIATE});
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xf3a10088);
  return 0;
}

UT_TEST(test_bfx_feature_gate_off)
{
  setup_no_bfx();

  thumb_opcode op = th_bfc(R0, 0, 1);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  op = th_bfi(R0, R1, 0, 1);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  op = th_sbfx(R0, R1, 0, 1);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);
  return 0;
}

UT_TEST(test_sat_feature_gate_off)
{
  setup_no_sat();

  thumb_opcode op = th_ssat(R0, 16, R1, THUMB_SHIFT_DEFAULT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  op = th_usat(R0, 8, R1, THUMB_SHIFT_DEFAULT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);
  return 0;
}

UT_TEST(test_bfc_rd_pc_rejected)
{
  setup_armv7m();

  /* BFC has REG_NOT_PC on rd */
  thumb_opcode op = th_bfc(R_PC, 0, 1);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);
  return 0;
}

UT_TEST(test_bfi_rd_pc_rejected)
{
  setup_armv7m();

  thumb_opcode op = th_bfi(R_PC, R1, 0, 1);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);
  return 0;
}

UT_TEST(test_sbfx_rd_pc_rejected)
{
  setup_armv7m();

  thumb_opcode op = th_sbfx(R_PC, R1, 0, 1);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);
  return 0;
}

UT_TEST(test_ssat_rd_pc_rejected)
{
  setup_armv7m();

  thumb_opcode op = th_ssat(R_PC, 16, R1, THUMB_SHIFT_DEFAULT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);
  return 0;
}

UT_TEST(test_usat_rd_pc_rejected)
{
  setup_armv7m();

  thumb_opcode op = th_usat(R_PC, 8, R1, THUMB_SHIFT_DEFAULT);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);
  return 0;
}

/*
 *  test_thop_alu_imm.c - suite for arch/arm/thumb/thop_alu_imm.c
 *
 *  Tests T16 narrow and T32 wide ALU-immediate encodings:
 *  ADD/SUB imm8/imm3/SP forms, ADDW/SUBW, and T32-only
 *  RSB/ADC/SBC/AND/BIC/ORR/ORN/EOR modified-immediate forms.
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/thumb/thop_alu_imm.h"
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
              .dsp = 1,
          },
      .is_secure_tz = false,
  };
}

static void setup_no_modimm(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m3",
      .feat =
          (thop_feat){
              .t16 = 1,
              .t32 = 1,
              .it = 1,
              .mod_imm = 0,
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

/* ------------------------------------------------------------------ ADD */

UT_TEST(test_add_imm_t16_imm8)
{
  setup_armv7m();

  /* T1 imm8: adds r0, r0, #42 => base 0x3000 | (0<<8) | 42 = 0x302A */
  thumb_opcode op = th_add_imm(0, 0, 42, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x302A);

  /* r7, r7, #255 (max imm8) => 0x30FF */
  op = th_add_imm(7, 7, 255, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x37FF);

  return 0;
}

UT_TEST(test_add_imm_t16_imm3)
{
  setup_armv7m();

  /* T2 imm3: adds r0, r1, #7 => base 0x1C00 | (0<<0) | (1<<3) | (7<<6) = 0x1DC8 */
  thumb_opcode op = th_add_imm(0, 1, 7, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x1DC8);

  return 0;
}

UT_TEST(test_add_imm_t16_sp_imm7)
{
  setup_armv7m();

  /* ADD SP, SP, #32 (32/4=8) => base 0xb000 | 8 = 0xb008 */
  thumb_opcode op = th_add_imm(R_SP, R_SP, 32, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xb008);

  return 0;
}

UT_TEST(test_add_imm_t16_sp_imm8)
{
  setup_armv7m();

  /* ADD r0, SP, #64 (64/4=16) => base 0xa800 | 16 | (0<<8) = 0xa810 */
  thumb_opcode op = th_add_imm(0, R_SP, 64, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xa810);

  return 0;
}

UT_TEST(test_add_imm_t32_mod_imm)
{
  setup_armv7m();

  /* T3: add.w r0, r1, #256 (modified imm packed=0x04007080).
   * 256 does not fit in T16 imm3/imm8, so it must fall to T32.
   * Verified against objdump: F5017080 -> add.w r0, r1, #256. */
  thumb_opcode op = th_add_imm(0, 1, 256, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF5017080);

  return 0;
}

UT_TEST(test_add_imm_t32_mod_imm_setflags)
{
  setup_armv7m();

  /* T3 with S: adds.w r0, r1, #256. */
  thumb_opcode op = th_add_imm(0, 1, 256, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF5117080);

  return 0;
}

UT_TEST(test_addw_imm12)
{
  setup_armv7m();

  /* ADDW r0, r1, #4095 (plain 12-bit). 4095 is not a valid modified
   * immediate, so the TH_ADD_IMM table falls through to the ADDW variant.
   * Verified against objdump: F60170FF -> addw r0, r1, #4095. */
  thumb_opcode op = th_addw(0, 1, 4095);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF60170FF);

  return 0;
}

/* ------------------------------------------------------------------ SUB */

UT_TEST(test_sub_imm_t16_imm8)
{
  setup_armv7m();

  /* T1 imm8: subs r2, r2, #10 => base 0x3800 | (2<<8) | 10 = 0x3A0A */
  thumb_opcode op = th_sub_imm(2, 2, 10, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x3A0A);

  return 0;
}

UT_TEST(test_sub_imm_t16_imm3)
{
  setup_armv7m();

  /* T2 imm3: subs r0, r1, #3 => base 0x1E00 | (0<<0) | (1<<3) | (3<<6) = 0x1EC8 */
  thumb_opcode op = th_sub_imm(0, 1, 3, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x1EC8);

  return 0;
}

UT_TEST(test_sub_imm_t16_sp_imm7)
{
  setup_armv7m();

  /* SUB SP, SP, #16 (16/4=4) => base 0xb080 | 4 = 0xb084 */
  thumb_opcode op = th_sub_imm(R_SP, R_SP, 16, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0xb084);

  return 0;
}

UT_TEST(test_subw_imm12)
{
  setup_armv7m();

  /* SUBW r0, r1, #1 => base 0xF2A00000 | rd=0 | rn=1<<16 | imm12=1 packed
   * imm12=1 -> i=0, imm3=0, imm8=1 => 0x00000001
   * total = 0xF2A10001 */
  thumb_opcode op = th_subw(0, 1, 1);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF2A10001);

  return 0;
}

/* ------------------------------------------------------------------ T32-only ALU imm */

UT_TEST(test_rsb_imm_t32_mod_imm)
{
  setup_armv7m();

  /* RSBS r0, r1, #0 (modified imm 0 -> packed=0)
   * base 0xF1C00000 | S=1<<20 | rd=0 | rn=1<<16 | imm=0
   * = 0xF1D10000 */
  thumb_opcode op = th_rsb_imm(0, 1, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF1D10000);

  return 0;
}

UT_TEST(test_adc_imm_t32_mod_imm)
{
  setup_armv7m();

  /* ADCS r0, r1, #1 => base 0xF1400000 | S=1<<20 | rd=0 | rn=1 | imm=1
   * = 0xF1510001 */
  thumb_opcode op = th_adc_imm(0, 1, 1, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF1510001);

  return 0;
}

UT_TEST(test_sbc_imm_t32_mod_imm)
{
  setup_armv7m();

  /* SBCS r0, r1, #1 => base 0xF1600000 | S=1<<20 | rd=0 | rn=1 | imm=1
   * = 0xF1710001 */
  thumb_opcode op = th_sbc_imm(0, 1, 1, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF1710001);

  return 0;
}

UT_TEST(test_and_imm_t32_mod_imm)
{
  setup_armv7m();

  /* ANDS r0, r1, #0xff (modified imm packed=0x0ff)
   * base 0xF0000000 | S=1<<20 | rd=0 | rn=1 | imm=0xff
   * = 0xF01100FF */
  thumb_opcode op = th_and_imm(0, 1, 0xff, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF01100FF);

  return 0;
}

UT_TEST(test_bic_imm_t32_mod_imm)
{
  setup_armv7m();

  /* BICS r0, r1, #0xff => base 0xF0200000 | S=1<<20 | rd=0 | rn=1 | imm=0xff
   * = 0xF03100FF */
  thumb_opcode op = th_bic_imm(0, 1, 0xff, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF03100FF);

  return 0;
}

UT_TEST(test_orr_imm_t32_mod_imm)
{
  setup_armv7m();

  /* ORRS r0, r1, #0xff => base 0xF0400000 | S=1<<20 | rd=0 | rn=1 | imm=0xff
   * = 0xF05100FF */
  thumb_opcode op = th_orr_imm(0, 1, 0xff, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF05100FF);

  return 0;
}

UT_TEST(test_orn_imm_t32_mod_imm)
{
  setup_armv7m();

  /* ORNS r0, r1, #0 => base 0xF0600000 | S=1<<20 | rd=0 | rn=1 | imm=0
   * = 0xF0710000 */
  thumb_opcode op = th_orn_imm(0, 1, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF0710000);

  return 0;
}

UT_TEST(test_eor_imm_t32_mod_imm)
{
  setup_armv7m();

  /* EORS r0, r1, #0xff => base 0xF0800000 | S=1<<20 | rd=0 | rn=1 | imm=0xff
   * = 0xF09100FF */
  thumb_opcode op = th_eor_imm(0, 1, 0xff, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF09100FF);

  return 0;
}

/* ------------------------------------------------------------------ constraints / feature mismatches */

UT_TEST(test_add_imm_rd_ne_rn_falls_to_t2)
{
  setup_armv7m();

  /* T1 imm8 requires rd==rn. rd=0, rn=1 -> T2 imm3.
   * base 0x1C00 | (0<<0) | (1<<3) | (5<<6) = 0x1D48. */
  thumb_opcode op = th_add_imm(0, 1, 5, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 2);
  UT_ASSERT_EQ(op.opcode, 0x1D48);

  return 0;
}

UT_TEST(test_add_imm_high_reg_falls_to_t32)
{
  setup_armv7m();

  /* T1/T2 require low regs. R8 -> T32. */
  thumb_opcode op = th_add_imm(R8, R8, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xF1080801);

  return 0;
}

UT_TEST(test_add_imm_t16_sp_requires_sp)
{
  setup_armv7m();

  /* T16 ADD SP,SP,imm requires both rd and rn to be SP. */
  thumb_opcode op = th_add_imm(0, R_SP, 32, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_16BIT);
  UT_ASSERT_EQ(op.size, 2);
  /* Falls through to ADD r0, SP, #imm8 form: base 0xa800 | 8 | (0<<8) = 0xa808 */
  UT_ASSERT_EQ(op.opcode, 0xa808);

  return 0;
}

UT_TEST(test_and_imm_no_modimm_feature_fails)
{
  setup_no_modimm();

  /* AND imm requires modified immediate (mod_imm feature). */
  thumb_opcode op = th_and_imm(0, 1, 0xff, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

UT_TEST(test_add_imm_pc_in_rd_fails_t32)
{
  setup_armv7m();

  /* T32 ADD requires rd != PC. */
  thumb_opcode op = th_add_imm(R_PC, R1, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);

  return 0;
}

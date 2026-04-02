/*
 *  test_thop_constraints.c - systematic validation of every thop_emit constraint
 *
 *  Uses synthetic thop_table entries to exercise the generic engine:
 *    • register masks (REG_LOW_ONLY, NOT_SP, NOT_PC, SP_ONLY, PC_ONLY, NOT_LR)
 *    • equality constraints (REG_EQ_RN, REG_EQ_RM)
 *    • register-list bitmasks (LOW_REGSET, RM_BIT_NOT_SP, RM_BITS_NOT_LR_PC)
 *    • encoding enforcement (ENFORCE_ENCODING_16BIT / 32BIT)
 *    • feature gating (thop_feat32_subset)
 *    • S-bit / IT-block interactions (has_s_bit, implicit_s, forbid_s_in_it)
 *    • shift constraints (shift_allowed mask)
 *    • PUW constraints (puw_fixed)
 *    • immediate validation (IMM_RAW, IMM_PACK_CONST, IMM_PACK_3_8_1, signed, scaled)
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thumb.h"
#include "ut.h"

/* ------------------------------------------------------------------ helpers */

static void setup_full_features(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m4",
      .feat = (thop_feat){
          .t16 = 1, .t32 = 1, .it = 1, .mod_imm = 1, .movw_movt = 1,
          .bfx = 1, .clz_rbit = 1, .tbb_tbh = 1, .cbz = 1, .sat = 1, .div = 1,
          .dsp = 1, .ldaex = 1, .vfp_sp = 1, .vfp_dp = 1,
      },
      .is_secure_tz = false,
  };
}

static void setup_t16_only(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m0",
      .feat = (thop_feat){.t16 = 1},
      .is_secure_tz = false,
  };
}

static thop_args args_zero(void)
{
  return (thop_args){.rd = 0, .rn = 0, .rm = 0, .ra = 0,
                     .flags = FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                     .enc = ENFORCE_ENCODING_NONE,
                     .shift = THUMB_SHIFT_DEFAULT};
}

#define ASSERT_FAIL(op)                                                        \
  do                                                                           \
  {                                                                            \
    UT_ASSERT_EQ((op).size, 0);                                                \
    UT_ASSERT_EQ((op).opcode, 0);                                              \
  } while (0)

#define ASSERT_OK(op, sz)                                                      \
  do                                                                           \
  {                                                                            \
    UT_ASSERT_EQ((op).size, (sz));                                             \
  } while (0)

/* ======================================================================== */
/*  1. REGISTER CONSTRAINTS (thop_reg_ok)                                    */
/* ======================================================================== */

/* --- REG_LOW_ONLY --- */
static const thop_variant_shape SHAPE_LOW_RD = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rd_con = REG_LOW_ONLY,
    .feat = {.t16 = 1},
};
static const thop_variant VARIANT_LOW_RD[] = {{&SHAPE_LOW_RD, 0x0000, NULL}};
static const thop_table TABLE_LOW_RD = {"low_rd", VARIANT_LOW_RD, 1};

UT_TEST(test_reg_low_only_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 7;
  thumb_opcode op = thop_emit(TABLE_LOW_RD.name, TABLE_LOW_RD.variants,
                              TABLE_LOW_RD.variant_count, a);
  ASSERT_OK(op, 2);
  return 0;
}

UT_TEST(test_reg_low_only_fail_r8)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 8;
  thumb_opcode op = thop_emit(TABLE_LOW_RD.name, TABLE_LOW_RD.variants,
                              TABLE_LOW_RD.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

UT_TEST(test_reg_low_only_fail_r15)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 15;
  thumb_opcode op = thop_emit(TABLE_LOW_RD.name, TABLE_LOW_RD.variants,
                              TABLE_LOW_RD.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* --- REG_NOT_SP --- */
static const thop_variant_shape SHAPE_NOTSP_RD = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_NOT_SP,
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_NOTSP_RD[] = {{&SHAPE_NOTSP_RD, 0xF0000000, NULL}};
static const thop_table TABLE_NOTSP_RD = {"notsp_rd", VARIANT_NOTSP_RD, 1};

UT_TEST(test_reg_not_sp_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 12; /* r12 = IP, fine */
  thumb_opcode op = thop_emit(TABLE_NOTSP_RD.name, TABLE_NOTSP_RD.variants,
                              TABLE_NOTSP_RD.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_reg_not_sp_fail_r13)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 13; /* SP */
  thumb_opcode op = thop_emit(TABLE_NOTSP_RD.name, TABLE_NOTSP_RD.variants,
                              TABLE_NOTSP_RD.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* --- REG_NOT_PC --- */
static const thop_variant_shape SHAPE_NOTPC_RD = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_NOT_PC,
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_NOTPC_RD[] = {{&SHAPE_NOTPC_RD, 0xF0000000, NULL}};
static const thop_table TABLE_NOTPC_RD = {"notpc_rd", VARIANT_NOTPC_RD, 1};

UT_TEST(test_reg_not_pc_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 14; /* LR */
  thumb_opcode op = thop_emit(TABLE_NOTPC_RD.name, TABLE_NOTPC_RD.variants,
                              TABLE_NOTPC_RD.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_reg_not_pc_fail_r15)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 15; /* PC */
  thumb_opcode op = thop_emit(TABLE_NOTPC_RD.name, TABLE_NOTPC_RD.variants,
                              TABLE_NOTPC_RD.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* --- REG_NOT_LR --- */
static const thop_variant_shape SHAPE_NOTLR_RD = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_NOT_LR,
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_NOTLR_RD[] = {{&SHAPE_NOTLR_RD, 0xF0000000, NULL}};
static const thop_table TABLE_NOTLR_RD = {"notlr_rd", VARIANT_NOTLR_RD, 1};

UT_TEST(test_reg_not_lr_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 15; /* PC is fine */
  thumb_opcode op = thop_emit(TABLE_NOTLR_RD.name, TABLE_NOTLR_RD.variants,
                              TABLE_NOTLR_RD.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_reg_not_lr_fail_r14)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 14; /* LR */
  thumb_opcode op = thop_emit(TABLE_NOTLR_RD.name, TABLE_NOTLR_RD.variants,
                              TABLE_NOTLR_RD.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* --- REG_SP_ONLY --- */
static const thop_variant_shape SHAPE_SPONLY_RD = {
    .size = THOP_VARIANT_T16,
    .rd_place = {8, 3},
    .rd_con = REG_SP_ONLY,
    .feat = {.t16 = 1},
};
static const thop_variant VARIANT_SPONLY_RD[] = {{&SHAPE_SPONLY_RD, 0xA000, NULL}};
static const thop_table TABLE_SPONLY_RD = {"sponly_rd", VARIANT_SPONLY_RD, 1};

UT_TEST(test_reg_sp_only_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 13; /* SP */
  thumb_opcode op = thop_emit(TABLE_SPONLY_RD.name, TABLE_SPONLY_RD.variants,
                              TABLE_SPONLY_RD.variant_count, a);
  ASSERT_OK(op, 2);
  return 0;
}

UT_TEST(test_reg_sp_only_fail_r12)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 12;
  thumb_opcode op = thop_emit(TABLE_SPONLY_RD.name, TABLE_SPONLY_RD.variants,
                              TABLE_SPONLY_RD.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* --- REG_PC_ONLY --- */
static const thop_variant_shape SHAPE_PCONLY_RD = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_PC_ONLY,
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_PCONLY_RD[] = {{&SHAPE_PCONLY_RD, 0xF0000000, NULL}};
static const thop_table TABLE_PCONLY_RD = {"pconly_rd", VARIANT_PCONLY_RD, 1};

UT_TEST(test_reg_pc_only_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 15; /* PC */
  thumb_opcode op = thop_emit(TABLE_PCONLY_RD.name, TABLE_PCONLY_RD.variants,
                              TABLE_PCONLY_RD.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_reg_pc_only_fail_r14)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 14; /* LR */
  thumb_opcode op = thop_emit(TABLE_PCONLY_RD.name, TABLE_PCONLY_RD.variants,
                              TABLE_PCONLY_RD.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* --- Combined mask: REG_LOW_ONLY | REG_NOT_PC --- */
static const thop_variant_shape SHAPE_LOW_NOTPC_RD = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rd_con = REG_LOW_ONLY | REG_NOT_PC,
    .feat = {.t16 = 1},
};
static const thop_variant VARIANT_LOW_NOTPC_RD[] = {{&SHAPE_LOW_NOTPC_RD, 0x0000, NULL}};
static const thop_table TABLE_LOW_NOTPC_RD = {"low_notpc_rd", VARIANT_LOW_NOTPC_RD, 1};

UT_TEST(test_reg_combined_mask_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 7; /* low, not pc */
  thumb_opcode op = thop_emit(TABLE_LOW_NOTPC_RD.name, TABLE_LOW_NOTPC_RD.variants,
                              TABLE_LOW_NOTPC_RD.variant_count, a);
  ASSERT_OK(op, 2);
  return 0;
}

UT_TEST(test_reg_combined_mask_fail_high_reg)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 8; /* high */
  thumb_opcode op = thop_emit(TABLE_LOW_NOTPC_RD.name, TABLE_LOW_NOTPC_RD.variants,
                              TABLE_LOW_NOTPC_RD.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* ======================================================================== */
/*  2. REGISTER EQUALITY CONSTRAINTS                                         */
/* ======================================================================== */

static const thop_variant_shape SHAPE_EQ_RN = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rn_place = {3, 3},
    .rd_con = REG_LOW_ONLY | REG_EQ_RN,
    .rn_con = REG_LOW_ONLY,
    .feat = {.t16 = 1},
};
static const thop_variant VARIANT_EQ_RN[] = {{&SHAPE_EQ_RN, 0x0000, NULL}};
static const thop_table TABLE_EQ_RN = {"eq_rn", VARIANT_EQ_RN, 1};

UT_TEST(test_reg_eq_rn_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 3;
  a.rn = 3;
  thumb_opcode op = thop_emit(TABLE_EQ_RN.name, TABLE_EQ_RN.variants,
                              TABLE_EQ_RN.variant_count, a);
  ASSERT_OK(op, 2);
  return 0;
}

UT_TEST(test_reg_eq_rn_fail_mismatch)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 3;
  a.rn = 4;
  thumb_opcode op = thop_emit(TABLE_EQ_RN.name, TABLE_EQ_RN.variants,
                              TABLE_EQ_RN.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

UT_TEST(test_reg_eq_rn_fail_rd_not_low)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 8; /* rd fails LOW_ONLY before EQ_RN is checked */
  a.rn = 8;
  thumb_opcode op = thop_emit(TABLE_EQ_RN.name, TABLE_EQ_RN.variants,
                              TABLE_EQ_RN.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

static const thop_variant_shape SHAPE_EQ_RM = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rm_place = {3, 3},
    .rd_con = REG_LOW_ONLY | REG_EQ_RM,
    .rm_con = REG_LOW_ONLY,
    .feat = {.t16 = 1},
};
static const thop_variant VARIANT_EQ_RM[] = {{&SHAPE_EQ_RM, 0x0000, NULL}};
static const thop_table TABLE_EQ_RM = {"eq_rm", VARIANT_EQ_RM, 1};

UT_TEST(test_reg_eq_rm_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 5;
  a.rm = 5;
  thumb_opcode op = thop_emit(TABLE_EQ_RM.name, TABLE_EQ_RM.variants,
                              TABLE_EQ_RM.variant_count, a);
  ASSERT_OK(op, 2);
  return 0;
}

UT_TEST(test_reg_eq_rm_fail_mismatch)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 5;
  a.rm = 6;
  thumb_opcode op = thop_emit(TABLE_EQ_RM.name, TABLE_EQ_RM.variants,
                              TABLE_EQ_RM.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* ======================================================================== */
/*  3. REGISTER-LIST BITMASK CONSTRAINTS                                     */
/* ======================================================================== */

static const thop_variant_shape SHAPE_LOW_REGSET = {
    .size = THOP_VARIANT_T16,
    .rm_raw_place = {0, 8},
    .rm_con = REG_LOW_REGSET,
    .feat = {.t16 = 1},
};
static const thop_variant VARIANT_LOW_REGSET[] = {{&SHAPE_LOW_REGSET, 0xB400, NULL}};
static const thop_table TABLE_LOW_REGSET = {"low_regset", VARIANT_LOW_REGSET, 1};

UT_TEST(test_reg_low_regset_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rm = 0x55; /* r0, r2, r4, r6 */
  thumb_opcode op = thop_emit(TABLE_LOW_REGSET.name, TABLE_LOW_REGSET.variants,
                              TABLE_LOW_REGSET.variant_count, a);
  ASSERT_OK(op, 2);
  return 0;
}

UT_TEST(test_reg_low_regset_fail_bit8)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rm = 0x155; /* bit 8 set */
  thumb_opcode op = thop_emit(TABLE_LOW_REGSET.name, TABLE_LOW_REGSET.variants,
                              TABLE_LOW_REGSET.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

static const thop_variant_shape SHAPE_RM_BIT_NOT_SP = {
    .size = THOP_VARIANT_T32,
    .rm_raw_place = {0, 13},
    .rm_con = REG_RM_BIT_NOT_SP,
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_RM_BIT_NOT_SP[] = {{&SHAPE_RM_BIT_NOT_SP, 0xE92D0000, NULL}};
static const thop_table TABLE_RM_BIT_NOT_SP = {"rm_bit_not_sp", VARIANT_RM_BIT_NOT_SP, 1};

UT_TEST(test_reg_rm_bit_not_sp_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rm = 0x1FFF; /* all bits 0-12 set, bit 13 clear */
  thumb_opcode op = thop_emit(TABLE_RM_BIT_NOT_SP.name, TABLE_RM_BIT_NOT_SP.variants,
                              TABLE_RM_BIT_NOT_SP.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_reg_rm_bit_not_sp_fail)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rm = (1u << 13); /* SP bit set */
  thumb_opcode op = thop_emit(TABLE_RM_BIT_NOT_SP.name, TABLE_RM_BIT_NOT_SP.variants,
                              TABLE_RM_BIT_NOT_SP.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

static const thop_variant_shape SHAPE_RM_BITS_NOT_LR_PC = {
    .size = THOP_VARIANT_T32,
    .rm_raw_place = {0, 14},
    .rm_con = REG_RM_BITS_NOT_LR_PC,
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_RM_BITS_NOT_LR_PC[] = {{&SHAPE_RM_BITS_NOT_LR_PC, 0xE8BD0000, NULL}};
static const thop_table TABLE_RM_BITS_NOT_LR_PC = {"rm_bits_not_lr_pc", VARIANT_RM_BITS_NOT_LR_PC, 1};

UT_TEST(test_reg_rm_bits_not_lr_pc_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rm = 0x3FFF; /* bits 0-13 set, 14/15 clear */
  thumb_opcode op = thop_emit(TABLE_RM_BITS_NOT_LR_PC.name, TABLE_RM_BITS_NOT_LR_PC.variants,
                              TABLE_RM_BITS_NOT_LR_PC.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_reg_rm_bits_not_lr_pc_fail_lr)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rm = (1u << 14); /* LR */
  thumb_opcode op = thop_emit(TABLE_RM_BITS_NOT_LR_PC.name, TABLE_RM_BITS_NOT_LR_PC.variants,
                              TABLE_RM_BITS_NOT_LR_PC.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

UT_TEST(test_reg_rm_bits_not_lr_pc_fail_pc)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rm = (1u << 15); /* PC */
  thumb_opcode op = thop_emit(TABLE_RM_BITS_NOT_LR_PC.name, TABLE_RM_BITS_NOT_LR_PC.variants,
                              TABLE_RM_BITS_NOT_LR_PC.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* ======================================================================== */
/*  4. ENCODING ENFORCEMENT                                                  */
/* ======================================================================== */

static const thop_variant_shape SHAPE_ENC_T16 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rd_con = REG_LOW_ONLY,
    .feat = {.t16 = 1},
};
static const thop_variant_shape SHAPE_ENC_T32 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_ANY,
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_ENC_DUAL[] = {
    {&SHAPE_ENC_T16, 0x0000, NULL},
    {&SHAPE_ENC_T32, 0xF0000000, NULL},
};
static const thop_table TABLE_ENC_DUAL = {"enc_dual", VARIANT_ENC_DUAL, 2};

UT_TEST(test_enc_none_prefers_first)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 5;
  thumb_opcode op = thop_emit(TABLE_ENC_DUAL.name, TABLE_ENC_DUAL.variants,
                              TABLE_ENC_DUAL.variant_count, a);
  /* T16 comes first, rd=5 is low reg → T16 matches */
  ASSERT_OK(op, 2);
  return 0;
}

UT_TEST(test_enc_force_16bit_ok)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 5;
  a.enc = ENFORCE_ENCODING_16BIT;
  thumb_opcode op = thop_emit(TABLE_ENC_DUAL.name, TABLE_ENC_DUAL.variants,
                              TABLE_ENC_DUAL.variant_count, a);
  ASSERT_OK(op, 2);
  return 0;
}

UT_TEST(test_enc_force_16bit_fail)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 8; /* high reg, T16 requires LOW_ONLY */
  a.enc = ENFORCE_ENCODING_16BIT;
  thumb_opcode op = thop_emit(TABLE_ENC_DUAL.name, TABLE_ENC_DUAL.variants,
                              TABLE_ENC_DUAL.variant_count, a);
  /* T16 rejected by reg constraint, T32 rejected by enc enforcement */
  ASSERT_FAIL(op);
  return 0;
}

UT_TEST(test_enc_force_32bit_ok)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 5;
  a.enc = ENFORCE_ENCODING_32BIT;
  thumb_opcode op = thop_emit(TABLE_ENC_DUAL.name, TABLE_ENC_DUAL.variants,
                              TABLE_ENC_DUAL.variant_count, a);
  /* T16 skipped by enc enforcement, T32 matches */
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_enc_force_32bit_only_table)
{
  /* Table with only T16 variant, force 32-bit → fail */
  static const thop_variant_shape SHAPE_ONLY_T16 = {
      .size = THOP_VARIANT_T16,
      .rd_place = {0, 3},
      .rd_con = REG_LOW_ONLY,
      .feat = {.t16 = 1},
  };
  static const thop_variant VARIANT_ONLY_T16[] = {{&SHAPE_ONLY_T16, 0x0000, NULL}};
  static const thop_table TABLE_ONLY_T16 = {"only_t16", VARIANT_ONLY_T16, 1};

  setup_full_features();
  thop_args a = args_zero();
  a.enc = ENFORCE_ENCODING_32BIT;
  thumb_opcode op = thop_emit(TABLE_ONLY_T16.name, TABLE_ONLY_T16.variants,
                              TABLE_ONLY_T16.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* ======================================================================== */
/*  5. FEATURE GATING                                                        */
/* ======================================================================== */

static const thop_variant_shape SHAPE_FEAT_T32_DSP = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_ANY,
    .feat = {.t32 = 1, .dsp = 1},
};
static const thop_variant VARIANT_FEAT_DSP[] = {{&SHAPE_FEAT_T32_DSP, 0xFA000000, NULL}};
static const thop_table TABLE_FEAT_DSP = {"feat_dsp", VARIANT_FEAT_DSP, 1};

UT_TEST(test_feat_dsp_present)
{
  setup_full_features(); /* has dsp=1 */
  thop_args a = args_zero();
  a.rd = 0;
  thumb_opcode op = thop_emit(TABLE_FEAT_DSP.name, TABLE_FEAT_DSP.variants,
                              TABLE_FEAT_DSP.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_feat_dsp_missing)
{
  setup_t16_only(); /* no dsp */
  thop_args a = args_zero();
  a.rd = 0;
  thumb_opcode op = thop_emit(TABLE_FEAT_DSP.name, TABLE_FEAT_DSP.variants,
                              TABLE_FEAT_DSP.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

static const thop_variant_shape SHAPE_FEAT_T32_DIV = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_ANY,
    .feat = {.t32 = 1, .div = 1},
};
static const thop_variant VARIANT_FEAT_DIV[] = {{&SHAPE_FEAT_T32_DIV, 0xFBB00000, NULL}};
static const thop_table TABLE_FEAT_DIV = {"feat_div", VARIANT_FEAT_DIV, 1};

UT_TEST(test_feat_div_present)
{
  setup_full_features(); /* has div=1 */
  thop_args a = args_zero();
  thumb_opcode op = thop_emit(TABLE_FEAT_DIV.name, TABLE_FEAT_DIV.variants,
                              TABLE_FEAT_DIV.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_feat_div_missing)
{
  setup_t16_only();
  thop_args a = args_zero();
  thumb_opcode op = thop_emit(TABLE_FEAT_DIV.name, TABLE_FEAT_DIV.variants,
                              TABLE_FEAT_DIV.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* ======================================================================== */
/*  6. S-BIT / IT-BLOCK INTERACTIONS                                         */
/* ======================================================================== */

static const thop_variant_shape SHAPE_HAS_S_BIT = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_ANY,
    .has_s_bit = 1,
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_HAS_S_BIT[] = {{&SHAPE_HAS_S_BIT, 0xF0000000, NULL}};
static const thop_table TABLE_HAS_S_BIT = {"has_s_bit", VARIANT_HAS_S_BIT, 1};

UT_TEST(test_sbit_has_s_bit_not_set)
{
  setup_full_features();
  thop_args a = args_zero();
  a.flags = FLAGS_BEHAVIOUR_NOT_IMPORTANT;
  thumb_opcode op = thop_emit(TABLE_HAS_S_BIT.name, TABLE_HAS_S_BIT.variants,
                              TABLE_HAS_S_BIT.variant_count, a);
  ASSERT_OK(op, 4);
  /* S-bit not set → no bit 20 */
  UT_ASSERT_EQ((op.opcode >> 20) & 1, 0);
  return 0;
}

UT_TEST(test_sbit_has_s_bit_set)
{
  setup_full_features();
  thop_args a = args_zero();
  a.flags = FLAGS_BEHAVIOUR_SET;
  thumb_opcode op = thop_emit(TABLE_HAS_S_BIT.name, TABLE_HAS_S_BIT.variants,
                              TABLE_HAS_S_BIT.variant_count, a);
  ASSERT_OK(op, 4);
  /* S-bit set → bit 20 should be 1 */
  UT_ASSERT_EQ((op.opcode >> 20) & 1, 1);
  return 0;
}

static const thop_variant_shape SHAPE_NO_S_BIT = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rd_con = REG_LOW_ONLY,
    .feat = {.t16 = 1},
    /* no has_s_bit, no implicit_s */
};
static const thop_variant VARIANT_NO_S_BIT[] = {{&SHAPE_NO_S_BIT, 0x0000, NULL}};
static const thop_table TABLE_NO_S_BIT = {"no_s_bit", VARIANT_NO_S_BIT, 1};

UT_TEST(test_sbit_set_but_no_s_bit_support)
{
  setup_full_features();
  thop_args a = args_zero();
  a.flags = FLAGS_BEHAVIOUR_SET;
  thumb_opcode op = thop_emit(TABLE_NO_S_BIT.name, TABLE_NO_S_BIT.variants,
                              TABLE_NO_S_BIT.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

static const thop_variant_shape SHAPE_IMPLICIT_S = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rd_con = REG_LOW_ONLY,
    .implicit_s = 1,
    .feat = {.t16 = 1},
};
static const thop_variant VARIANT_IMPLICIT_S[] = {{&SHAPE_IMPLICIT_S, 0x0000, NULL}};
static const thop_table TABLE_IMPLICIT_S = {"implicit_s", VARIANT_IMPLICIT_S, 1};

UT_TEST(test_implicit_s_outside_it)
{
  setup_full_features();
  thop_args a = args_zero();
  a.in_it_block = false;
  thumb_opcode op = thop_emit(TABLE_IMPLICIT_S.name, TABLE_IMPLICIT_S.variants,
                              TABLE_IMPLICIT_S.variant_count, a);
  ASSERT_OK(op, 2);
  return 0;
}

UT_TEST(test_implicit_s_inside_it)
{
  setup_full_features();
  thop_args a = args_zero();
  a.in_it_block = true;
  thumb_opcode op = thop_emit(TABLE_IMPLICIT_S.name, TABLE_IMPLICIT_S.variants,
                              TABLE_IMPLICIT_S.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

static const thop_variant_shape SHAPE_FORBID_S_IN_IT = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_ANY,
    .has_s_bit = 1,
    .forbid_s_in_it = 1,
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_FORBID_S_IN_IT[] = {{&SHAPE_FORBID_S_IN_IT, 0xF0000000, NULL}};
static const thop_table TABLE_FORBID_S_IN_IT = {"forbid_s_in_it", VARIANT_FORBID_S_IN_IT, 1};

UT_TEST(test_forbid_s_in_it_outside_it)
{
  setup_full_features();
  thop_args a = args_zero();
  a.in_it_block = false;
  a.flags = FLAGS_BEHAVIOUR_SET;
  thumb_opcode op = thop_emit(TABLE_FORBID_S_IN_IT.name, TABLE_FORBID_S_IN_IT.variants,
                              TABLE_FORBID_S_IN_IT.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_forbid_s_in_it_inside_it)
{
  setup_full_features();
  thop_args a = args_zero();
  a.in_it_block = true;
  a.flags = FLAGS_BEHAVIOUR_SET;
  thumb_opcode op = thop_emit(TABLE_FORBID_S_IN_IT.name, TABLE_FORBID_S_IN_IT.variants,
                              TABLE_FORBID_S_IN_IT.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* ======================================================================== */
/*  7. SHIFT CONSTRAINTS                                                     */
/* ======================================================================== */

static const thop_variant_shape SHAPE_SHIFT_LSL_ONLY = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_ANY,
    .shift_type_bits = {4, 2},
    .shift_imm2_bits = {6, 2},
    .shift_imm3_bits = {12, 3},
    .shift_allowed = (1u << THUMB_SHIFT_LSL),
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_SHIFT_LSL_ONLY[] = {{&SHAPE_SHIFT_LSL_ONLY, 0xEA000000, NULL}};
static const thop_table TABLE_SHIFT_LSL_ONLY = {"shift_lsl_only", VARIANT_SHIFT_LSL_ONLY, 1};

UT_TEST(test_shift_lsl_allowed)
{
  setup_full_features();
  thop_args a = args_zero();
  a.shift = (thumb_shift){.type = THUMB_SHIFT_LSL, .value = 5, .mode = THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = thop_emit(TABLE_SHIFT_LSL_ONLY.name, TABLE_SHIFT_LSL_ONLY.variants,
                              TABLE_SHIFT_LSL_ONLY.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_shift_lsr_rejected)
{
  setup_full_features();
  thop_args a = args_zero();
  a.shift = (thumb_shift){.type = THUMB_SHIFT_LSR, .value = 5, .mode = THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = thop_emit(TABLE_SHIFT_LSL_ONLY.name, TABLE_SHIFT_LSL_ONLY.variants,
                              TABLE_SHIFT_LSL_ONLY.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

UT_TEST(test_shift_none_allowed_when_fields_present)
{
  /* When shift fields exist and shift.type == NONE, the shift check
   * is skipped entirely, so the variant matches. */
  setup_full_features();
  thop_args a = args_zero();
  a.shift = THUMB_SHIFT_DEFAULT;
  thumb_opcode op = thop_emit(TABLE_SHIFT_LSL_ONLY.name, TABLE_SHIFT_LSL_ONLY.variants,
                              TABLE_SHIFT_LSL_ONLY.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

static const thop_variant_shape SHAPE_NO_SHIFT_FIELDS = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rd_con = REG_LOW_ONLY,
    .feat = {.t16 = 1},
    /* no shift_type_bits, no shift_imm*, shift_allowed = 0 */
};
static const thop_variant VARIANT_NO_SHIFT_FIELDS[] = {{&SHAPE_NO_SHIFT_FIELDS, 0x0000, NULL}};
static const thop_table TABLE_NO_SHIFT_FIELDS = {"no_shift_fields", VARIANT_NO_SHIFT_FIELDS, 1};

UT_TEST(test_shift_any_rejected_when_no_fields)
{
  setup_full_features();
  thop_args a = args_zero();
  a.shift = (thumb_shift){.type = THUMB_SHIFT_LSL, .value = 1, .mode = THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = thop_emit(TABLE_NO_SHIFT_FIELDS.name, TABLE_NO_SHIFT_FIELDS.variants,
                              TABLE_NO_SHIFT_FIELDS.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* ======================================================================== */
/*  8. PUW CONSTRAINTS                                                       */
/* ======================================================================== */

static const thop_variant_shape SHAPE_PUW_FIXED = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_ANY,
    .puw_fixed = 6, /* must be PUW=6 (post-indexed, add, writeback) */
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_PUW_FIXED[] = {{&SHAPE_PUW_FIXED, 0xF8400000, NULL}};
static const thop_table TABLE_PUW_FIXED = {"puw_fixed", VARIANT_PUW_FIXED, 1};

UT_TEST(test_puw_fixed_match)
{
  setup_full_features();
  thop_args a = args_zero();
  a.puw = 6;
  thumb_opcode op = thop_emit(TABLE_PUW_FIXED.name, TABLE_PUW_FIXED.variants,
                              TABLE_PUW_FIXED.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_puw_fixed_mismatch)
{
  setup_full_features();
  thop_args a = args_zero();
  a.puw = 5;
  thumb_opcode op = thop_emit(TABLE_PUW_FIXED.name, TABLE_PUW_FIXED.variants,
                              TABLE_PUW_FIXED.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

static const thop_variant_shape SHAPE_PUW_BITS = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_ANY,
    .puw_bits = {8, 3},
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_PUW_BITS[] = {{&SHAPE_PUW_BITS, 0xF8400000, NULL}};
static const thop_table TABLE_PUW_BITS = {"puw_bits", VARIANT_PUW_BITS, 1};

UT_TEST(test_puw_bits_any_value)
{
  setup_full_features();
  thop_args a = args_zero();
  a.puw = 7;
  thumb_opcode op = thop_emit(TABLE_PUW_BITS.name, TABLE_PUW_BITS.variants,
                              TABLE_PUW_BITS.variant_count, a);
  ASSERT_OK(op, 4);
  /* Check that puw bits are placed correctly: bit 8 should be 1 (puw=7, bit0) */
  UT_ASSERT_EQ((op.opcode >> 8) & 7, 7);
  return 0;
}

/* ======================================================================== */
/*  9. IMMEDIATE VALIDATION                                                  */
/* ======================================================================== */

/* --- IMM_RAW with width --- */
static const thop_variant_shape SHAPE_IMM_RAW_8 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rd_con = REG_LOW_ONLY,
    .imm = {.kind = IMM_RAW, .width = 8},
    .imm_place = {0, 8},
    .feat = {.t16 = 1},
};
static const thop_variant VARIANT_IMM_RAW_8[] = {{&SHAPE_IMM_RAW_8, 0x0000, NULL}};
static const thop_table TABLE_IMM_RAW_8 = {"imm_raw_8", VARIANT_IMM_RAW_8, 1};

UT_TEST(test_imm_raw_width_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 255; /* max for 8 bits */
  thumb_opcode op = thop_emit(TABLE_IMM_RAW_8.name, TABLE_IMM_RAW_8.variants,
                              TABLE_IMM_RAW_8.variant_count, a);
  ASSERT_OK(op, 2);
  return 0;
}

UT_TEST(test_imm_raw_width_fail)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 256; /* too big for 8 bits */
  thumb_opcode op = thop_emit(TABLE_IMM_RAW_8.name, TABLE_IMM_RAW_8.variants,
                              TABLE_IMM_RAW_8.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* --- IMM_RAW with scaling --- */
static const thop_variant_shape SHAPE_IMM_RAW_SCALE2 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rd_con = REG_LOW_ONLY,
    .imm = {.kind = IMM_RAW, .width = 8, .scale_log2 = 2},
    .imm_place = {0, 8},
    .feat = {.t16 = 1},
};
static const thop_variant VARIANT_IMM_RAW_SCALE2[] = {{&SHAPE_IMM_RAW_SCALE2, 0x0000, NULL}};
static const thop_table TABLE_IMM_RAW_SCALE2 = {"imm_raw_scale2", VARIANT_IMM_RAW_SCALE2, 1};

UT_TEST(test_imm_raw_scaled_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 1020; /* 255 * 4, fits after scaling by 4 */
  thumb_opcode op = thop_emit(TABLE_IMM_RAW_SCALE2.name, TABLE_IMM_RAW_SCALE2.variants,
                              TABLE_IMM_RAW_SCALE2.variant_count, a);
  ASSERT_OK(op, 2);
  return 0;
}

UT_TEST(test_imm_raw_scaled_fail_not_aligned)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 1022; /* not divisible by 4 */
  thumb_opcode op = thop_emit(TABLE_IMM_RAW_SCALE2.name, TABLE_IMM_RAW_SCALE2.variants,
                              TABLE_IMM_RAW_SCALE2.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

UT_TEST(test_imm_raw_scaled_fail_too_big)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 1024; /* 256 * 4, scaled value = 256 > 255 */
  thumb_opcode op = thop_emit(TABLE_IMM_RAW_SCALE2.name, TABLE_IMM_RAW_SCALE2.variants,
                              TABLE_IMM_RAW_SCALE2.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* --- IMM_RAW signed --- */
static const thop_variant_shape SHAPE_IMM_RAW_SIGNED = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_ANY,
    .imm = {.kind = IMM_RAW, .width = 12, .is_signed = 1},
    .imm_place = {0, 12},
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_IMM_RAW_SIGNED[] = {{&SHAPE_IMM_RAW_SIGNED, 0xF0000000, NULL}};
static const thop_table TABLE_IMM_RAW_SIGNED = {"imm_raw_signed", VARIANT_IMM_RAW_SIGNED, 1};

UT_TEST(test_imm_raw_signed_pass_negative)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = (uint32_t)-4095; /* -4095 as unsigned, abs = 4095 */
  thumb_opcode op = thop_emit(TABLE_IMM_RAW_SIGNED.name, TABLE_IMM_RAW_SIGNED.variants,
                              TABLE_IMM_RAW_SIGNED.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_imm_raw_signed_fail_positive)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 100; /* positive value rejected when is_signed=1 */
  thumb_opcode op = thop_emit(TABLE_IMM_RAW_SIGNED.name, TABLE_IMM_RAW_SIGNED.variants,
                              TABLE_IMM_RAW_SIGNED.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

UT_TEST(test_imm_raw_signed_fail_zero)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 0; /* zero is not negative */
  thumb_opcode op = thop_emit(TABLE_IMM_RAW_SIGNED.name, TABLE_IMM_RAW_SIGNED.variants,
                              TABLE_IMM_RAW_SIGNED.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* --- IMM_NONE --- */
static const thop_variant_shape SHAPE_IMM_NONE = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rd_con = REG_LOW_ONLY,
    .imm = {.kind = IMM_NONE},
    .feat = {.t16 = 1},
};
static const thop_variant VARIANT_IMM_NONE[] = {{&SHAPE_IMM_NONE, 0x0000, NULL}};
static const thop_table TABLE_IMM_NONE = {"imm_none", VARIANT_IMM_NONE, 1};

UT_TEST(test_imm_none_zero_ok)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 0; /* must be zero */
  thumb_opcode op = thop_emit(TABLE_IMM_NONE.name, TABLE_IMM_NONE.variants,
                              TABLE_IMM_NONE.variant_count, a);
  ASSERT_OK(op, 2);
  return 0;
}

UT_TEST(test_imm_none_any_value_ignored)
{
  /* IMM_NONE means no immediate is expected; thop_emit skips validation
   * entirely, so any a.imm value is silently ignored. */
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 0xDEADBEEF; /* arbitrary value, should be ignored */
  thumb_opcode op = thop_emit(TABLE_IMM_NONE.name, TABLE_IMM_NONE.variants,
                              TABLE_IMM_NONE.variant_count, a);
  ASSERT_OK(op, 2);
  return 0;
}

/* --- IMM_PACK_CONST --- */
static const thop_variant_shape SHAPE_IMM_PACK_CONST = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_ANY,
    .imm = {.kind = IMM_PACK_CONST, .width = 12},
    .feat = {.t32 = 1, .mod_imm = 1},
};
static const thop_variant VARIANT_IMM_PACK_CONST[] = {{&SHAPE_IMM_PACK_CONST, 0xF0400000, NULL}};
static const thop_table TABLE_IMM_PACK_CONST = {"imm_pack_const", VARIANT_IMM_PACK_CONST, 1};

UT_TEST(test_imm_pack_const_zero)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 0; /* zero is always valid for modified immediate */
  thumb_opcode op = thop_emit(TABLE_IMM_PACK_CONST.name, TABLE_IMM_PACK_CONST.variants,
                              TABLE_IMM_PACK_CONST.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_imm_pack_const_valid)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 0xFF000000; /* rotatable 8-bit pattern */
  thumb_opcode op = thop_emit(TABLE_IMM_PACK_CONST.name, TABLE_IMM_PACK_CONST.variants,
                              TABLE_IMM_PACK_CONST.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_imm_pack_const_invalid)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 0x12345678; /* not a valid modified immediate */
  thumb_opcode op = thop_emit(TABLE_IMM_PACK_CONST.name, TABLE_IMM_PACK_CONST.variants,
                              TABLE_IMM_PACK_CONST.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* --- IMM_PACK_3_8_1 --- */
static const thop_variant_shape SHAPE_IMM_PACK_3_8_1 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_ANY,
    .imm = {.kind = IMM_PACK_3_8_1, .width = 12},
    .imm_place = {0, 12},
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_IMM_PACK_3_8_1[] = {{&SHAPE_IMM_PACK_3_8_1, 0xF2400000, NULL}};
static const thop_table TABLE_IMM_PACK_3_8_1 = {"imm_pack_3_8_1", VARIANT_IMM_PACK_3_8_1, 1};

UT_TEST(test_imm_pack_3_8_1_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 0x123; /* fits in 12 bits */
  thumb_opcode op = thop_emit(TABLE_IMM_PACK_3_8_1.name, TABLE_IMM_PACK_3_8_1.variants,
                              TABLE_IMM_PACK_3_8_1.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_imm_pack_3_8_1_fail_too_big)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 0x1000; /* 4096, too big for 12 bits */
  thumb_opcode op = thop_emit(TABLE_IMM_PACK_3_8_1.name, TABLE_IMM_PACK_3_8_1.variants,
                              TABLE_IMM_PACK_3_8_1.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* ======================================================================== */
/*  10. SPECIAL PLACEMENT FIELDS                                             */
/* ======================================================================== */

/* --- has_rd_hi --- */
static const thop_variant_shape SHAPE_HAS_RD_HI = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rd_con = REG_ANY,
    .has_rd_hi = 1,
    .feat = {.t16 = 1},
};
static const thop_variant VARIANT_HAS_RD_HI[] = {{&SHAPE_HAS_RD_HI, 0x0000, NULL}};
static const thop_table TABLE_HAS_RD_HI = {"has_rd_hi", VARIANT_HAS_RD_HI, 1};

UT_TEST(test_has_rd_hi_low)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 3; /* rd<3:0>=3, rd_hi=0 */
  thumb_opcode op = thop_emit(TABLE_HAS_RD_HI.name, TABLE_HAS_RD_HI.variants,
                              TABLE_HAS_RD_HI.variant_count, a);
  ASSERT_OK(op, 2);
  UT_ASSERT_EQ((op.opcode >> 7) & 1, 0);
  UT_ASSERT_EQ(op.opcode & 7, 3);
  return 0;
}

UT_TEST(test_has_rd_hi_high)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 8; /* rd<3:0>=0, rd_hi=1 (bit 7) */
  thumb_opcode op = thop_emit(TABLE_HAS_RD_HI.name, TABLE_HAS_RD_HI.variants,
                              TABLE_HAS_RD_HI.variant_count, a);
  ASSERT_OK(op, 2);
  UT_ASSERT_EQ((op.opcode >> 7) & 1, 1);
  return 0;
}

/* --- dn_rd_split --- */
static const thop_variant_shape SHAPE_DN_RD_SPLIT = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rd_con = REG_ANY,
    .dn_rd_split = {0, 3},
    .feat = {.t16 = 1},
};
static const thop_variant VARIANT_DN_RD_SPLIT[] = {{&SHAPE_DN_RD_SPLIT, 0x0000, NULL}};
static const thop_table TABLE_DN_RD_SPLIT = {"dn_rd_split", VARIANT_DN_RD_SPLIT, 1};

UT_TEST(test_dn_rd_split_r8)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 8; /* DN=1, Rd<2:0>=0 */
  thumb_opcode op = thop_emit(TABLE_DN_RD_SPLIT.name, TABLE_DN_RD_SPLIT.variants,
                              TABLE_DN_RD_SPLIT.variant_count, a);
  ASSERT_OK(op, 2);
  UT_ASSERT_EQ((op.opcode >> 7) & 1, 1);
  UT_ASSERT_EQ(op.opcode & 7, 0);
  return 0;
}

UT_TEST(test_dn_rd_split_r12)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 12; /* DN=1, Rd<2:0>=4 */
  thumb_opcode op = thop_emit(TABLE_DN_RD_SPLIT.name, TABLE_DN_RD_SPLIT.variants,
                              TABLE_DN_RD_SPLIT.variant_count, a);
  ASSERT_OK(op, 2);
  UT_ASSERT_EQ((op.opcode >> 7) & 1, 1);
  UT_ASSERT_EQ(op.opcode & 7, 4);
  return 0;
}

/* --- split_imm2 / split_imm3 --- */
static const thop_variant_shape SHAPE_SPLIT_IMM = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_ANY,
    .split_imm2_place = {6, 2},
    .split_imm3_place = {12, 3},
    .imm = {.kind = IMM_RAW, .width = 5},
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_SPLIT_IMM[] = {{&SHAPE_SPLIT_IMM, 0xF0000000, NULL}};
static const thop_table TABLE_SPLIT_IMM = {"split_imm", VARIANT_SPLIT_IMM, 1};

UT_TEST(test_split_imm_placement)
{
  setup_full_features();
  thop_args a = args_zero();
  a.imm = 0x15; /* 0b10101: imm<1:0>=01, imm<4:2>=101 */
  thumb_opcode op = thop_emit(TABLE_SPLIT_IMM.name, TABLE_SPLIT_IMM.variants,
                              TABLE_SPLIT_IMM.variant_count, a);
  ASSERT_OK(op, 4);
  UT_ASSERT_EQ((op.opcode >> 6) & 3, 1);  /* imm<1:0> */
  UT_ASSERT_EQ((op.opcode >> 12) & 7, 5); /* imm<4:2> */
  return 0;
}

/* --- exclude_bit --- */
static const thop_variant_shape SHAPE_EXCLUDE_BIT = {
    .size = THOP_VARIANT_T16,
    .rm_raw_place = {0, 8},
    .rm_con = REG_LOW_REGSET,
    .feat = {.t16 = 1},
};
static const thop_variant VARIANT_EXCLUDE_BIT[] = {{&SHAPE_EXCLUDE_BIT, 0xB400, NULL}};
static const thop_table TABLE_EXCLUDE_BIT = {"exclude_bit", VARIANT_EXCLUDE_BIT, 1};

UT_TEST(test_exclude_bit_clears_bit)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rm = 0x55; /* r0, r2, r4, r6 */
  a.exclude_bit = 2; /* clear bit 2 from rm before placement */
  thumb_opcode op = thop_emit(TABLE_EXCLUDE_BIT.name, TABLE_EXCLUDE_BIT.variants,
                              TABLE_EXCLUDE_BIT.variant_count, a);
  ASSERT_OK(op, 2);
  /* raw placement should have bit 2 cleared: 0x55 & ~0x04 = 0x51 */
  UT_ASSERT_EQ(op.opcode & 0xFF, 0x51);
  return 0;
}

/* ======================================================================== */
/*  11. FALLBACK / MULTI-VARIANT SELECTION                                   */
/* ======================================================================== */

static const thop_variant_shape SHAPE_FALLBACK_T16 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rd_con = REG_LOW_ONLY,
    .feat = {.t16 = 1},
};
static const thop_variant_shape SHAPE_FALLBACK_T32 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_ANY,
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_FALLBACK[] = {
    {&SHAPE_FALLBACK_T16, 0x1000, NULL},
    {&SHAPE_FALLBACK_T32, 0xF0000000, NULL},
};
static const thop_table TABLE_FALLBACK = {"fallback", VARIANT_FALLBACK, 2};

UT_TEST(test_fallback_t16_to_t32)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 8; /* T16 fails (high reg), T32 matches */
  thumb_opcode op = thop_emit(TABLE_FALLBACK.name, TABLE_FALLBACK.variants,
                              TABLE_FALLBACK.variant_count, a);
  ASSERT_OK(op, 4);
  /* Should have used T32 base */
  UT_ASSERT_EQ(op.opcode & 0xF0000000, 0xF0000000);
  return 0;
}

UT_TEST(test_fallback_t16_when_possible)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 3; /* T16 matches */
  thumb_opcode op = thop_emit(TABLE_FALLBACK.name, TABLE_FALLBACK.variants,
                              TABLE_FALLBACK.variant_count, a);
  ASSERT_OK(op, 2);
  /* Should have used T16 base */
  UT_ASSERT_EQ(op.opcode & 0xF000, 0x1000);
  return 0;
}

/* ======================================================================== */
/*  12. COMBINED CONSTRAINTS (real-world patterns)                           */
/* ======================================================================== */

static const thop_variant_shape SHAPE_COMBO_ALU_T32 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {16, 4},
    .rm_place = {0, 4},
    .rd_con = REG_NOT_PC,
    .rn_con = REG_NOT_PC,
    .rm_con = REG_NOT_SP | REG_NOT_PC,
    .has_s_bit = 1,
    .shift_type_bits = {4, 2},
    .shift_imm2_bits = {6, 2},
    .shift_imm3_bits = {12, 3},
    .feat = {.t32 = 1},
};
static const thop_variant VARIANT_COMBO_ALU_T32[] = {{&SHAPE_COMBO_ALU_T32, 0xEA000000, NULL}};
static const thop_table TABLE_COMBO_ALU_T32 = {"combo_alu_t32", VARIANT_COMBO_ALU_T32, 1};

UT_TEST(test_combo_alu_t32_pass)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 0;
  a.rn = 1;
  a.rm = 2;
  a.flags = FLAGS_BEHAVIOUR_SET;
  a.shift = (thumb_shift){.type = THUMB_SHIFT_LSL, .value = 3, .mode = THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = thop_emit(TABLE_COMBO_ALU_T32.name, TABLE_COMBO_ALU_T32.variants,
                              TABLE_COMBO_ALU_T32.variant_count, a);
  ASSERT_OK(op, 4);
  return 0;
}

UT_TEST(test_combo_alu_t32_rd_pc_fail)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 15;
  a.rn = 1;
  a.rm = 2;
  thumb_opcode op = thop_emit(TABLE_COMBO_ALU_T32.name, TABLE_COMBO_ALU_T32.variants,
                              TABLE_COMBO_ALU_T32.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

UT_TEST(test_combo_alu_t32_rm_sp_fail)
{
  setup_full_features();
  thop_args a = args_zero();
  a.rd = 0;
  a.rn = 1;
  a.rm = 13; /* SP */
  thumb_opcode op = thop_emit(TABLE_COMBO_ALU_T32.name, TABLE_COMBO_ALU_T32.variants,
                              TABLE_COMBO_ALU_T32.variant_count, a);
  ASSERT_FAIL(op);
  return 0;
}

/* ======================================================================== */
/*  SUITE                                                                    */
/* ======================================================================== */

UT_SUITE(thop_constraints)
{
  /* register constraints */
  UT_RUN(test_reg_low_only_pass);
  UT_RUN(test_reg_low_only_fail_r8);
  UT_RUN(test_reg_low_only_fail_r15);
  UT_RUN(test_reg_not_sp_pass);
  UT_RUN(test_reg_not_sp_fail_r13);
  UT_RUN(test_reg_not_pc_pass);
  UT_RUN(test_reg_not_pc_fail_r15);
  UT_RUN(test_reg_not_lr_pass);
  UT_RUN(test_reg_not_lr_fail_r14);
  UT_RUN(test_reg_sp_only_pass);
  UT_RUN(test_reg_sp_only_fail_r12);
  UT_RUN(test_reg_pc_only_pass);
  UT_RUN(test_reg_pc_only_fail_r14);
  UT_RUN(test_reg_combined_mask_pass);
  UT_RUN(test_reg_combined_mask_fail_high_reg);

  /* equality constraints */
  UT_RUN(test_reg_eq_rn_pass);
  UT_RUN(test_reg_eq_rn_fail_mismatch);
  UT_RUN(test_reg_eq_rn_fail_rd_not_low);
  UT_RUN(test_reg_eq_rm_pass);
  UT_RUN(test_reg_eq_rm_fail_mismatch);

  /* register-list constraints */
  UT_RUN(test_reg_low_regset_pass);
  UT_RUN(test_reg_low_regset_fail_bit8);
  UT_RUN(test_reg_rm_bit_not_sp_pass);
  UT_RUN(test_reg_rm_bit_not_sp_fail);
  UT_RUN(test_reg_rm_bits_not_lr_pc_pass);
  UT_RUN(test_reg_rm_bits_not_lr_pc_fail_lr);
  UT_RUN(test_reg_rm_bits_not_lr_pc_fail_pc);

  /* encoding enforcement */
  UT_RUN(test_enc_none_prefers_first);
  UT_RUN(test_enc_force_16bit_ok);
  UT_RUN(test_enc_force_16bit_fail);
  UT_RUN(test_enc_force_32bit_ok);
  UT_RUN(test_enc_force_32bit_only_table);

  /* feature gating */
  UT_RUN(test_feat_dsp_present);
  UT_RUN(test_feat_dsp_missing);
  UT_RUN(test_feat_div_present);
  UT_RUN(test_feat_div_missing);

  /* S-bit / IT-block */
  UT_RUN(test_sbit_has_s_bit_not_set);
  UT_RUN(test_sbit_has_s_bit_set);
  UT_RUN(test_sbit_set_but_no_s_bit_support);
  UT_RUN(test_implicit_s_outside_it);
  UT_RUN(test_implicit_s_inside_it);
  UT_RUN(test_forbid_s_in_it_outside_it);
  UT_RUN(test_forbid_s_in_it_inside_it);

  /* shift constraints */
  UT_RUN(test_shift_lsl_allowed);
  UT_RUN(test_shift_lsr_rejected);
  UT_RUN(test_shift_none_allowed_when_fields_present);
  UT_RUN(test_shift_any_rejected_when_no_fields);

  /* PUW constraints */
  UT_RUN(test_puw_fixed_match);
  UT_RUN(test_puw_fixed_mismatch);
  UT_RUN(test_puw_bits_any_value);

  /* immediate validation */
  UT_RUN(test_imm_raw_width_pass);
  UT_RUN(test_imm_raw_width_fail);
  UT_RUN(test_imm_raw_scaled_pass);
  UT_RUN(test_imm_raw_scaled_fail_not_aligned);
  UT_RUN(test_imm_raw_scaled_fail_too_big);
  UT_RUN(test_imm_raw_signed_pass_negative);
  UT_RUN(test_imm_raw_signed_fail_positive);
  UT_RUN(test_imm_raw_signed_fail_zero);
  UT_RUN(test_imm_none_zero_ok);
  UT_RUN(test_imm_none_any_value_ignored);
  UT_RUN(test_imm_pack_const_zero);
  UT_RUN(test_imm_pack_const_valid);
  UT_RUN(test_imm_pack_const_invalid);
  UT_RUN(test_imm_pack_3_8_1_pass);
  UT_RUN(test_imm_pack_3_8_1_fail_too_big);

  /* special placement fields */
  UT_RUN(test_has_rd_hi_low);
  UT_RUN(test_has_rd_hi_high);
  UT_RUN(test_dn_rd_split_r8);
  UT_RUN(test_dn_rd_split_r12);
  UT_RUN(test_split_imm_placement);
  UT_RUN(test_exclude_bit_clears_bit);

  /* fallback / multi-variant */
  UT_RUN(test_fallback_t16_to_t32);
  UT_RUN(test_fallback_t16_when_possible);

  /* combined constraints */
  UT_RUN(test_combo_alu_t32_pass);
  UT_RUN(test_combo_alu_t32_rd_pc_fail);
  UT_RUN(test_combo_alu_t32_rm_sp_fail);
}

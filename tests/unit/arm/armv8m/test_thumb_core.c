/*
 *  test_thumb_core.c - suite for arch/arm/thumb/thumb.c
 *
 *  Unlike the other test_thop_*.c suites (which each cover one opcode
 *  encoder's th_<mnemonic>_* entry points), arch/arm/thumb/thumb.c itself
 *  contains no opcode encoders. It is shared infrastructure that every
 *  thop_*.c encoder links against:
 *
 *    - Feature-profile resolution: thumb_resolve_features() / thumb_resolve_fpu()
 *      (turns -march=/-mfpu=/-mextension= strings into a thop_feat bitset).
 *    - thop_emit_error(): the diagnostic "no variant matched" path that
 *      thop_emit() (inline in thumb.h, exercised indirectly by every
 *      test_thop_*.c file) falls through to on failure.
 *    - Bit-packing / branch-encoding utilities used by arm-thumb-gen.c and
 *      arm-thumb-asm.c to patch branch targets after layout:
 *      th_pack_const, th_packimm_3_8_1, th_packimm_10_11_0, th_encbranch*,
 *      th_shift_type_to_op, th_shift_value_to_sr_type,
 *      th_generic_op_reg_shift_with_status.
 *    - th_sym_t/th_sym_d ELF `$t`/`$d` mapping-symbol emitters.
 *
 *  Oracle values for th_pack_const/th_packimm_3_8_1 were cross-checked
 *  against `arm-none-eabi-as -march=armv8-m.main` disassembly of the
 *  corresponding mov.w/movw encodings (see docs/plan_ut_next_steps.md
 *  authoring contract: oracle asserts, not characterization).
 */

#define USING_GLOBALS
#include "arch/arm/thumb/thumb.h"
#include "ut.h"

/* ------------------------------------------------------------------ helpers */

static void setup_armv8m_main(void)
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m33",
      .feat = (thop_feat){
          .t16 = 1, .t32 = 1, .it = 1, .mod_imm = 1, .movw_movt = 1,
          .bfx = 1, .clz_rbit = 1, .tbb_tbh = 1, .cbz = 1, .sat = 1, .div = 1,
          .dsp = 1, .ldaex = 1,
      },
      .is_secure_tz = false,
  };
}

/* ============================================================ */
/*  thumb_resolve_features() / thumb_resolve_fpu()               */
/* ============================================================ */

UT_TEST(test_resolve_features_null_march_defaults_to_v8m_main)
{
  /* thop_feats_from_march(NULL) returns THOP_PROFILE_ARMV8M_MAIN_CORE. */
  thop_feat f = thumb_resolve_features(NULL, NULL, 0);
  UT_ASSERT_EQ(f.t16, 1);
  UT_ASSERT_EQ(f.t32, 1);
  UT_ASSERT_EQ(f.mod_imm, 1);
  UT_ASSERT_EQ(f.movw_movt, 1);
  UT_ASSERT_EQ(f.bfx, 1);
  UT_ASSERT_EQ(f.dsp, 1);
  UT_ASSERT_EQ(f.ldaex, 1);
  UT_ASSERT_EQ(f.fp_armv8, 1);
  UT_ASSERT_EQ(f.vfp_sp, 0); /* no FP unit unless -mfpu given */
  return 0;
}

UT_TEST(test_resolve_features_armv6m_core)
{
  thop_feat f = thumb_resolve_features("armv6-m", NULL, 0);
  UT_ASSERT_EQ(f.t16, 1);
  UT_ASSERT_EQ(f.t32, 0);
  UT_ASSERT_EQ(f.it, 0);
  UT_ASSERT_EQ(f.mod_imm, 0);
  return 0;
}

UT_TEST(test_resolve_features_armv7em_core_has_dsp)
{
  thop_feat f = thumb_resolve_features("armv7e-m", NULL, 0);
  UT_ASSERT_EQ(f.t16, 1);
  UT_ASSERT_EQ(f.t32, 1);
  UT_ASSERT_EQ(f.dsp, 1);
  UT_ASSERT_EQ(f.fp_armv8, 0); /* only armv8-m.main+ has fp_armv8 baked in */
  return 0;
}

UT_TEST(test_resolve_features_armv8m_base_core)
{
  thop_feat f = thumb_resolve_features("armv8-m.base", NULL, 0);
  UT_ASSERT_EQ(f.t16, 1);
  UT_ASSERT_EQ(f.t32, 0);
  UT_ASSERT_EQ(f.movw_movt, 1);
  UT_ASSERT_EQ(f.cbz, 1);
  UT_ASSERT_EQ(f.ldaex, 1);
  return 0;
}

UT_TEST(test_resolve_features_armv81m_main_has_lob)
{
  thop_feat f = thumb_resolve_features("armv8.1-m.main", NULL, 0);
  UT_ASSERT_EQ(f.lob, 1);
  UT_ASSERT_EQ(f.fp_armv8, 1);
  return 0;
}

UT_TEST(test_resolve_features_plus_dsp_extension)
{
  /* base profile (armv8-m.base) has no dsp; "+dsp" ORs it in. */
  thop_feat f = thumb_resolve_features("armv8-m.base+dsp", NULL, 0);
  UT_ASSERT_EQ(f.dsp, 1);
  UT_ASSERT_EQ(f.t32, 0); /* rest of the base profile is untouched */
  return 0;
}

UT_TEST(test_resolve_features_plus_fp_extension)
{
  thop_feat f = thumb_resolve_features("armv7-m+fp", NULL, 0);
  UT_ASSERT_EQ(f.vfp_sp, 1);
  UT_ASSERT_EQ(f.vfp_dp, 0);
  return 0;
}

UT_TEST(test_resolve_features_plus_fp_dp_extension)
{
  thop_feat f = thumb_resolve_features("armv7-m+fp.dp", NULL, 0);
  UT_ASSERT_EQ(f.vfp_sp, 1);
  UT_ASSERT_EQ(f.vfp_dp, 1);
  return 0;
}

UT_TEST(test_resolve_features_plus_mve_fp_extension)
{
  /* +mve.fp sets both mve_int and mve_fp. thumb_resolve_features() errors
   * out (tcc_error -> abort) if mve_fp is requested without an FP unit, so
   * an -mfpu= must be supplied alongside it here. */
  thop_feat f = thumb_resolve_features("armv8.1-m.main+mve.fp", "fpv5-d16", 0);
  UT_ASSERT_EQ(f.mve_int, 1);
  UT_ASSERT_EQ(f.mve_fp, 1);
  UT_ASSERT_EQ(f.vfp_sp, 1);
  return 0;
}

UT_TEST(test_resolve_features_plus_multiple_extensions_chained)
{
  thop_feat f = thumb_resolve_features("armv8-m.main+dsp+sec+lob", NULL, 0);
  UT_ASSERT_EQ(f.dsp, 1); /* already 1 from the base profile too */
  UT_ASSERT_EQ(f.sec, 1);
  UT_ASSERT_EQ(f.lob, 1);
  return 0;
}

UT_TEST(test_resolve_features_extra_feat_bits_ored_in)
{
  /* extra_feat_bits is OR'd on top of the march-derived profile. Use the
   * 'pacbti' bit (bit 19 per thop_feat_bit_name) as a marker that armv8-m.main
   * itself does not set. */
  thop_feat base = thumb_resolve_features("armv8-m.main", NULL, 0);
  UT_ASSERT_EQ(base.pacbti, 0);

  uint64_t pacbti_bit = 1ull << 19;
  thop_feat f = thumb_resolve_features("armv8-m.main", NULL, pacbti_bit);
  UT_ASSERT_EQ(f.pacbti, 1);
  /* march-derived bits are preserved alongside the extra bit */
  UT_ASSERT_EQ(f.t32, 1);
  return 0;
}

UT_TEST(test_resolve_features_mfpu_ored_on_top_of_march)
{
  thop_feat f = thumb_resolve_features("armv8-m.main", "fpv5-d16", 0);
  UT_ASSERT_EQ(f.vfp_sp, 1);
  UT_ASSERT_EQ(f.vfp_dp, 1);
  UT_ASSERT_EQ(f.fp_armv8, 1); /* already 1 from the base profile too */
  UT_ASSERT_EQ(f.t32, 1);     /* core features preserved */
  return 0;
}

UT_TEST(test_resolve_features_mfpu_none_leaves_no_fp)
{
  thop_feat f = thumb_resolve_features("armv8-m.main", "none", 0);
  UT_ASSERT_EQ(f.vfp_sp, 0);
  UT_ASSERT_EQ(f.vfp_dp, 0);
  return 0;
}

UT_TEST(test_resolve_fpu_vfpv4_sp_d16_aliases)
{
  /* Both spellings ("vfpv4-sp-d16" and "fpv4-sp-d16") map to the same bundle. */
  thop_feat a = thumb_resolve_fpu("vfpv4-sp-d16");
  thop_feat b = thumb_resolve_fpu("fpv4-sp-d16");
  UT_ASSERT_EQ(a.vfp_sp, 1);
  UT_ASSERT_EQ(a.fp_armv8, 0);
  UT_ASSERT_EQ(thop_feat_bits(a), thop_feat_bits(b));
  return 0;
}

UT_TEST(test_resolve_fpu_fpv5_sp_d16_has_fp_armv8)
{
  thop_feat f = thumb_resolve_fpu("fpv5-sp-d16");
  UT_ASSERT_EQ(f.vfp_sp, 1);
  UT_ASSERT_EQ(f.vfp_dp, 0);
  UT_ASSERT_EQ(f.fp_armv8, 1);
  return 0;
}

UT_TEST(test_resolve_fpu_fpv5_d32_has_d32_flag)
{
  thop_feat f = thumb_resolve_fpu("fpv5-d32");
  UT_ASSERT_EQ(f.vfp_dp, 1);
  UT_ASSERT_EQ(f.fp_dp_d32, 1);
  UT_ASSERT_EQ(f.fp16, 0);
  return 0;
}

UT_TEST(test_resolve_fpu_fp_armv8_full_has_fp16)
{
  thop_feat f = thumb_resolve_fpu("fp-armv8-full");
  UT_ASSERT_EQ(f.vfp_sp, 1);
  UT_ASSERT_EQ(f.vfp_dp, 1);
  UT_ASSERT_EQ(f.fp_dp_d32, 1);
  UT_ASSERT_EQ(f.fp16, 1);
  return 0;
}

UT_TEST(test_resolve_fpu_none_string_and_null_are_equivalent)
{
  thop_feat a = thumb_resolve_fpu(NULL);
  thop_feat b = thumb_resolve_fpu("none");
  UT_ASSERT_EQ(thop_feat_bits(a), 0);
  UT_ASSERT_EQ(thop_feat_bits(a), thop_feat_bits(b));
  return 0;
}

UT_TEST(test_resolve_fpu_does_not_fold_in_core_features)
{
  /* Unlike thumb_resolve_features(), thumb_resolve_fpu() must not pull in
   * any t16/t32/mod_imm/etc core bits -- only FP-unit bits. */
  thop_feat f = thumb_resolve_fpu("fpv5-d16");
  UT_ASSERT_EQ(f.t16, 0);
  UT_ASSERT_EQ(f.t32, 0);
  UT_ASSERT_EQ(f.mod_imm, 0);
  return 0;
}

/* ============================================================ */
/*  thop_emit_error() -- the no-match diagnostic path             */
/* ============================================================ */

static const thop_variant_shape SHAPE_ERR_LOW_ONLY = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rd_con = REG_LOW_ONLY,
    .feat = {.t16 = 1},
};
static const thop_variant VARIANT_ERR_LOW_ONLY[] = {{&SHAPE_ERR_LOW_ONLY, 0x0000, NULL}};

static const thop_variant_shape SHAPE_ERR_FEAT_DSP = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_ANY,
    .feat = {.t32 = 1, .dsp = 1},
};
static const thop_variant VARIANT_ERR_FEAT_DSP[] = {{&SHAPE_ERR_FEAT_DSP, 0xFA000000, NULL}};

UT_TEST(test_emit_error_returns_zero_opcode_reg_constraint_fail)
{
  setup_armv8m_main();
  thop_args a = {.rd = 8, /* high reg, table requires LOW_ONLY */
                .shift = THUMB_SHIFT_DEFAULT,
                .enc = ENFORCE_ENCODING_NONE,
                .flags = FLAGS_BEHAVIOUR_NOT_IMPORTANT};
  thumb_opcode op = thop_emit_error("test_low_only", VARIANT_ERR_LOW_ONLY, 1, a);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);
  return 0;
}

UT_TEST(test_emit_error_returns_zero_opcode_feature_mismatch)
{
  /* No dsp feature -> the missing-feature diagnostic branch executes
   * (thop_feat_describe_missing), still returns a zero opcode. */
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m0",
      .feat = (thop_feat){.t16 = 1},
      .is_secure_tz = false,
  };
  thop_args a = {.rd = 0,
                .shift = THUMB_SHIFT_DEFAULT,
                .enc = ENFORCE_ENCODING_NONE,
                .flags = FLAGS_BEHAVIOUR_NOT_IMPORTANT};
  thumb_opcode op = thop_emit_error("test_feat_dsp", VARIANT_ERR_FEAT_DSP, 1, a);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);
  return 0;
}

UT_TEST(test_emit_error_empty_table)
{
  setup_armv8m_main();
  thop_args a = {.shift = THUMB_SHIFT_DEFAULT,
                .enc = ENFORCE_ENCODING_NONE,
                .flags = FLAGS_BEHAVIOUR_NOT_IMPORTANT};
  thumb_opcode op = thop_emit_error("test_empty", VARIANT_ERR_LOW_ONLY, 0, a);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);
  return 0;
}

UT_TEST(test_emit_error_reached_via_thop_emit_fallthrough)
{
  /* thop_emit() (inline, thumb.h) itself falls through to thop_emit_error()
   * when no variant matches -- confirm the two paths agree on the result. */
  setup_armv8m_main();
  thop_args a = {.rd = 8,
                .shift = THUMB_SHIFT_DEFAULT,
                .enc = ENFORCE_ENCODING_NONE,
                .flags = FLAGS_BEHAVIOUR_NOT_IMPORTANT};
  thumb_opcode op = thop_emit("test_low_only", VARIANT_ERR_LOW_ONLY, 1, a);
  UT_ASSERT_EQ(op.size, 0);
  UT_ASSERT_EQ(op.opcode, 0);
  return 0;
}

/* ============================================================ */
/*  th_pack_const() -- ARMv7-M modified-immediate encoder        */
/*  Oracle values cross-checked against arm-none-eabi-as mov.w    */
/*  disassembly (see file header comment).                        */
/* ============================================================ */

UT_TEST(test_pack_const_plain_byte)
{
  /* 00000000 00000000 00000000 abcdefgh -> packed == imm itself */
  UT_ASSERT_EQ(th_pack_const(0xFF), 0xFF);
  UT_ASSERT_EQ(th_pack_const(0x01), 0x01);
  UT_ASSERT_EQ(th_pack_const(0x00), 0x00);
  return 0;
}

UT_TEST(test_pack_const_00xy00xy_pattern)
{
  /* 00000000 abcdefgh 00000000 abcdefgh -> (1<<12) | byte */
  UT_ASSERT_EQ(th_pack_const(0x00AB00AB), (1u << 12) | 0xAB);
  return 0;
}

UT_TEST(test_pack_const_xy00xy00_pattern)
{
  /* abcdefgh 00000000 abcdefgh 00000000 -> (2<<12) | byte */
  UT_ASSERT_EQ(th_pack_const(0xAB00AB00), (2u << 12) | 0xAB);
  return 0;
}

UT_TEST(test_pack_const_xyxyxyxy_pattern)
{
  /* abcdefgh abcdefgh abcdefgh abcdefgh -> (3<<12) | byte */
  UT_ASSERT_EQ(th_pack_const(0xABABABAB), (3u << 12) | 0xAB);
  return 0;
}

UT_TEST(test_pack_const_rotated_msb_byte)
{
  /* 0xFF000000 -- verified against `mov.w r0,#0xFF000000` -> f04f 407f */
  UT_ASSERT_EQ(th_pack_const(0xFF000000), 0x407f);
  return 0;
}

UT_TEST(test_pack_const_rotated_top_bit_only)
{
  /* 0x80000000 -- verified against `mov.w r0,#0x80000000` -> f04f 4000 */
  UT_ASSERT_EQ(th_pack_const(0x80000000), 0x4000);
  return 0;
}

UT_TEST(test_pack_const_rotated_mid_value)
{
  /* 0x100 -- verified against `mov.w r0,#0x100` -> f44f 7080 */
  UT_ASSERT_EQ(th_pack_const(0x100), 0x04007080u);
  return 0;
}

UT_TEST(test_pack_const_unrepresentable_returns_zero)
{
  /* 0x12345678 is not a valid ARMv7-M modified immediate in any of the 4
   * families -- th_pack_const signals this the same way as "value 0",
   * which callers (thop_try_imm) disambiguate via `imm != 0`. */
  UT_ASSERT_EQ(th_pack_const(0x12345678), 0);
  return 0;
}

UT_TEST(test_pack_const_cache_returns_consistent_value_on_repeat)
{
  /* th_pack_const memoizes by imm in a direct-mapped cache; calling twice
   * with the same value (and an intervening different value, to guard
   * against a stale "the loop just never advanced" false-pass) must
   * return the identical packed result both times. */
  uint32_t first = th_pack_const(0xAB00AB00);
  uint32_t other = th_pack_const(0xFF000000);
  uint32_t second = th_pack_const(0xAB00AB00);
  UT_ASSERT_EQ(first, second);
  UT_ASSERT_EQ(first, (2u << 12) | 0xAB);
  UT_ASSERT_EQ(other, 0x407f);
  return 0;
}

/* ============================================================ */
/*  th_packimm_3_8_1() -- movw/adr scattered 12-bit immediate     */
/* ============================================================ */

UT_TEST(test_packimm_3_8_1_matches_movw_disassembly)
{
  /* movw r0, #0x1234 -> f241 2034; low halfword bits[15:0] (2034) plus
   * i (bit 26 of the 32-bit word, placed in bit 10 of the high halfword)
   * must equal th_packimm_3_8_1(0x1234). Cross-checked against
   * arm-none-eabi-as -march=armv8-m.main output. */
  uint32_t v = th_packimm_3_8_1(0x1234);
  UT_ASSERT_EQ(v, 0x00012034u);
  return 0;
}

UT_TEST(test_packimm_3_8_1_zero)
{
  UT_ASSERT_EQ(th_packimm_3_8_1(0), 0);
  return 0;
}

UT_TEST(test_packimm_3_8_1_max_16bit)
{
  /* imm4=0xf, i=1, imm3=7, imm8=0xff -> (1<<26)|(0xf<<16)|(7<<12)|0xff */
  uint32_t v = th_packimm_3_8_1(0xFFFF);
  uint32_t expect = (1u << 26) | (0xFu << 16) | (7u << 12) | 0xFFu;
  UT_ASSERT_EQ(v, expect);
  return 0;
}

UT_TEST(test_packimm_3_8_1_field_isolation)
{
  /* Each field only carries its own bits: imm8 alone. */
  uint32_t v = th_packimm_3_8_1(0x00FF);
  UT_ASSERT_EQ(v, 0xFFu);
  return 0;
}

/* ============================================================ */
/*  th_packimm_10_11_0() -- BL/B.W T4 S:I1:I2:imm10:imm11 packing */
/* ============================================================ */

UT_TEST(test_packimm_10_11_0_zero)
{
  /* imm=0 -> s=0, j1=~(0^0)&1=1, j2=~(0^0)&1=1, everything else 0 */
  uint32_t v = th_packimm_10_11_0(0);
  uint32_t expect = (1u << 13) | (1u << 11);
  UT_ASSERT_EQ(v, expect);
  return 0;
}

UT_TEST(test_packimm_10_11_0_positive_offset_bits)
{
  /* imm = 0x100000 (bit 20 set): s = bit24 = 0, imm10 = (imm>>12)&0x3ff
   * = 0x100, imm11 = (imm>>1)&0x7ff = 0. j1 = ~(bit23^s)&1 = 1,
   * j2 = ~(bit22^s)&1 = 1 (bits 22/23 are 0, s is 0). */
  uint32_t imm = 0x100000;
  uint32_t v = th_packimm_10_11_0(imm);
  uint32_t expect = (0x100u << 16) | (1u << 13) | (1u << 11) | 0u;
  UT_ASSERT_EQ(v, expect);
  return 0;
}

UT_TEST(test_packimm_10_11_0_sign_bit_flips_j1_j2)
{
  /* Setting the sign bit (bit 24) alone: s=1, bits22/23=0 so
   * j1 = ~(0^1)&1 = 0, j2 = ~(0^1)&1 = 0. */
  uint32_t imm = (1u << 24);
  uint32_t v = th_packimm_10_11_0(imm);
  uint32_t expect = (1u << 26); /* s in bit 26, j1/j2/imm10/imm11 all 0 */
  UT_ASSERT_EQ(v, expect);
  return 0;
}

/* ============================================================ */
/*  th_encbranch / th_encbranch_8 / th_encbranch_11 / th_encbranch_20 */
/* ============================================================ */

UT_TEST(test_encbranch_basic_forward)
{
  /* th_encbranch returns a raw byte delta: addr - pos - 4 (PC-relative,
   * PC reads as pos+4 on ARM/Thumb). */
  UT_ASSERT_EQ(th_encbranch(0, 8), 4);
  return 0;
}

UT_TEST(test_encbranch_basic_backward)
{
  UT_ASSERT_EQ((int32_t)th_encbranch(100, 0), -104);
  return 0;
}

UT_TEST(test_encbranch_8_halfword_scaled)
{
  /* th_encbranch_8: (addr-pos-4)>>1, masked to 8 bits (CBZ-style short
   * conditional branch displacement encoding). */
  UT_ASSERT_EQ(th_encbranch_8(0, 10), 3); /* (10-0-4)>>1 = 3 */
  return 0;
}

UT_TEST(test_encbranch_8_negative_wraps_into_byte)
{
  /* addr behind pos: (addr-pos-4)>>1 = -6, masked with & 0xff */
  uint32_t v = th_encbranch_8(20, 0);
  UT_ASSERT_EQ(v, (uint32_t)(((0 - 20 - 4) >> 1) & 0xff));
  return 0;
}

UT_TEST(test_encbranch_11_halfword_scaled)
{
  UT_ASSERT_EQ(th_encbranch_11(0, 20), 8); /* (20-0-4)>>1 = 8 */
  return 0;
}

UT_TEST(test_encbranch_11_masks_to_11_bits)
{
  uint32_t v = th_encbranch_11(0, 2000);
  uint32_t expect = (uint32_t)(((2000 - 0 - 4) >> 1) & 0x7ff);
  UT_ASSERT_EQ(v, expect);
  return 0;
}

UT_TEST(test_encbranch_20_halfword_scaled_no_mask)
{
  /* th_encbranch_20 does not mask -- it hands the raw halfword-scaled
   * signed delta to th_encbranch_b_t3/th_packimm_10_11_0 for field
   * packing. */
  UT_ASSERT_EQ(th_encbranch_20(0, 100), 48); /* (100-0-4)>>1 = 48 */
  return 0;
}

UT_TEST(test_encbranch_20_negative)
{
  UT_ASSERT_EQ((int32_t)th_encbranch_20(100, 0), (int32_t)((0 - 100 - 4) >> 1));
  return 0;
}

/* ============================================================ */
/*  th_encbranch_b_t3() -- Bcc.W T3 S:J1:J2:imm6:imm11 packing     */
/* ============================================================ */

UT_TEST(test_encbranch_b_t3_zero)
{
  uint32_t v = th_encbranch_b_t3(0);
  UT_ASSERT_EQ(v, 0);
  return 0;
}

UT_TEST(test_encbranch_b_t3_low_imm11_only)
{
  /* imm11 = bits[10:0] land directly in the low 11 bits of the low half. */
  uint32_t v = th_encbranch_b_t3(0x7FF);
  UT_ASSERT_EQ(v & 0x7FF, 0x7FF);
  UT_ASSERT_EQ((v >> 16) & 0xFFFF, 0); /* imm6/s untouched */
  return 0;
}

UT_TEST(test_encbranch_b_t3_imm6_field)
{
  /* imm6 = bits[16:11] of the input land at bits[26:16][5:0] (a=(s<<10)|imm6,
   * placed at bit 16 of the result). */
  uint32_t imm = 0x3F << 11; /* imm6 = 0x3f, s=0, everything else 0 */
  uint32_t v = th_encbranch_b_t3(imm);
  UT_ASSERT_EQ((v >> 16) & 0x3FF, 0x3F);
  UT_ASSERT_EQ(v & 0xFFFF, 0);
  return 0;
}

UT_TEST(test_encbranch_b_t3_sign_bit_sets_s_and_a)
{
  uint32_t imm = 1u << 19; /* s bit */
  uint32_t v = th_encbranch_b_t3(imm);
  UT_ASSERT_EQ((v >> 16) & 0x400, 0x400); /* s placed at bit 10 of the 'a' halfword */
  return 0;
}

/* ============================================================ */
/*  th_shift_type_to_op() / th_shift_value_to_sr_type()           */
/* ============================================================ */

UT_TEST(test_shift_type_to_op_all_known_values)
{
  UT_ASSERT_EQ(th_shift_type_to_op((thumb_shift){.type = THUMB_SHIFT_ASR}), 4);
  UT_ASSERT_EQ(th_shift_type_to_op((thumb_shift){.type = THUMB_SHIFT_LSL}), 2);
  UT_ASSERT_EQ(th_shift_type_to_op((thumb_shift){.type = THUMB_SHIFT_LSR}), 3);
  UT_ASSERT_EQ(th_shift_type_to_op((thumb_shift){.type = THUMB_SHIFT_ROR}), 7);
  return 0;
}

UT_TEST(test_shift_value_to_sr_type_none_and_lsl_are_zero)
{
  UT_ASSERT_EQ(th_shift_value_to_sr_type((thumb_shift){.type = THUMB_SHIFT_NONE}), 0);
  UT_ASSERT_EQ(th_shift_value_to_sr_type((thumb_shift){.type = THUMB_SHIFT_LSL}), 0);
  return 0;
}

UT_TEST(test_shift_value_to_sr_type_lsr_asr)
{
  UT_ASSERT_EQ(th_shift_value_to_sr_type((thumb_shift){.type = THUMB_SHIFT_LSR}), 1);
  UT_ASSERT_EQ(th_shift_value_to_sr_type((thumb_shift){.type = THUMB_SHIFT_ASR}), 2);
  return 0;
}

UT_TEST(test_shift_value_to_sr_type_ror_and_rrx_share_encoding)
{
  /* ROR and RRX both encode as sr_type==3; RRX is a degenerate ROR #1
   * with no separate hardware shift-type code on Thumb-2. */
  UT_ASSERT_EQ(th_shift_value_to_sr_type((thumb_shift){.type = THUMB_SHIFT_ROR}), 3);
  UT_ASSERT_EQ(th_shift_value_to_sr_type((thumb_shift){.type = THUMB_SHIFT_RRX}), 3);
  return 0;
}

/* ============================================================ */
/*  th_generic_op_reg_shift_with_status()                         */
/*  Oracle cross-checked against `add.w r0, r1, r2[, lsl #3]`     */
/*  disassembly (see file header comment).                        */
/* ============================================================ */

UT_TEST(test_generic_op_reg_shift_no_shift_no_status)
{
  /* add.w r0, r1, r2 -> eb01 0002 */
  thumb_opcode op = th_generic_op_reg_shift_with_status(
      0xEB00, 0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEB010002u);
  return 0;
}

UT_TEST(test_generic_op_reg_shift_with_lsl_shift)
{
  /* add.w r0, r1, r2, lsl #3 -> eb01 00c2 */
  thumb_shift shift = {.type = THUMB_SHIFT_LSL, .value = 3, .mode = THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_generic_op_reg_shift_with_status(
      0xEB00, 0, 1, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, shift);
  UT_ASSERT_EQ(op.size, 4);
  UT_ASSERT_EQ(op.opcode, 0xEB0100C2u);
  return 0;
}

UT_TEST(test_generic_op_reg_shift_sets_status_bit_20)
{
  thumb_opcode op = th_generic_op_reg_shift_with_status(
      0xEB00, 0, 1, 2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT);
  UT_ASSERT_EQ((op.opcode >> 20) & 1, 1);
  return 0;
}

UT_TEST(test_generic_op_reg_shift_asr_shift_encodes_sr2)
{
  /* asr shift type -> sr=2 placed at bits [6:4]. */
  thumb_shift shift = {.type = THUMB_SHIFT_ASR, .value = 5, .mode = THUMB_SHIFT_IMMEDIATE};
  thumb_opcode op = th_generic_op_reg_shift_with_status(
      0xEB00, 3, 4, 5, FLAGS_BEHAVIOUR_NOT_IMPORTANT, shift);
  UT_ASSERT_EQ((op.opcode >> 4) & 0x3, 2);
  /* imm3:imm2 = 5 -> imm2=1 (bits [7:6]), imm3=1 (bits [14:12]) */
  UT_ASSERT_EQ((op.opcode >> 6) & 0x3, 1);
  UT_ASSERT_EQ((op.opcode >> 12) & 0x7, 1);
  return 0;
}


/* ============================================================ */
/*  th_sym_t() / th_sym_d() -- ELF $t/$d mapping symbols          */
/*  set_elf_sym is stubbed in stubs.c to ignore its Section*      */
/*  argument and always return 0, so these are safe to call       */
/*  directly without a real ELF Section/symtab.                   */
/* ============================================================ */

UT_TEST(test_sym_t_does_not_crash)
{
  th_sym_t();
  return 0;
}

UT_TEST(test_sym_d_does_not_crash)
{
  th_sym_d();
  return 0;
}

/* ======================================================================== */

UT_SUITE(thumb_core)
{
  UT_RUN(test_resolve_features_null_march_defaults_to_v8m_main);
  UT_RUN(test_resolve_features_armv6m_core);
  UT_RUN(test_resolve_features_armv7em_core_has_dsp);
  UT_RUN(test_resolve_features_armv8m_base_core);
  UT_RUN(test_resolve_features_armv81m_main_has_lob);
  UT_RUN(test_resolve_features_plus_dsp_extension);
  UT_RUN(test_resolve_features_plus_fp_extension);
  UT_RUN(test_resolve_features_plus_fp_dp_extension);
  UT_RUN(test_resolve_features_plus_mve_fp_extension);
  UT_RUN(test_resolve_features_plus_multiple_extensions_chained);
  UT_RUN(test_resolve_features_extra_feat_bits_ored_in);
  UT_RUN(test_resolve_features_mfpu_ored_on_top_of_march);
  UT_RUN(test_resolve_features_mfpu_none_leaves_no_fp);
  UT_RUN(test_resolve_fpu_vfpv4_sp_d16_aliases);
  UT_RUN(test_resolve_fpu_fpv5_sp_d16_has_fp_armv8);
  UT_RUN(test_resolve_fpu_fpv5_d32_has_d32_flag);
  UT_RUN(test_resolve_fpu_fp_armv8_full_has_fp16);
  UT_RUN(test_resolve_fpu_none_string_and_null_are_equivalent);
  UT_RUN(test_resolve_fpu_does_not_fold_in_core_features);

  UT_RUN(test_emit_error_returns_zero_opcode_reg_constraint_fail);
  UT_RUN(test_emit_error_returns_zero_opcode_feature_mismatch);
  UT_RUN(test_emit_error_empty_table);
  UT_RUN(test_emit_error_reached_via_thop_emit_fallthrough);

  UT_RUN(test_pack_const_plain_byte);
  UT_RUN(test_pack_const_00xy00xy_pattern);
  UT_RUN(test_pack_const_xy00xy00_pattern);
  UT_RUN(test_pack_const_xyxyxyxy_pattern);
  UT_RUN(test_pack_const_rotated_msb_byte);
  UT_RUN(test_pack_const_rotated_top_bit_only);
  UT_RUN(test_pack_const_rotated_mid_value);
  UT_RUN(test_pack_const_unrepresentable_returns_zero);
  UT_RUN(test_pack_const_cache_returns_consistent_value_on_repeat);

  UT_RUN(test_packimm_3_8_1_matches_movw_disassembly);
  UT_RUN(test_packimm_3_8_1_zero);
  UT_RUN(test_packimm_3_8_1_max_16bit);
  UT_RUN(test_packimm_3_8_1_field_isolation);

  UT_RUN(test_packimm_10_11_0_zero);
  UT_RUN(test_packimm_10_11_0_positive_offset_bits);
  UT_RUN(test_packimm_10_11_0_sign_bit_flips_j1_j2);

  UT_RUN(test_encbranch_basic_forward);
  UT_RUN(test_encbranch_basic_backward);
  UT_RUN(test_encbranch_8_halfword_scaled);
  UT_RUN(test_encbranch_8_negative_wraps_into_byte);
  UT_RUN(test_encbranch_11_halfword_scaled);
  UT_RUN(test_encbranch_11_masks_to_11_bits);
  UT_RUN(test_encbranch_20_halfword_scaled_no_mask);
  UT_RUN(test_encbranch_20_negative);

  UT_RUN(test_encbranch_b_t3_zero);
  UT_RUN(test_encbranch_b_t3_low_imm11_only);
  UT_RUN(test_encbranch_b_t3_imm6_field);
  UT_RUN(test_encbranch_b_t3_sign_bit_sets_s_and_a);

  UT_RUN(test_shift_type_to_op_all_known_values);
  UT_RUN(test_shift_value_to_sr_type_none_and_lsl_are_zero);
  UT_RUN(test_shift_value_to_sr_type_lsr_asr);
  UT_RUN(test_shift_value_to_sr_type_ror_and_rrx_share_encoding);

  UT_RUN(test_generic_op_reg_shift_no_shift_no_status);
  UT_RUN(test_generic_op_reg_shift_with_lsl_shift);
  UT_RUN(test_generic_op_reg_shift_sets_status_bit_20);
  UT_RUN(test_generic_op_reg_shift_asr_shift_encodes_sr2);

  UT_RUN(test_sym_t_does_not_crash);
  UT_RUN(test_sym_d_does_not_crash);
}

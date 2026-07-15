/*
 *  test_arm_target.c - suite for arch/arm/arm.c target init & capability query
 *
 *  Covers:
 *    - arm_target_init(): resolves march/mfpu/mcpu/extra_feat_bits into the
 *      backend-private arm_target_dependent struct and the generic
 *      architecture_config (pointer size, reg counts, fp_reg_count ternary,
 *      march_name default, has_fpu/fpu wiring).
 *    - tcc_target_has(): exhaustive per-capability dispatch from
 *      tcc_target_cap onto the matching thop_feat bit in
 *      arm_target_dependent.feat.
 *
 *  arm_target_init() delegates profile/extension resolution to
 *  thumb_resolve_features() (arch/arm/thumb/thumb.c), which is exercised in
 *  detail elsewhere; here we only need enough march/mfpu combinations to
 *  drive arm.c's own branches (mcpu passthrough, is_secure_tz, march_name
 *  default-when-NULL, and the fp_reg_count 64/32/32/0 ternary chain).
 */

#define USING_GLOBALS
#include "source/backend/arch/arm/arm.h"
#include "source/backend/arch/arm/thumb/thumb.h"
#include "tcc.h"

#include "ut.h"

/* ------------------------------------------------------------------ tests */

UT_TEST(test_arm_target_init_basic_fields_no_fpu)
{
  arm_target_init("armv8-m.main", NULL, "cortex-m33", 0);

  /* target_dependent_config */
  UT_ASSERT_STREQ(arm_target_dependent.mcpu_name, "cortex-m33");
  UT_ASSERT_EQ(arm_target_dependent.is_secure_tz, false);

  /* armv8-m.main core profile (see THOP_PROFILE_ARMV8M_MAIN_CORE) */
  UT_ASSERT_EQ(arm_target_dependent.feat.t16, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.t32, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.it, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.dsp, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.div, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.fp_armv8, 1);
  /* no -mfpu given => no vfp/mve bits set by the profile itself */
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_sp, 0);
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_dp, 0);
  UT_ASSERT_EQ(arm_target_dependent.feat.fp_dp_d32, 0);
  UT_ASSERT_EQ(arm_target_dependent.feat.sec, 0);

  /* generic architecture_config: fixed ARMv8-M constants */
  UT_ASSERT_EQ(architecture_config.pointer_size, 4);
  UT_ASSERT_EQ(architecture_config.stack_align, 8);
  UT_ASSERT_EQ(architecture_config.reg_size, 4);
  UT_ASSERT_EQ(architecture_config.parameter_registers, 4);
  UT_ASSERT_EQ(architecture_config.static_chain_reg, 10);
  UT_ASSERT_EQ(architecture_config.int_reg_count, 13);
  UT_ASSERT_EQ(architecture_config.default_align, 4);
  UT_ASSERT_EQ(architecture_config.big_endian, 0);
  UT_ASSERT_STREQ(architecture_config.march_name, "armv8-m.main");

  /* mfpu == NULL => the has_fpu/fpu assignment block is skipped entirely;
   * has_fpu stays at its designated-initializer value of 0 and fpu at NULL. */
  UT_ASSERT_EQ(architecture_config.has_fpu, 0);
  UT_ASSERT(architecture_config.fpu == NULL);

  /* no vfp_sp/vfp_dp/fp_dp_d32 => fp_reg_count falls through the ternary to 0 */
  UT_ASSERT_EQ(architecture_config.fp_reg_count, 0);

  /* target_dependent must point back at the backend-private struct */
  UT_ASSERT(architecture_config.target_dependent == (struct target_dependent_config *)&arm_target_dependent);

  return 0;
}

UT_TEST(test_arm_target_init_null_march_defaults_to_armv8m_main)
{
  /* thop_feats_from_march(NULL) returns THOP_PROFILE_ARMV8M_MAIN_CORE, and
   * arm.c's own march ? march : "armv8-m.main" fills the display name -- so
   * a NULL march must reproduce test_arm_target_init_basic_fields_no_fpu's
   * feature set exactly, even though no string was passed in. */
  arm_target_init(NULL, NULL, "cortex-m33", 0);

  UT_ASSERT_STREQ(architecture_config.march_name, "armv8-m.main");
  UT_ASSERT_EQ(arm_target_dependent.feat.t16, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.t32, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.dsp, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.fp_armv8, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_sp, 0);

  return 0;
}

UT_TEST(test_arm_target_init_mcpu_passthrough_null)
{
  /* mcpu is stored verbatim with no validation/defaulting, unlike march. */
  arm_target_init("armv8-m.main", NULL, NULL, 0);
  UT_ASSERT(arm_target_dependent.mcpu_name == NULL);

  arm_target_init("armv8-m.main", NULL, "cortex-m23", 0);
  UT_ASSERT_STREQ(arm_target_dependent.mcpu_name, "cortex-m23");

  return 0;
}

UT_TEST(test_arm_target_init_mfpu_vfp_dp_sets_fp_reg_count_32)
{
  /* fpv5-d16 => vfp_sp=1, vfp_dp=1, fp_armv8=1, fp_dp_d32=0
   * => fp_reg_count ternary picks the vfp_dp branch: 32. */
  arm_target_init("armv8-m.main", "fpv5-d16", "cortex-m33", 0);

  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_sp, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_dp, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.fp_dp_d32, 0);
  UT_ASSERT_EQ(architecture_config.fp_reg_count, 32);

  /* arm_resolve_fpu() is still an unconditional NULL-returning stub (see the
   * TODO in arm.c), so has_fpu/fpu are NOT actually populated from a real
   * FPU config even though mfpu was given a valid, recognised name. This
   * documents current (stub) behaviour, not a design choice by this test. */
  UT_ASSERT_EQ(architecture_config.has_fpu, 0);
  UT_ASSERT(architecture_config.fpu == NULL);

  return 0;
}

UT_TEST(test_arm_target_init_mfpu_d32_sets_fp_reg_count_64)
{
  /* fpv5-d32 => vfp_sp=1, vfp_dp=1, fp_armv8=1, fp_dp_d32=1
   * => fp_reg_count ternary picks the fp_dp_d32 branch first: 64. */
  arm_target_init("armv8-m.main", "fpv5-d32", "cortex-m33", 0);

  UT_ASSERT_EQ(arm_target_dependent.feat.fp_dp_d32, 1);
  UT_ASSERT_EQ(architecture_config.fp_reg_count, 64);

  return 0;
}

UT_TEST(test_arm_target_init_mfpu_sp_only_sets_fp_reg_count_32)
{
  /* fpv5-sp-d16 => vfp_sp=1 only (no vfp_dp, no fp_dp_d32)
   * => fp_reg_count ternary falls to the vfp_sp branch: 32. */
  arm_target_init("armv8-m.main", "fpv5-sp-d16", "cortex-m33", 0);

  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_sp, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_dp, 0);
  UT_ASSERT_EQ(arm_target_dependent.feat.fp_dp_d32, 0);
  UT_ASSERT_EQ(architecture_config.fp_reg_count, 32);

  return 0;
}

UT_TEST(test_arm_target_init_mfpu_none_string_clears_fpu_bits)
{
  /* -mfpu=none is a recognised name (THOP_FPU_NONE, all-zero) rather than
   * NULL, so it *does* take the mfpu != NULL branch in arm_target_init
   * (has_fpu computed from arm_resolve_fpu(), still NULL/0 per the stub)
   * while contributing no feature bits at all. */
  arm_target_init("armv8-m.main", "none", "cortex-m33", 0);

  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_sp, 0);
  UT_ASSERT_EQ(arm_target_dependent.feat.vfp_dp, 0);
  UT_ASSERT_EQ(architecture_config.fp_reg_count, 0);
  UT_ASSERT_EQ(architecture_config.has_fpu, 0);
  UT_ASSERT(architecture_config.fpu == NULL);

  return 0;
}

UT_TEST(test_arm_target_init_march_ext_sec_sets_is_secure_tz)
{
  /* +sec march extension sets feat.sec, which arm_target_init folds into
   * is_secure_tz via "feat.sec != 0". */
  arm_target_init("armv8-m.main+sec", NULL, "cortex-m33", 0);

  UT_ASSERT_EQ(arm_target_dependent.feat.sec, 1);
  UT_ASSERT_EQ(arm_target_dependent.is_secure_tz, true);

  return 0;
}

UT_TEST(test_arm_target_init_march_base_profile_omits_main_only_bits)
{
  /* armv8-m.base has no t32/it/div/dsp -- distinguishes it from the .main
   * profile and exercises a different thop_feats_from_march() table row. */
  arm_target_init("armv8-m.base", NULL, "cortex-m23", 0);

  UT_ASSERT_EQ(arm_target_dependent.feat.t16, 1);
  UT_ASSERT_EQ(arm_target_dependent.feat.t32, 0);
  UT_ASSERT_EQ(arm_target_dependent.feat.it, 0);
  UT_ASSERT_EQ(arm_target_dependent.feat.div, 0);
  UT_ASSERT_EQ(arm_target_dependent.feat.dsp, 0);
  UT_ASSERT_EQ(arm_target_dependent.feat.ldaex, 1);
  UT_ASSERT_STREQ(architecture_config.march_name, "armv8-m.base");

  return 0;
}

UT_TEST(test_arm_target_init_extra_feat_bits_fold_in)
{
  /* extra_feat_bits is OR'd into the resolved profile inside
   * thumb_resolve_features() before arm.c ever sees the result -- confirm
   * a bit not implied by the armv8-m.base profile (mve_int) reaches
   * arm_target_dependent.feat when passed via extra_feat_bits. */
  thop_feat extra = {0};
  extra.mve_int = 1;
  uint64_t extra_bits = thop_feat_bits(extra);

  arm_target_init("armv8-m.base", NULL, "cortex-m23", extra_bits);

  UT_ASSERT_EQ(arm_target_dependent.feat.mve_int, 1);
  /* base profile bits are still present alongside the extra bit */
  UT_ASSERT_EQ(arm_target_dependent.feat.t16, 1);

  return 0;
}

UT_TEST(test_arm_target_init_reinit_overwrites_previous_state)
{
  /* arm_target_init has no "first call only" guard -- a second call fully
   * overwrites the globals rather than merging with the previous state. */
  arm_target_init("armv8-m.main+sec", NULL, "cortex-m33", 0);
  UT_ASSERT_EQ(arm_target_dependent.is_secure_tz, true);

  arm_target_init("armv8-m.base", NULL, "cortex-m23", 0);
  UT_ASSERT_EQ(arm_target_dependent.is_secure_tz, false);
  UT_ASSERT_EQ(arm_target_dependent.feat.sec, 0);
  UT_ASSERT_STREQ(arm_target_dependent.mcpu_name, "cortex-m23");

  return 0;
}

/* ------------------------------------------------------------ tcc_target_has */

/* Reset arm_target_dependent to an all-zero feature set, then set exactly
 * one bit. Used to prove tcc_target_has() dispatches to the *matching* bit
 * rather than e.g. always returning true/false or reading the wrong field. */
static void set_single_feat_bit(void (*setter)(thop_feat *f))
{
  arm_target_dependent = (struct target_dependent_config){
      .mcpu_name = "cortex-m33",
      .feat = (thop_feat){0},
      .is_secure_tz = false,
  };
  setter(&arm_target_dependent.feat);
}

static void set_div(thop_feat *f) { f->div = 1; }
static void set_vfp_sp(thop_feat *f) { f->vfp_sp = 1; }
static void set_vfp_dp(thop_feat *f) { f->vfp_dp = 1; }
static void set_fp16(thop_feat *f) { f->fp16 = 1; }
static void set_dsp(thop_feat *f) { f->dsp = 1; }
static void set_sat(thop_feat *f) { f->sat = 1; }
static void set_bfx(thop_feat *f) { f->bfx = 1; }
static void set_it(thop_feat *f) { f->it = 1; }
static void set_movw_movt(thop_feat *f) { f->movw_movt = 1; }
static void set_mve_int(thop_feat *f) { f->mve_int = 1; }
static void set_sec(thop_feat *f) { f->sec = 1; }
static void set_pacbti(thop_feat *f) { f->pacbti = 1; }
static void set_lob(thop_feat *f) { f->lob = 1; }

UT_TEST(test_tcc_target_has_all_caps_false_on_zero_feat)
{
  set_single_feat_bit(set_div); /* dummy setter, immediately overwritten */
  arm_target_dependent.feat = (thop_feat){0};

  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_DIVIDE), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_FP_SP), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_FP_DP), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_FP_HP), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_DSP_SIMD), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_SATURATING_ARITH), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_BITFIELD_INSTRS), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_COND_EXEC), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_MOVE_IMM_WIDE), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_VECTOR), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_SECURITY), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_POINTER_AUTH), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_LOW_OVERHEAD_LOOP), false);

  return 0;
}

UT_TEST(test_tcc_target_has_hw_divide_reads_div_bit_only)
{
  set_single_feat_bit(set_div);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_DIVIDE), true);
  /* a neighbouring cap must NOT alias onto the same bit */
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_FP_SP), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_SATURATING_ARITH), false);
  return 0;
}

UT_TEST(test_tcc_target_has_fp_sp_reads_vfp_sp_bit_only)
{
  set_single_feat_bit(set_vfp_sp);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_FP_SP), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_FP_DP), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_DIVIDE), false);
  return 0;
}

UT_TEST(test_tcc_target_has_fp_dp_reads_vfp_dp_bit_only)
{
  set_single_feat_bit(set_vfp_dp);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_FP_DP), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_FP_SP), false);
  return 0;
}

UT_TEST(test_tcc_target_has_fp_hp_reads_fp16_bit_only)
{
  set_single_feat_bit(set_fp16);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_FP_HP), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_FP_DP), false);
  return 0;
}

UT_TEST(test_tcc_target_has_dsp_simd_reads_dsp_bit_only)
{
  set_single_feat_bit(set_dsp);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_DSP_SIMD), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_VECTOR), false);
  return 0;
}

UT_TEST(test_tcc_target_has_saturating_arith_reads_sat_bit_only)
{
  set_single_feat_bit(set_sat);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_SATURATING_ARITH), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_DIVIDE), false);
  return 0;
}

UT_TEST(test_tcc_target_has_bitfield_instrs_reads_bfx_bit_only)
{
  set_single_feat_bit(set_bfx);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_BITFIELD_INSTRS), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_COND_EXEC), false);
  return 0;
}

UT_TEST(test_tcc_target_has_cond_exec_reads_it_bit_only)
{
  set_single_feat_bit(set_it);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_COND_EXEC), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_BITFIELD_INSTRS), false);
  return 0;
}

UT_TEST(test_tcc_target_has_move_imm_wide_reads_movw_movt_bit_only)
{
  set_single_feat_bit(set_movw_movt);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_MOVE_IMM_WIDE), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_COND_EXEC), false);
  return 0;
}

UT_TEST(test_tcc_target_has_vector_reads_mve_int_bit_only)
{
  set_single_feat_bit(set_mve_int);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_VECTOR), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_DSP_SIMD), false);
  return 0;
}

UT_TEST(test_tcc_target_has_security_reads_sec_bit_only)
{
  set_single_feat_bit(set_sec);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_SECURITY), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_POINTER_AUTH), false);
  return 0;
}

UT_TEST(test_tcc_target_has_pointer_auth_reads_pacbti_bit_only)
{
  set_single_feat_bit(set_pacbti);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_POINTER_AUTH), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_SECURITY), false);
  return 0;
}

UT_TEST(test_tcc_target_has_low_overhead_loop_reads_lob_bit_only)
{
  set_single_feat_bit(set_lob);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_LOW_OVERHEAD_LOOP), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_POINTER_AUTH), false);
  return 0;
}

UT_TEST(test_tcc_target_has_reflects_arm_target_init_end_to_end)
{
  /* Same dispatch, but driven through the real init path (armv7e-m has dsp
   * but no bfx/div distinction vs the base .main profile -- exercise the
   * public entry point end-to-end rather than only via direct struct
   * assignment as the other tcc_target_has tests do). */
  arm_target_init("armv7e-m", NULL, "cortex-m4", 0);

  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_DSP_SIMD), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_DIVIDE), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_BITFIELD_INSTRS), true);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_HW_FP_SP), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_SECURITY), false);
  UT_ASSERT_EQ(tcc_target_has(TCC_CAP_LOW_OVERHEAD_LOOP), false);

  return 0;
}

UT_TEST(test_tcc_target_has_unknown_cap_falls_through_to_false)
{
  /* The switch in tcc_target_has() has no `default:` case -- it exhaustively
   * lists every current tcc_target_cap enumerator, and the trailing
   * `return false;` after the switch exists purely as a defensive fallback
   * for a value outside the enum's defined range (e.g. an ABI mismatch or a
   * future enumerator the switch hasn't been updated for). Cast an
   * out-of-range int to reach that line deliberately. */
  set_single_feat_bit(set_div);
  tcc_target_cap bogus = (tcc_target_cap)9999;
  UT_ASSERT_EQ(tcc_target_has(bogus), false);
  return 0;
}

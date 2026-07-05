/*
 *  test_libtcc_options_target.c - suite for libtcc.c: tcc_set_options()
 *  target/debug/dependency flag parsing
 *
 *  Exercises the real tcc_parse_args()/tcc_set_options() dispatch in
 *  libtcc.c (mfpu=, mfloat-abi=, std=, -g, -o, -M/-MF/-MMD/-MD) against a
 *  freshly-created TCCState, asserting on the exact TCCState fields the
 *  real code writes (confirmed by reading libtcc.c directly, not guessed).
 */

#include "tcc.h"

#include "ut.h"

/* ------------------------------------------------------------------ helpers */

static TCCState *setup_state(void)
{
  TCCState *s = tcc_new();
  return s;
}

/* ------------------------------------------------------------------ mfpu */

UT_TEST(test_set_options_mfpu_vfpv4)
{
  TCCState *s = setup_state();
  UT_ASSERT_EQ(tcc_set_options(s, "-mfpu=vfpv4"), 0);
  UT_ASSERT_EQ(s->fpu_type, ARM_FPU_VFPV4);
  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_options_mfpu_fpv5_sp_d16)
{
  TCCState *s = setup_state();
  UT_ASSERT_EQ(tcc_set_options(s, "-mfpu=fpv5-sp-d16"), 0);
  UT_ASSERT_EQ(s->fpu_type, ARM_FPU_FPV5_SP_D16);
  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_options_mfpu_none)
{
  TCCState *s = setup_state();
  UT_ASSERT_EQ(tcc_set_options(s, "-mfpu=none"), 0);
  UT_ASSERT_EQ(s->fpu_type, ARM_FPU_NONE);
  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_options_mfpu_neon_fp_armv8)
{
  TCCState *s = setup_state();
  UT_ASSERT_EQ(tcc_set_options(s, "-mfpu=neon-fp-armv8"), 0);
  UT_ASSERT_EQ(s->fpu_type, ARM_FPU_NEON_FP_ARMV8);
  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_options_mfpu_unsupported_errors)
{
  TCCState *s = setup_state();
  /* "soft" is not a recognized -mfpu= value in libtcc.c's mfpu table (only
   * mfloat-abi recognizes "soft") -- confirmed by reading the TCC_OPTION_mfpu
   * case in libtcc.c, which has no "soft" branch and falls into the
   * tcc_error_noabort("unsupported FPU type ...") else-arm, returning < 0. */
  int ret = tcc_set_options(s, "-mfpu=soft");
  UT_ASSERT(ret < 0);
  tcc_delete(s);
  return 0;
}

/* ------------------------------------------------------------------ mfloat-abi */

UT_TEST(test_set_options_mfloat_abi_soft)
{
  TCCState *s = setup_state();
  UT_ASSERT_EQ(tcc_set_options(s, "-mfloat-abi=soft"), 0);
  UT_ASSERT_EQ(s->float_abi, ARM_SOFT_FLOAT);
  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_options_mfloat_abi_softfp)
{
  TCCState *s = setup_state();
  UT_ASSERT_EQ(tcc_set_options(s, "-mfloat-abi=softfp"), 0);
  UT_ASSERT_EQ(s->float_abi, ARM_SOFTFP_FLOAT);
  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_options_mfloat_abi_hard)
{
  TCCState *s = setup_state();
  UT_ASSERT_EQ(tcc_set_options(s, "-mfloat-abi=hard"), 0);
  UT_ASSERT_EQ(s->float_abi, ARM_HARD_FLOAT);
  tcc_delete(s);
  return 0;
}

/* ------------------------------------------------------------------ std= */

UT_TEST(test_set_options_std_c11)
{
  TCCState *s = setup_state();
  /* tcc_new() already defaults cversion to 201112; flip to c17 first so this
   * assertion actually exercises the TCC_OPTION_std case rather than
   * trivially matching the untouched default. */
  UT_ASSERT_EQ(tcc_set_options(s, "-std=c17"), 0);
  UT_ASSERT_EQ(s->cversion, 201710);
  UT_ASSERT_EQ(tcc_set_options(s, "-std=c11"), 0);
  UT_ASSERT_EQ(s->cversion, 201112);
  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_options_std_gnu17)
{
  TCCState *s = setup_state();
  UT_ASSERT_EQ(tcc_set_options(s, "-std=gnu17"), 0);
  UT_ASSERT_EQ(s->cversion, 201710);
  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_options_std_c23)
{
  TCCState *s = setup_state();
  UT_ASSERT_EQ(tcc_set_options(s, "-std=c2x"), 0);
  UT_ASSERT_EQ(s->cversion, 202311);
  tcc_delete(s);
  return 0;
}

/* ------------------------------------------------------------------ -g */

UT_TEST(test_set_options_g_sets_debug_and_dwarf)
{
  TCCState *s = setup_state();
  UT_ASSERT_EQ(s->do_debug, 0); /* tcc_new() default: no debug info */
  UT_ASSERT_EQ(tcc_set_options(s, "-g"), 0);
  UT_ASSERT_EQ(s->do_debug, 2);
  UT_ASSERT_EQ(s->dwarf, CONFIG_DWARF_VERSION);
  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_options_g_digit_sets_debug_level)
{
  TCCState *s = setup_state();
  /* "-g1" -> isnum('1') branch: x = '1'-'0' = 1; do_backtrace is 0 by
   * default so the "x == 0 && do_backtrace" special case does not apply;
   * do_debug is set directly to x (1). */
  UT_ASSERT_EQ(tcc_set_options(s, "-g1"), 0);
  UT_ASSERT_EQ(s->do_debug, 1);
  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_options_g3_clamps_to_2)
{
  TCCState *s = setup_state();
  /* "-g3" -> x = 3, and the ternary clamps any x > 2 down to 2. */
  UT_ASSERT_EQ(tcc_set_options(s, "-g3"), 0);
  UT_ASSERT_EQ(s->do_debug, 2);
  tcc_delete(s);
  return 0;
}

/* ------------------------------------------------------------------ -o */

UT_TEST(test_set_options_o_sets_outfile)
{
  TCCState *s = setup_state();
  UT_ASSERT(s->outfile == NULL);
  UT_ASSERT_EQ(tcc_set_options(s, "-o out1.elf"), 0);
  UT_ASSERT(s->outfile != NULL);
  UT_ASSERT_STREQ(s->outfile, "out1.elf");
  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_options_o_reassignment_overwrites_not_leaks)
{
  TCCState *s = setup_state();
  UT_ASSERT_EQ(tcc_set_options(s, "-o first.elf"), 0);
  UT_ASSERT_STREQ(s->outfile, "first.elf");
  /* TCC_OPTION_o: a pre-existing s->outfile triggers tcc_warning("multiple
   * -o option") and tcc_free()s the old string before strdup'ing the new
   * one -- it does not refuse the reassignment, so the second -o simply
   * overwrites (this also proves the old string was freed, not leaked,
   * under the project's default ASan build). */
  UT_ASSERT_EQ(tcc_set_options(s, "-o second.elf"), 0);
  UT_ASSERT_STREQ(s->outfile, "second.elf");
  tcc_delete(s);
  return 0;
}

/* ------------------------------------------------------------------ dependency flags */

UT_TEST(test_set_options_M_sets_deps_fields)
{
  TCCState *s = setup_state();
  /* TCC_OPTION_M falls through TCC_OPTION_MM into TCC_OPTION_MMD:
   * include_sys_deps=1, just_deps=1, deps_outfile defaults to "-" (since
   * unset), gen_deps=1. */
  UT_ASSERT_EQ(tcc_set_options(s, "-M"), 0);
  UT_ASSERT_EQ(s->include_sys_deps, 1);
  UT_ASSERT_EQ(s->just_deps, 1);
  UT_ASSERT_EQ(s->gen_deps, 1);
  UT_ASSERT(s->deps_outfile != NULL);
  UT_ASSERT_STREQ(s->deps_outfile, "-");
  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_options_MF_sets_deps_outfile)
{
  TCCState *s = setup_state();
  UT_ASSERT_EQ(tcc_set_options(s, "-MF deps.d"), 0);
  UT_ASSERT(s->deps_outfile != NULL);
  UT_ASSERT_STREQ(s->deps_outfile, "deps.d");
  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_options_MMD_sets_gen_deps_only)
{
  TCCState *s = setup_state();
  /* TCC_OPTION_MMD sets gen_deps=1 directly, without falling through the M
   * / MM cases above it, so include_sys_deps and just_deps stay 0. */
  UT_ASSERT_EQ(tcc_set_options(s, "-MMD"), 0);
  UT_ASSERT_EQ(s->gen_deps, 1);
  UT_ASSERT_EQ(s->include_sys_deps, 0);
  UT_ASSERT_EQ(s->just_deps, 0);
  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_options_MD_sets_gen_deps_and_include_sys_deps)
{
  TCCState *s = setup_state();
  /* TCC_OPTION_MD sets both gen_deps=1 and include_sys_deps=1, but not
   * just_deps (that's only M/MM). */
  UT_ASSERT_EQ(tcc_set_options(s, "-MD"), 0);
  UT_ASSERT_EQ(s->gen_deps, 1);
  UT_ASSERT_EQ(s->include_sys_deps, 1);
  UT_ASSERT_EQ(s->just_deps, 0);
  tcc_delete(s);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(libtcc_options_target)
{
  UT_RUN(test_set_options_mfpu_vfpv4);
  UT_RUN(test_set_options_mfpu_fpv5_sp_d16);
  UT_RUN(test_set_options_mfpu_none);
  UT_RUN(test_set_options_mfpu_neon_fp_armv8);
  UT_RUN(test_set_options_mfpu_unsupported_errors);

  UT_RUN(test_set_options_mfloat_abi_soft);
  UT_RUN(test_set_options_mfloat_abi_softfp);
  UT_RUN(test_set_options_mfloat_abi_hard);

  UT_RUN(test_set_options_std_c11);
  UT_RUN(test_set_options_std_gnu17);
  UT_RUN(test_set_options_std_c23);

  UT_RUN(test_set_options_g_sets_debug_and_dwarf);
  UT_RUN(test_set_options_g_digit_sets_debug_level);
  UT_RUN(test_set_options_g3_clamps_to_2);

  UT_RUN(test_set_options_o_sets_outfile);
  UT_RUN(test_set_options_o_reassignment_overwrites_not_leaks);

  UT_RUN(test_set_options_M_sets_deps_fields);
  UT_RUN(test_set_options_MF_sets_deps_outfile);
  UT_RUN(test_set_options_MMD_sets_gen_deps_only);
  UT_RUN(test_set_options_MD_sets_gen_deps_and_include_sys_deps);
}

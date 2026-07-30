/*
 *  test_libtcc_options_opt.c - suite for libtcc.c: tcc_set_options()
 *
 *  Covers the optimization-level (-O0/-O1/-O2) and warning (-Wall/-w/
 *  unrecognized flag) branches of tcc_parse_args()'s TCC_OPTION_O /
 *  TCC_OPTION_W / TCC_OPTION_w / "invalid option" paths, using the real
 *  linked libtcc.c (see build_libtcc_api/, same binary as
 *  test_libtcc_lifecycle.c). Field names and dispatch behavior were
 *  confirmed by reading tcc_parse_args()/options_W[]/options_f[]/set_flag()
 *  in libtcc.c and the TCCState layout in tcc.h -- not guessed.
 */

#include "tcc.h"
#include "libtcc.h"

#include <string.h>

#include "ut.h"

/* ------------------------------------------------------------------ error capture helpers */

#define UT_ERRBUF_SIZE 512

static char ut_opt_error_buf[UT_ERRBUF_SIZE];
static int ut_opt_error_calls;

static void ut_opt_capture_error(void *opaque, const char *msg)
{
  (void)opaque;
  ut_opt_error_calls++;
  strncpy(ut_opt_error_buf, msg, UT_ERRBUF_SIZE - 1);
  ut_opt_error_buf[UT_ERRBUF_SIZE - 1] = '\0';
}

static void ut_opt_reset_capture(TCCState *s)
{
  ut_opt_error_buf[0] = '\0';
  ut_opt_error_calls = 0;
  tcc_set_error_func(s, NULL, ut_opt_capture_error);
}

/* ------------------------------------------------------------------ -O tests */

UT_TEST(test_tcc_set_options_no_o_flag_leaves_opt_fields_at_default)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  /* Never touched -O at all: everything must be at tcc_new()'s zeroed
   * default (tcc_mallocz). */
  UT_ASSERT_EQ(s->optimize, 0);
  UT_ASSERT_EQ(s->opt_dce, 0);
  UT_ASSERT_EQ(s->opt_const_prop, 0);
  UT_ASSERT_EQ(s->opt_licm, 0);
  UT_ASSERT_EQ(s->opt_inline_small, 0);
  UT_ASSERT_EQ(s->opt_inline_functions, 0);
  UT_ASSERT_EQ(s->opt_inline_limit, 0);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_tcc_set_options_o0_leaves_opt_fields_at_default)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  int ret = tcc_set_options(s, "-O0");
  UT_ASSERT_EQ(ret, 0);

  UT_ASSERT_EQ(s->optimize, 0);
  UT_ASSERT_EQ(s->opt_dce, 0);
  UT_ASSERT_EQ(s->opt_const_prop, 0);
  UT_ASSERT_EQ(s->opt_cse, 0);
  UT_ASSERT_EQ(s->opt_licm, 0);
  UT_ASSERT_EQ(s->opt_iv_strength_red, 0);
  UT_ASSERT_EQ(s->opt_inline_small, 0);
  UT_ASSERT_EQ(s->opt_inline_functions, 0);
  UT_ASSERT_EQ(s->opt_inline_limit, 0);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_tcc_set_options_o1_enables_pass_batch)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  int ret = tcc_set_options(s, "-O1");
  UT_ASSERT_EQ(ret, 0);

  UT_ASSERT_EQ(s->optimize, 1);

  /* The batch of passes TCC_OPTION_O turns on when optimize >= 1
   * (verified against the literal case TCC_OPTION_O body in libtcc.c). */
  UT_ASSERT_EQ(s->opt_dce, 1);
  UT_ASSERT_EQ(s->opt_const_prop, 1);
  UT_ASSERT_EQ(s->opt_copy_prop, 1);
  UT_ASSERT_EQ(s->opt_cse, 1);
  UT_ASSERT_EQ(s->opt_bool_idempotent, 1);
  UT_ASSERT_EQ(s->opt_bool_simplify, 1);
  UT_ASSERT_EQ(s->opt_store_load_fwd, 1);
  UT_ASSERT_EQ(s->opt_redundant_store, 1);
  UT_ASSERT_EQ(s->opt_dead_store, 1);
  UT_ASSERT_EQ(s->opt_indexed_memory, 1);
  UT_ASSERT_EQ(s->opt_disp_fusion, 1);
  UT_ASSERT_EQ(s->opt_lea_fold, 1);
  UT_ASSERT_EQ(s->opt_stack_addr_cse, 1);
  UT_ASSERT_EQ(s->opt_strength_red, 1);
  UT_ASSERT_EQ(s->opt_vrp, 1);
  UT_ASSERT_EQ(s->opt_float_narrow, 1);
  UT_ASSERT_EQ(s->opt_jump_threading, 1);
  UT_ASSERT_EQ(s->opt_inline_small, 1);

  /* -O2-only knobs must NOT be enabled yet. */
  UT_ASSERT_EQ(s->opt_mla_fusion, 0);
  UT_ASSERT_EQ(s->opt_licm, 0);
  UT_ASSERT_EQ(s->opt_ipc, 0);
  UT_ASSERT_EQ(s->opt_iv_strength_red, 0);
  UT_ASSERT_EQ(s->opt_loop_unroll, 0);
  UT_ASSERT_EQ(s->opt_reroll, 0);
  UT_ASSERT_EQ(s->opt_inline_functions, 0);

  /* opt_inline_limit was 0 (unset) so -O1 raises it to the level-1
   * default of 30. */
  UT_ASSERT_EQ(s->opt_inline_limit, 30);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_tcc_set_options_o2_additionally_enables_inline_functions)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  int ret = tcc_set_options(s, "-O2");
  UT_ASSERT_EQ(ret, 0);

  UT_ASSERT_EQ(s->optimize, 2);

  /* Still gets the full -O1 batch (optimize >= 1 block also runs). */
  UT_ASSERT_EQ(s->opt_dce, 1);
  UT_ASSERT_EQ(s->opt_inline_small, 1);

  /* -O2-only heavy tier: loops, MLA fusion, IPC, and auto-inline of larger
   * functions with the threshold raised from the -O1 default of 30 to 100
   * (opt_inline_limit < 100 check). */
  UT_ASSERT_EQ(s->opt_mla_fusion, 1);
  UT_ASSERT_EQ(s->opt_licm, 1);
  UT_ASSERT_EQ(s->opt_ipc, 1);
  UT_ASSERT_EQ(s->opt_iv_strength_red, 1);
  UT_ASSERT_EQ(s->opt_loop_unroll, 1);
  UT_ASSERT_EQ(s->opt_reroll, 1);
  UT_ASSERT_EQ(s->opt_inline_functions, 1);
  UT_ASSERT_EQ(s->opt_inline_limit, 100);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_tcc_set_options_o2_does_not_lower_explicit_inline_limit)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  /* -finline-limit=N sets opt_inline_limit directly (TCC_OPTION_f,
   * "inline-limit=" prefix handled before set_flag()). Setting it above
   * 100 first and then applying -O2 must NOT clobber it downward, since
   * TCC_OPTION_O only raises the limit when it is below the level
   * default (s->opt_inline_limit < 100). */
  int ret = tcc_set_options(s, "-finline-limit=200 -O2");
  UT_ASSERT_EQ(ret, 0);

  UT_ASSERT_EQ(s->opt_inline_functions, 1);
  UT_ASSERT_EQ(s->opt_inline_limit, 200);

  tcc_delete(s);
  return 0;
}

/* ------------------------------------------------------------------ -W tests */

UT_TEST(test_tcc_set_options_wall_sets_warn_batch)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  /* warn_all is 0 by default (only -Wall turns it on); the other two
   * WD_ALL members are already 1 straight out of tcc_new() (see
   * test_libtcc_lifecycle.c), so warn_all is the only field whose
   * before/after actually distinguishes "-Wall ran" from "did nothing". */
  UT_ASSERT_EQ(s->warn_all, 0);

  int ret = tcc_set_options(s, "-Wall");
  UT_ASSERT_EQ(ret, 0);

  UT_ASSERT_EQ(s->warn_all, 1);
  UT_ASSERT_EQ(s->warn_implicit_function_declaration, 1);
  UT_ASSERT_EQ(s->warn_discarded_qualifiers, 1);

  /* Non-WD_ALL members of options_W[] must be untouched by -Wall. */
  UT_ASSERT_EQ(s->warn_error, 0);
  UT_ASSERT_EQ(s->warn_write_strings, 0);
  UT_ASSERT_EQ(s->warn_unsupported, 0);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_tcc_set_options_single_w_flag_sets_only_that_flag)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  int ret = tcc_set_options(s, "-Wwrite-strings");
  UT_ASSERT_EQ(ret, 0);

  UT_ASSERT_EQ(s->warn_write_strings, 1);
  UT_ASSERT_EQ(s->warn_all, 0);
  UT_ASSERT_EQ(s->warn_none, 0);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_tcc_set_options_w_sets_warn_none)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  UT_ASSERT_EQ(s->warn_none, 0);

  int ret = tcc_set_options(s, "-w");
  UT_ASSERT_EQ(ret, 0);

  UT_ASSERT_EQ(s->warn_none, 1);

  tcc_delete(s);
  return 0;
}

/* A plain tcc_warning() call (not the tcc_warning_c(<option>) form used
 * for e.g. "unsupported option") only gates on warn_error/warn_none, so
 * it is the path that actually demonstrates -w suppression end-to-end.
 * "-o a -o b" triggers exactly one such call: "multiple -o option". */
UT_TEST(test_tcc_set_options_w_suppresses_plain_warning_via_error_func)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);
  ut_opt_reset_capture(s);

  int ret = tcc_set_options(s, "-w -o a.out -o b.out");
  UT_ASSERT_EQ(ret, 0);

  UT_ASSERT_EQ(s->warn_none, 1);
  UT_ASSERT_EQ(ut_opt_error_calls, 0);
  UT_ASSERT_EQ(ut_opt_error_buf[0], '\0');

  tcc_delete(s);
  return 0;
}

UT_TEST(test_tcc_set_options_without_w_plain_warning_reaches_error_func)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);
  ut_opt_reset_capture(s);

  /* Same repro as the suppression test above, minus "-w". */
  int ret = tcc_set_options(s, "-o a.out -o b.out");
  UT_ASSERT_EQ(ret, 0);

  UT_ASSERT_EQ(s->warn_none, 0);
  UT_ASSERT(ut_opt_error_calls >= 1);
  UT_ASSERT(strstr(ut_opt_error_buf, "multiple -o option") != NULL);

  tcc_delete(s);
  return 0;
}

/* ------------------------------------------------------------------ unrecognized option */

/* "-zbogus" doesn't share a prefix with any entry in tcc_options[] (no
 * registered option starts with 'z'), so it falls through the option
 * table lookup in tcc_parse_args() to `return tcc_error_noabort("invalid
 * option -- '%s'", r)` -- a genuine hard error, unlike an unrecognized
 * -W<name> or -f<name> flag (those go through set_flag() failure into
 * the "unsupported_option" label, which only warns and does not fail). */
UT_TEST(test_tcc_set_options_unrecognized_flag_returns_minus1_no_abort)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);
  ut_opt_reset_capture(s);

  int ret = tcc_set_options(s, "-zbogus");

  /* Reaching this line at all proves tcc_set_options() did not call
   * exit()/abort() for an unrecognized option. */
  UT_ASSERT_EQ(ret, -1);
  UT_ASSERT_EQ(ut_opt_error_calls, 1);
  UT_ASSERT(strstr(ut_opt_error_buf, "invalid option") != NULL);
  UT_ASSERT(strstr(ut_opt_error_buf, "-zbogus") != NULL);

  tcc_delete(s);
  return 0;
}

/* By contrast, an unrecognized *-W* sub-flag is merely an "unsupported
 * option" warning (routed through set_flag() failure), and does not fail
 * tcc_set_options() at all -- confirm this distinction is real rather
 * than assumed. */
UT_TEST(test_tcc_set_options_unrecognized_w_subflag_is_not_an_error)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);
  ut_opt_reset_capture(s);

  int ret = tcc_set_options(s, "-Wthis-warning-name-does-not-exist");

  UT_ASSERT_EQ(ret, 0);

  tcc_delete(s);
  return 0;
}

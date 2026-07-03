/*
 *  test_libtcc_options_linker.c - suite for libtcc.c: tcc_set_options(s, ...)
 *  linker suboption parsing (the comma-joined "-Wl,<sub1>,<sub2>,..." form
 *  dispatched by the internal tcc_set_linker() in libtcc.c).
 *
 *  Modeled on test_libtcc_lifecycle.c: fresh tcc_new()/tcc_delete() per test,
 *  real libtcc.c linked into the libtcc-api/ binary.
 */

#include "tcc.h"

#include "ut.h"

/* ------------------------------------------------------------- error capture */

#define LINKER_TEST_ERRBUF_SIZE 256

static char linker_test_errbuf[LINKER_TEST_ERRBUF_SIZE];
static int linker_test_error_count;

static void linker_test_error_func(void *opaque, const char *msg)
{
  (void)opaque;
  linker_test_error_count++;
  if (msg)
  {
    strncpy(linker_test_errbuf, msg, LINKER_TEST_ERRBUF_SIZE - 1);
    linker_test_errbuf[LINKER_TEST_ERRBUF_SIZE - 1] = '\0';
  }
}

static void linker_test_reset_capture(TCCState *s)
{
  linker_test_errbuf[0] = '\0';
  linker_test_error_count = 0;
  tcc_set_error_func(s, NULL, linker_test_error_func);
}

/* --------------------------------------------------------------------- tests */

UT_TEST(test_wl_bsymbolic_sets_symbolic_flag)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  UT_ASSERT_EQ(s->symbolic, 0);

  int ret = tcc_set_options(s, "-Wl,-Bsymbolic");
  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(s->symbolic, 1);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_wl_rpath_sets_rpath_field)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  UT_ASSERT(s->rpath == NULL);

  int ret = tcc_set_options(s, "-Wl,-rpath=/opt/mylibs");
  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT(s->rpath != NULL);
  UT_ASSERT_STREQ(s->rpath, "/opt/mylibs");

  tcc_delete(s);
  return 0;
}

UT_TEST(test_wl_soname_sets_soname_field)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  UT_ASSERT(s->soname == NULL);

  int ret = tcc_set_options(s, "-Wl,-soname=libfoo.so.1");
  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT(s->soname != NULL);
  UT_ASSERT_STREQ(s->soname, "libfoo.so.1");

  tcc_delete(s);
  return 0;
}

UT_TEST(test_wl_gc_sections_sets_flag)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  UT_ASSERT_EQ(s->gc_sections, 0);

  int ret = tcc_set_options(s, "-Wl,--gc-sections");
  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(s->gc_sections, 1);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_wl_combined_suboptions_all_land)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  UT_ASSERT_EQ(s->symbolic, 0);
  UT_ASSERT(s->rpath == NULL);
  UT_ASSERT(s->soname == NULL);

  /* NOTE: order matters here -- see docs/bugs.md ("boolean linker suboption
   * must be last in a comma chain"). A value-taking suboption ("name=...")
   * consumes only up to the next comma and correctly advances the parser to
   * the next suboption, but a bare boolean flag (no '=') only matches when
   * it is the *entire* remaining string, so it must be placed last. */
  int ret = tcc_set_options(s, "-Wl,-rpath=/opt/mylibs,-soname=libfoo.so.1,-Bsymbolic");
  UT_ASSERT_EQ(ret, 0);

  UT_ASSERT_EQ(s->symbolic, 1);
  UT_ASSERT(s->rpath != NULL);
  UT_ASSERT_STREQ(s->rpath, "/opt/mylibs");
  UT_ASSERT(s->soname != NULL);
  UT_ASSERT_STREQ(s->soname, "libfoo.so.1");

  tcc_delete(s);
  return 0;
}

UT_TEST(test_wl_unrecognized_suboption_returns_error)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);
  linker_test_reset_capture(s);

  int ret = tcc_set_options(s, "-Wl,-this-suboption-does-not-exist");
  UT_ASSERT_EQ(ret, -1);
  UT_ASSERT_EQ(linker_test_error_count, 1);
  UT_ASSERT(strstr(linker_test_errbuf, "unsupported linker option") != NULL);
  UT_ASSERT(strstr(linker_test_errbuf, "this-suboption-does-not-exist") != NULL);

  tcc_delete(s);
  return 0;
}

/* Regression pin for a real parser defect (see bugs_found in the harness
 * report / docs/bugs.md): a bare boolean-flag suboption (no '=', e.g.
 * "-Bsymbolic") only matches tcc_set_linker's link_option() when it is the
 * *entire* remaining string, so placing it before a value-taking suboption
 * in the same comma chain makes the whole "-Wl,..." argument fail --
 * even though the flag alone, or the same flag placed last, works fine
 * (see test_wl_bsymbolic_sets_symbolic_flag and
 * test_wl_combined_suboptions_all_land above). This test documents the
 * CURRENT (buggy) behavior so a fix will be noticed here. */
UT_TEST(test_wl_boolean_flag_before_value_suboption_currently_fails)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);
  linker_test_reset_capture(s);

  int ret = tcc_set_options(s, "-Wl,-Bsymbolic,-rpath=/opt/mylibs,-soname=libfoo.so.1");
  UT_ASSERT_EQ(ret, -1);
  /* Nothing lands -- the whole comma chain is rejected as one unit. */
  UT_ASSERT_EQ(s->symbolic, 0);
  UT_ASSERT(s->rpath == NULL);
  UT_ASSERT(s->soname == NULL);
  UT_ASSERT_EQ(linker_test_error_count, 1);
  UT_ASSERT(strstr(linker_test_errbuf, "unsupported linker option") != NULL);

  tcc_delete(s);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(libtcc_options_linker)
{
  UT_RUN(test_wl_bsymbolic_sets_symbolic_flag);
  UT_RUN(test_wl_rpath_sets_rpath_field);
  UT_RUN(test_wl_soname_sets_soname_field);
  UT_RUN(test_wl_gc_sections_sets_flag);
  UT_RUN(test_wl_combined_suboptions_all_land);
  UT_RUN(test_wl_boolean_flag_before_value_suboption_currently_fails);
  UT_RUN(test_wl_unrecognized_suboption_returns_error);
}

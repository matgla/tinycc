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

  /* Suboption order is now irrelevant: both value-taking ("name=...") and
   * bare boolean ("-Bsymbolic") suboptions advance the parser to the next
   * comma-separated suboption.  See test_wl_boolean_flag_before_value_suboption
   * for the boolean-flag-first ordering. */
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

/* A bare boolean-flag suboption (no '=', e.g. "-Bsymbolic") followed by more
 * suboptions in the same "-Wl," comma chain is now accepted: link_option()
 * tolerates a trailing ',' after the flag and leaves the parser positioned to
 * advance to the next suboption. Every suboption lands regardless of order. */
UT_TEST(test_wl_boolean_flag_before_value_suboption)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);
  linker_test_reset_capture(s);

  int ret = tcc_set_options(s, "-Wl,-Bsymbolic,-rpath=/opt/mylibs,-soname=libfoo.so.1");
  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(s->symbolic, 1);
  UT_ASSERT(s->rpath != NULL);
  UT_ASSERT_STREQ(s->rpath, "/opt/mylibs");
  UT_ASSERT(s->soname != NULL);
  UT_ASSERT_STREQ(s->soname, "libfoo.so.1");
  UT_ASSERT_EQ(linker_test_error_count, 0);

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
  UT_RUN(test_wl_boolean_flag_before_value_suboption);
  UT_RUN(test_wl_unrecognized_suboption_returns_error);
}

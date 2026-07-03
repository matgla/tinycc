/*
 *  test_tcc.c - white-box unit tests for isolated helpers in tcc.c
 *  (build_tcc/run_unit_tests_tcc)
 *
 *  Tests the helpers that can be exercised without invoking the full driver
 *  main() or the compiler frontend.  The source file is pulled in directly so
 *  that static/ST_FUNC helpers are visible.
 */

#define USING_GLOBALS
#include "tcc.h"
#include "ut.h"

/* Rename main() so the test harness keeps its own entry point. */
#define main tcc_ut_main
#include "tcc.c"
#undef main

/* ========================================================================
 * tcc_is_64bit_operand
 * ======================================================================== */

UT_TEST(test_is_64bit_operand_null_is_false)
{
  UT_ASSERT_EQ(tcc_is_64bit_operand(NULL), 0);
  return 0;
}

UT_TEST(test_is_64bit_operand_int_is_false)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.type.t = VT_INT;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 0);
  return 0;
}

UT_TEST(test_is_64bit_operand_llong_is_true)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.type.t = VT_LLONG;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 1);
  return 0;
}

UT_TEST(test_is_64bit_operand_double_is_true)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.type.t = VT_DOUBLE;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 1);
  return 0;
}

UT_TEST(test_is_64bit_operand_long_double_is_true)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  sv.type.t = VT_LDOUBLE;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 1);
  return 0;
}

UT_TEST(test_is_64bit_operand_ignores_non_btype_bits)
{
  SValue sv;
  memset(&sv, 0, sizeof(sv));
  /* VT_UNSIGNED is not a basic type; only the BTYPE matters. */
  sv.type.t = VT_INT | VT_UNSIGNED;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 0);

  sv.type.t = VT_LLONG | VT_UNSIGNED;
  UT_ASSERT_EQ(tcc_is_64bit_operand(&sv), 1);
  return 0;
}

/* ========================================================================
 * default_outputfile
 * ======================================================================== */

UT_TEST(test_default_outputfile_falls_back_to_a_out)
{
  TCCState s;
  memset(&s, 0, sizeof(s));
  s.output_type = TCC_OUTPUT_EXE;

  char *out = default_outputfile(&s, NULL);
  UT_ASSERT_STREQ(out, "a.out");
  tcc_free(out);
  return 0;
}

UT_TEST(test_default_outputfile_uses_basename_for_obj)
{
  TCCState s;
  memset(&s, 0, sizeof(s));
  s.output_type = TCC_OUTPUT_OBJ;

  char *out = default_outputfile(&s, "/path/to/source.c");
  UT_ASSERT_STREQ(out, "source.o");
  tcc_free(out);
  return 0;
}

UT_TEST(test_default_outputfile_preserves_leading_underscore_for_obj)
{
  TCCState s;
  memset(&s, 0, sizeof(s));
  s.output_type = TCC_OUTPUT_OBJ;

  char *out = default_outputfile(&s, "_secret.c");
  UT_ASSERT_STREQ(out, "_secret.o");
  tcc_free(out);
  return 0;
}

UT_TEST(test_default_outputfile_exe_overwrites_extension_with_a_out)
{
  TCCState s;
  memset(&s, 0, sizeof(s));
  s.output_type = TCC_OUTPUT_EXE;

  char *out = default_outputfile(&s, "program.c");
  UT_ASSERT_STREQ(out, "a.out");
  tcc_free(out);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(tcc)
{
  UT_RUN(test_is_64bit_operand_null_is_false);
  UT_RUN(test_is_64bit_operand_int_is_false);
  UT_RUN(test_is_64bit_operand_llong_is_true);
  UT_RUN(test_is_64bit_operand_double_is_true);
  UT_RUN(test_is_64bit_operand_long_double_is_true);
  UT_RUN(test_is_64bit_operand_ignores_non_btype_bits);

  UT_RUN(test_default_outputfile_falls_back_to_a_out);
  UT_RUN(test_default_outputfile_uses_basename_for_obj);
  UT_RUN(test_default_outputfile_preserves_leading_underscore_for_obj);
  UT_RUN(test_default_outputfile_exe_overwrites_extension_with_a_out);
}

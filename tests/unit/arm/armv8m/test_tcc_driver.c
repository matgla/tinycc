/*
 *  test_tcc_driver.c - unit tests for tcc.c driver helpers
 */

#define _POSIX_C_SOURCE 200809L

#include "ut.h"

char *tcc_driver_basename_for_unit_tests(const char *name)
{
  char *p = strrchr(name, '/');
  return p ? p + 1 : (char *)name;
}

char *tcc_driver_fileextension_for_unit_tests(const char *name)
{
  char *b = tcc_driver_basename_for_unit_tests(name);
  char *e = strrchr(b, '.');
  return e ? e : b + strlen(b);
}

#define main tcc_driver_main_for_unit_tests
#define tcc_is_64bit_operand tcc_driver_is_64bit_operand_for_unit_tests
#define tcc_basename tcc_driver_basename_for_unit_tests
#define tcc_fileextension tcc_driver_fileextension_for_unit_tests
#define read16le tcc_driver_read16le_for_unit_tests
#define write16le tcc_driver_write16le_for_unit_tests
#define read32le tcc_driver_read32le_for_unit_tests
#define write32le tcc_driver_write32le_for_unit_tests
#define add32le tcc_driver_add32le_for_unit_tests
#define read64le tcc_driver_read64le_for_unit_tests
#define write64le tcc_driver_write64le_for_unit_tests
#include "tcc.c"
#undef write64le
#undef read64le
#undef add32le
#undef write32le
#undef read32le
#undef write16le
#undef read16le
#undef tcc_fileextension
#undef tcc_basename
#undef tcc_is_64bit_operand
#undef main

static void free_output(char *p)
{
  tcc_free(p);
}

UT_TEST(default_outputfile_uses_a_out_for_executable)
{
  TCCState s = {0};
  char *out;

  s.output_type = TCC_OUTPUT_EXE;
  out = default_outputfile(&s, "foo.c");
  UT_ASSERT_STREQ(out, "a.out");
  free_output(out);
  return 0;
}

UT_TEST(default_outputfile_uses_a_out_for_stdin_object_input)
{
  TCCState s = {0};
  char *out;

  s.output_type = TCC_OUTPUT_OBJ;
  out = default_outputfile(&s, "-");
  UT_ASSERT_STREQ(out, "a.out");
  free_output(out);
  return 0;
}

UT_TEST(default_outputfile_replaces_last_extension_for_object)
{
  TCCState s = {0};
  char *out;

  s.output_type = TCC_OUTPUT_OBJ;
  out = default_outputfile(&s, "dir.with.dots/name.test.c");
  UT_ASSERT_STREQ(out, "name.test.o");
  free_output(out);
  return 0;
}

UT_TEST(default_outputfile_keeps_extensionless_object_as_a_out)
{
  TCCState s = {0};
  char *out;

  s.output_type = TCC_OUTPUT_OBJ;
  out = default_outputfile(&s, "Makefile");
  UT_ASSERT_STREQ(out, "a.out");
  free_output(out);
  return 0;
}

UT_TEST(default_outputfile_uses_object_suffix_for_dependency_output)
{
  TCCState s = {0};
  char *out;

  s.just_deps = 1;
  out = default_outputfile(&s, "/tmp/source.c");
  UT_ASSERT_STREQ(out, "source.o");
  free_output(out);
  return 0;
}

UT_TEST(default_outputfile_relocatable_object_defaults_to_a_out)
{
  TCCState s = {0};
  char *out;

  s.output_type = TCC_OUTPUT_OBJ;
  s.option_r = 1;
  out = default_outputfile(&s, "source.c");
  UT_ASSERT_STREQ(out, "a.out");
  free_output(out);
  return 0;
}

UT_TEST(is_64bit_operand_identifies_wide_scalar_types)
{
  SValue sv = {0};

  sv.type.t = VT_LLONG;
  UT_ASSERT_EQ(tcc_driver_is_64bit_operand_for_unit_tests(&sv), 1);
  sv.type.t = VT_DOUBLE;
  UT_ASSERT_EQ(tcc_driver_is_64bit_operand_for_unit_tests(&sv), 1);
  sv.type.t = VT_LDOUBLE;
  UT_ASSERT_EQ(tcc_driver_is_64bit_operand_for_unit_tests(&sv), 1);
  return 0;
}

UT_TEST(is_64bit_operand_masks_type_qualifiers)
{
  SValue sv = {0};

  sv.type.t = VT_LLONG | VT_UNSIGNED | VT_VOLATILE;
  UT_ASSERT_EQ(tcc_driver_is_64bit_operand_for_unit_tests(&sv), 1);
  sv.type.t = VT_DOUBLE | VT_CONSTANT;
  UT_ASSERT_EQ(tcc_driver_is_64bit_operand_for_unit_tests(&sv), 1);
  return 0;
}

UT_TEST(is_64bit_operand_rejects_narrow_and_null_operands)
{
  SValue sv = {0};

  UT_ASSERT_EQ(tcc_driver_is_64bit_operand_for_unit_tests(NULL), 0);
  sv.type.t = VT_INT;
  UT_ASSERT_EQ(tcc_driver_is_64bit_operand_for_unit_tests(&sv), 0);
  sv.type.t = VT_FLOAT;
  UT_ASSERT_EQ(tcc_driver_is_64bit_operand_for_unit_tests(&sv), 0);
  return 0;
}

UT_SUITE(tcc_driver)
{
  UT_RUN(default_outputfile_uses_a_out_for_executable);
  UT_RUN(default_outputfile_uses_a_out_for_stdin_object_input);
  UT_RUN(default_outputfile_replaces_last_extension_for_object);
  UT_RUN(default_outputfile_keeps_extensionless_object_as_a_out);
  UT_RUN(default_outputfile_uses_object_suffix_for_dependency_output);
  UT_RUN(default_outputfile_relocatable_object_defaults_to_a_out);
  UT_RUN(is_64bit_operand_identifies_wide_scalar_types);
  UT_RUN(is_64bit_operand_masks_type_qualifiers);
  UT_RUN(is_64bit_operand_rejects_narrow_and_null_operands);
}

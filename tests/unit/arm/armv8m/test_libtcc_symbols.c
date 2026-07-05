/*
 *  test_libtcc_symbols.c - suite for libtcc.c: tcc_define_symbol(),
 *  tcc_undefine_symbol(), tcc_add_symbol(), tcc_add_dllref()
 *
 *  Part of the libtcc-api/ binary (build_libtcc_api/run_unit_tests_libtcc_api)
 *  -- see test_libtcc_lifecycle.c for the phase-0 rationale. This suite
 *  covers the "A-bucket" symbol-table entry points: pure state manipulation
 *  that doesn't require a real preprocessor/parser/ELF writer.
 *
 *  tcc_define_symbol()/tcc_undefine_symbol() write into s->cmdline_defs, a
 *  plain CString -- asserted directly against .data/.size using the real
 *  (verbatim-algorithm) cstr helpers linked via libtcc_api_stubs.c.
 *
 *  tcc_add_symbol() forwards to set_global_sym(), which is stubbed in
 *  libtcc_api_stubs.c to log the call count and last name passed through --
 *  used here to assert on the leading-underscore transform.
 *
 *  tcc_add_dllref() is ST_FUNC (empty macro in this build -> plain external
 *  linkage) and declared in tcc.h, so it is directly reachable without going
 *  through tcc_add_library()/tcc_add_dll() (which pull in file I/O and are
 *  stubbed out as no-ops in libtcc_api_stubs.c).
 */

#include "tcc.h"

#include "libtcc_api_stubs.h"
#include "ut.h"

#include <string.h>

/* ------------------------------------------------------------------ tests */

UT_TEST(test_define_symbol_explicit_value)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  tcc_define_symbol(s, "FOO", "1");

  /* Verified empirically: cstr_printf formats "#define %.*s %s\n" with
   * eq pointing at the symbol's NUL (no '=' in "FOO"), so the full name is
   * printed followed by the given value. cstr->size tracks vsnprintf's
   * returned length, which excludes the trailing NUL (the NUL still lands
   * in the buffer -- size_allocated has room for it -- but isn't counted). */
  UT_ASSERT_EQ(s->cmdline_defs.size, (int)strlen("#define FOO 1\n"));
  UT_ASSERT(0 == strcmp(s->cmdline_defs.data, "#define FOO 1\n"));

  tcc_delete(s);
  return 0;
}

UT_TEST(test_define_symbol_null_value_defaults_to_1)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  /* value == NULL and sym has no '=' -> eq points at sym's NUL, *eq == 0,
   * so value defaults to the literal "1" (verified by reading
   * tcc_define_symbol's real body in libtcc.c). */
  tcc_define_symbol(s, "BAR", NULL);

  UT_ASSERT(0 == strcmp(s->cmdline_defs.data, "#define BAR 1\n"));

  tcc_delete(s);
  return 0;
}

UT_TEST(test_define_symbol_embedded_eq_value_and_null_arg)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  /* sym == "BAZ=42", value == NULL: eq finds '=' inside sym, *eq != 0 so
   * value becomes eq+1 == "42"; only the part before '=' is used as the
   * name (eq - sym == 3 -> "BAZ"). */
  tcc_define_symbol(s, "BAZ=42", NULL);

  UT_ASSERT(0 == strcmp(s->cmdline_defs.data, "#define BAZ 42\n"));

  tcc_delete(s);
  return 0;
}

UT_TEST(test_undefine_symbol_appends_without_disturbing_prior)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  tcc_define_symbol(s, "FOO", "1");
  int size_after_define = s->cmdline_defs.size;
  UT_ASSERT_EQ(size_after_define, (int)strlen("#define FOO 1\n"));

  tcc_undefine_symbol(s, "FOO");

  /* Prior content must still be present, unmodified... */
  UT_ASSERT(0 == memcmp(s->cmdline_defs.data, "#define FOO 1\n", (size_t)size_after_define));
  /* ...followed by the appended undef form. */
  UT_ASSERT(0 == strcmp(s->cmdline_defs.data, "#define FOO 1\n#undef FOO\n"));

  tcc_delete(s);
  return 0;
}

UT_TEST(test_add_symbol_calls_set_global_sym_with_plain_name)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);
  lapi_reset();

  int dummy;
  s->leading_underscore = 0;
  int ret = tcc_add_symbol(s, "my_func", &dummy);

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(lapi_set_global_sym_call_count(), 1);
  UT_ASSERT(0 == strcmp(lapi_set_global_sym_last_name(), "my_func"));

  tcc_delete(s);
  return 0;
}

UT_TEST(test_add_symbol_leading_underscore_prefixes_name)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);
  lapi_reset();

  int dummy;
  s->leading_underscore = 1;
  int ret = tcc_add_symbol(s, "my_func", &dummy);

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(lapi_set_global_sym_call_count(), 1);
  UT_ASSERT(0 == strcmp(lapi_set_global_sym_last_name(), "_my_func"));

  tcc_delete(s);
  return 0;
}

UT_TEST(test_add_dllref_creates_new_ref)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  DLLReference *ref = tcc_add_dllref(s, "libfoo.so", 2);

  UT_ASSERT(ref != NULL);
  UT_ASSERT(0 == strcmp(ref->name, "libfoo.so"));
  UT_ASSERT_EQ(ref->level, 2);
  UT_ASSERT_EQ(s->nb_loaded_dlls, 1);
  UT_ASSERT(s->loaded_dlls[0] == ref);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_add_dllref_dedup_lowers_level_and_sets_found)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  DLLReference *ref1 = tcc_add_dllref(s, "libbar.so", 2);
  UT_ASSERT(ref1 != NULL);
  UT_ASSERT_EQ(ref1->found, 0);

  /* Same name, lower level -> level is lowered and found is set; no new
   * entry is created (dedup by name). */
  DLLReference *ref2 = tcc_add_dllref(s, "libbar.so", 1);
  UT_ASSERT(ref2 == ref1);
  UT_ASSERT_EQ(ref2->level, 1);
  UT_ASSERT_EQ(ref2->found, 1);
  UT_ASSERT_EQ(s->nb_loaded_dlls, 1);

  /* Same name, higher level -> level is NOT raised back up. */
  DLLReference *ref3 = tcc_add_dllref(s, "libbar.so", 5);
  UT_ASSERT(ref3 == ref1);
  UT_ASSERT_EQ(ref3->level, 1);
  UT_ASSERT_EQ(s->nb_loaded_dlls, 1);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_add_dllref_level_minus_one_is_lookup_only)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  /* Lookup of a name that hasn't been added yet -> NULL, and no entry is
   * created (per the real tcc_add_dllref body: level == -1 returns
   * immediately after the search, before the mallocz/dynarray_add path). */
  DLLReference *missing = tcc_add_dllref(s, "libqux.so", -1);
  UT_ASSERT(missing == NULL);
  UT_ASSERT_EQ(s->nb_loaded_dlls, 0);

  DLLReference *added = tcc_add_dllref(s, "libqux.so", 3);
  UT_ASSERT(added != NULL);
  UT_ASSERT_EQ(s->nb_loaded_dlls, 1);

  /* Now a level == -1 lookup finds it without disturbing its level. */
  DLLReference *found = tcc_add_dllref(s, "libqux.so", -1);
  UT_ASSERT(found == added);
  UT_ASSERT_EQ(found->level, 3);
  UT_ASSERT_EQ(s->nb_loaded_dlls, 1);

  tcc_delete(s);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(libtcc_symbols)
{
  UT_RUN(test_define_symbol_explicit_value);
  UT_RUN(test_define_symbol_null_value_defaults_to_1);
  UT_RUN(test_define_symbol_embedded_eq_value_and_null_arg);
  UT_RUN(test_undefine_symbol_appends_without_disturbing_prior);
  UT_RUN(test_add_symbol_calls_set_global_sym_with_plain_name);
  UT_RUN(test_add_symbol_leading_underscore_prefixes_name);
  UT_RUN(test_add_dllref_creates_new_ref);
  UT_RUN(test_add_dllref_dedup_lowers_level_and_sets_found);
  UT_RUN(test_add_dllref_level_minus_one_is_lookup_only);
}

/*
 *  test_libtcc_output_files.c - suite for libtcc.c: tcc_set_output_type(),
 *  and the guard-clause (no-open / zero-search-path) paths of
 *  tcc_add_file() and tcc_add_library().
 *
 *  Scope: this binary (libtcc-api/) links the real libtcc.c against
 *  libtcc_api_stubs.c, which no-ops the ELF/pipeline entry points
 *  (tccelf_new, tcc_load_*, tcc_compile-adjacent pieces are unreachable
 *  from tccgen_compile()/tcc_preprocess() stubs). That means only the
 *  guard-clause / early-return paths of these three entry points can be
 *  exercised here -- not a real compile, load, or library resolution.
 */

#include "tcc.h"

#include "ut.h"

/* ------------------------------------------------------------------ tests */

UT_TEST(test_set_output_type_preprocess_sets_field_and_returns_zero)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  /* Poison do_debug first so the assertion below actually proves
   * tcc_set_output_type() zeroed it (real logic: output_type ==
   * TCC_OUTPUT_PREPROCESS -> s->do_debug = 0; return 0;) rather than it
   * merely being mallocz()'d zero already. */
  s->do_debug = 1;

  int ret = tcc_set_output_type(s, TCC_OUTPUT_PREPROCESS);

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(s->output_type, TCC_OUTPUT_PREPROCESS);
  UT_ASSERT_EQ(s->do_debug, 0);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_output_type_memory_sets_field_and_returns_zero)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  int ret = tcc_set_output_type(s, TCC_OUTPUT_MEMORY);

  UT_ASSERT_EQ(ret, 0);
  UT_ASSERT_EQ(s->output_type, TCC_OUTPUT_MEMORY);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_add_file_missing_file_returns_file_not_found)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  /* open() fails immediately (ENOENT) -- the guard clause at the top of
   * tcc_add_file_internal() returns FILE_NOT_FOUND before any ELF or
   * compile pipeline entry point is reached. */
  int ret = tcc_add_file(s, "/this/path/definitely/does/not/exist.c");

  UT_ASSERT_EQ(ret, FILE_NOT_FOUND);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_add_library_no_search_paths_returns_file_not_found)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  /* tcc_set_output_type() is deliberately never called on this state, so
   * s->library_paths / s->nb_library_paths stay at their tcc_new()
   * zero-value defaults (NULL / 0). tcc_add_library_internal()'s search
   * loop is `for (i = 0; i < nb_paths; i++)`, i.e. a zero-iteration
   * no-op for both the "%s/lib%s.so" and "%s/lib%s.a" candidates, and
   * the final tcc_add_dll() fallback hits the same zero-iteration loop
   * -- so the whole call resolves deterministically to FILE_NOT_FOUND
   * without ever touching the filesystem. */
  UT_ASSERT_EQ(s->nb_library_paths, 0);

  int ret = tcc_add_library(s, "nonexistent_xyz_123");

  UT_ASSERT_EQ(ret, FILE_NOT_FOUND);

  tcc_delete(s);
  return 0;
}

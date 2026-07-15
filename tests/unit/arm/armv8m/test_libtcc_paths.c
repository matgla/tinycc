/*
 *  test_libtcc_paths.c - suite for libtcc.c: path-list management
 *
 *  Covers tcc_add_include_path(), tcc_add_sysinclude_path(),
 *  tcc_add_library_path() and tcc_set_lib_path() -- all thin wrappers
 *  around the static tcc_split_path() helper (PATHSEP-delimited splitting,
 *  tcc_strdup()'d storage, "{B}" token substitution from s->tcc_lib_path).
 *  No preprocessor/ELF machinery involved; every test is a plain
 *  tcc_new() / exercise / tcc_delete() cycle, same as
 *  test_libtcc_lifecycle.c.
 */

#include "tcc.h"

#include "ut.h"

#include <string.h>

/* ------------------------------------------------------------------ tests */

UT_TEST(test_add_include_path_grows_dynarray_and_stores_content)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(s->nb_include_paths, 0);

  const char *p0 = "/opt/inc0";
  const char *p1 = "/opt/inc1";
  const char *p2 = "/opt/inc2";

  UT_ASSERT_EQ(tcc_add_include_path(s, p0), 0);
  UT_ASSERT_EQ(s->nb_include_paths, 1);

  UT_ASSERT_EQ(tcc_add_include_path(s, p1), 0);
  UT_ASSERT_EQ(s->nb_include_paths, 2);

  UT_ASSERT_EQ(tcc_add_include_path(s, p2), 0);
  UT_ASSERT_EQ(s->nb_include_paths, 3);

  /* content matches what was passed in ... */
  UT_ASSERT(strcmp(s->include_paths[0], p0) == 0);
  UT_ASSERT(strcmp(s->include_paths[1], p1) == 0);
  UT_ASSERT(strcmp(s->include_paths[2], p2) == 0);

  /* ... but libtcc.c tcc_strdup()'d each path, so the stored pointer is
   * NOT the pointer we passed in. */
  UT_ASSERT(s->include_paths[0] != p0);
  UT_ASSERT(s->include_paths[1] != p1);
  UT_ASSERT(s->include_paths[2] != p2);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_add_sysinclude_path_grows_its_own_dynarray)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(s->nb_sysinclude_paths, 0);

  const char *p0 = "/usr/sysinc0";
  const char *p1 = "/usr/sysinc1";

  UT_ASSERT_EQ(tcc_add_sysinclude_path(s, p0), 0);
  UT_ASSERT_EQ(tcc_add_sysinclude_path(s, p1), 0);
  UT_ASSERT_EQ(s->nb_sysinclude_paths, 2);

  UT_ASSERT(strcmp(s->sysinclude_paths[0], p0) == 0);
  UT_ASSERT(strcmp(s->sysinclude_paths[1], p1) == 0);
  UT_ASSERT(s->sysinclude_paths[0] != p0);

  /* sysinclude_paths and include_paths are independent dynarrays. */
  UT_ASSERT_EQ(s->nb_include_paths, 0);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_add_library_path_colon_joined_splits_into_three_entries)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);
  UT_ASSERT_EQ(s->nb_library_paths, 0);

  UT_ASSERT_EQ(tcc_add_library_path(s, "/a:/b:/c"), 0);

  UT_ASSERT_EQ(s->nb_library_paths, 3);
  UT_ASSERT(strcmp(s->library_paths[0], "/a") == 0);
  UT_ASSERT(strcmp(s->library_paths[1], "/b") == 0);
  UT_ASSERT(strcmp(s->library_paths[2], "/c") == 0);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_set_lib_path_then_brace_b_token_substitutes_it)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  /* tcc_new() already set a default tcc_lib_path (CONFIG_TCCDIR); override
   * it, then use the "{B}" token in a later tcc_add_include_path() call --
   * tcc_split_path() must substitute the CURRENT s->tcc_lib_path, not the
   * default one. */
  tcc_set_lib_path(s, "/custom/lib");
  UT_ASSERT(strcmp(s->tcc_lib_path, "/custom/lib") == 0);

  UT_ASSERT_EQ(tcc_add_include_path(s, "{B}/include"), 0);

  UT_ASSERT_EQ(s->nb_include_paths, 1);
  UT_ASSERT(strcmp(s->include_paths[0], "/custom/lib/include") == 0);

  tcc_delete(s);
  return 0;
}

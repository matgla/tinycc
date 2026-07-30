/*
 *  test_libtcc_lifecycle.c - suite for libtcc.c: tcc_new()/tcc_delete()
 *
 *  Phase 0 of the libtcc-api/ binary: proves the real libtcc.c links
 *  against libtcc_api_stubs.c and that the state-lifecycle entry points
 *  work end-to-end (no preprocessor/ELF machinery involved on this path).
 */

#include "tcc.h"

#include "ut.h"

UT_TEST(test_tcc_new_sets_defaults)
{
  TCCState *s = tcc_new();
  UT_ASSERT(s != NULL);

  UT_ASSERT_EQ(s->tcc_ext, 1);
  UT_ASSERT_EQ(s->nocommon, 1);
  UT_ASSERT_EQ(s->dollars_in_identifiers, 1);
  UT_ASSERT_EQ(s->cversion, 201112);
  UT_ASSERT_EQ(s->float_abi, ARM_SOFTFP_FLOAT);
  UT_ASSERT_EQ(s->fpu_type, ARM_FPU_AUTO);
  UT_ASSERT(s->ppfp == stdout);
  UT_ASSERT(s->tcc_lib_path != NULL);

  tcc_delete(s);
  return 0;
}

UT_TEST(test_tcc_new_delete_cycle_no_leak)
{
  /* 5x new/delete under the project's default ASan build (see
   * ./configure) -- any leak in tcc_new()/tcc_delete()'s interaction with
   * this binary's stub layer aborts the test binary. */
  for (int i = 0; i < 5; i++)
  {
    TCCState *s = tcc_new();
    UT_ASSERT(s != NULL);
    tcc_delete(s);
  }
  return 0;
}

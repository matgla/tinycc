/*
 *  tcc_state_stub.c - shared UT stub, dual-build split.
 *
 *  This file is linked into several unit-test binaries with different needs:
 *    - The main (run_unit_tests), backend (UT2) and other binaries do NOT
 *      link libtcc.c, so they need the full stub layer below (allocators,
 *      the tcc_state global, ELF/section fakes, etc.).
 *    - build_ssaopt (UT11) links the REAL libtcc.c + ir/opt/ssa_opt*.c, which
 *      already own those symbols; it compiles every TU with -DUT_SSA_OPT_REAL
 *      (see the build_ssaopt rules in the Makefile), so the shared stubs must
 *      be skipped there to avoid multiple-definition clashes.
 *
 *  Same guard idiom as ra_link_stubs.c. Keep the two branches in sync when a
 *  new shared symbol is genuinely needed by BOTH builds (define it outside
 *  the guard in that case).
 */
#ifndef UT_SSA_OPT_REAL
/* ===== shared builds (main / backend / tccpp / ... ): full stub layer ===== */
/*
 *  tcc_state_stub.c - global TCCState pointer for unit tests
 *
 *  Modules that read tcc_state->* fields (e.g. tcc_ir_vreg_type_set_fp
 *  reading tcc_state->float_abi) need the ST_DATA TCCState *tcc_state
 *  symbol at link time.
 *
 *  Tests that care about specific field values must write them before
 *  calling the function under test.
 */

#define USING_GLOBALS
#include "tcc.h"

static TCCState ut_tcc_state_storage;
TCCState *tcc_state = &ut_tcc_state_storage;

#else /* UT_SSA_OPT_REAL — build_ssaopt (UT11) ===================== */
/*
 *  tcc_state_stub.c - stubs for TCC state initialization
 *
 *  Provides stubs for TCC state functions that are not needed in the
 *  isolated unit test environment.
 */

#define USING_GLOBALS
#include "tcc.h"

/* Stub: tcc_state_init (not used in unit tests). */
void tcc_state_init(TCCState *s)
{
  /* Do nothing. */
}

/* Stub: tcc_state_free (not used in unit tests). */
void tcc_state_free(TCCState *s)
{
  /* Do nothing. */
}
#endif /* UT_SSA_OPT_REAL */

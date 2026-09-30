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

/* Symbols of TUs these binaries do not link, reached from ones they do:
 * tccyaff.c's import set reads archives through tccelf.c, sret_nrvo.c
 * reads tccgen's current-function type, and the thumb backend names
 * helper symbols through strlit_pool.c.  Weak, so a binary that links the
 * real TU (build_tccelf's tccelf.c) keeps it. */
__attribute__((weak)) CType func_vt;
__attribute__((weak)) int func_vc;
__attribute__((weak)) ssize_t full_read(int fd, void *buf, size_t count)
{
  (void)fd; (void)buf; (void)count;
  return -1;
}
__attribute__((weak)) int tcc_archive_index_load(TCCState *s1, const char *path)
{
  (void)s1; (void)path;
  return -1;
}
__attribute__((weak)) int tcc_archive_index_has(TCCState *s1, const char *path, const char *name)
{
  (void)s1; (void)path; (void)name;
  return 0;
}
__attribute__((weak)) int tccelf_arm_fp_lib_is_shared(TCCState *s1)
{
  (void)s1;
  return 0;
}
__attribute__((weak)) const char *tccelf_get_fp_lib_name(TCCState *s1)
{
  (void)s1;
  return NULL;
}
__attribute__((weak)) Sym *external_helper_sym(int v)
{
  (void)v;
  return NULL;
}
/* codegen.c's word-aligned body start (cg_word_align_start). */
__attribute__((weak)) int func_align_pad;
__attribute__((weak)) void gen_fill_nops(int bytes)
{
  (void)bytes;
}

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

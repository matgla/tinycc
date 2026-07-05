/*
 *  codegen_backend_stubs.h - link stubs for the backend/ binary
 *  (build_backend/run_unit_tests_backend), which links the REAL
 *  arm-thumb-gen.c and arm-thumb-callsite.c directly (bypassing
 *  ir/codegen.c's dispatch loop and codegen_mop_stubs.c entirely) to call
 *  tcc_gen_machine_*_mop functions and assert on the real emitted Thumb-2
 *  bytes.
 *
 *  This is a DIFFERENT stub surface than codegen_mop_stubs.c (which fakes
 *  the mop functions themselves for the main run_unit_tests binary's
 *  dispatch-loop tests) -- here the mop functions are real; only their
 *  frontend/ELF-symbol-table dependencies are faked. See
 *  docs/plan_codegen_unit_tests.md §0 for why arm-thumb-gen.c can't be
 *  linked into the main binary at all.
 */

#ifndef TCC_UT_CODEGEN_BACKEND_STUBS_H
#define TCC_UT_CODEGEN_BACKEND_STUBS_H

#define USING_GLOBALS
#include "tcc.h"

/* Resets put_extern_sym()'s fake ELF-index counter. Call at the top of every
 * UT_TEST that (transitively) calls put_extern_sym (most mem/call/switch/fp
 * mop tests, via the literal-pool/relocation path). */
void cgb_reset(void);

#endif /* TCC_UT_CODEGEN_BACKEND_STUBS_H */

/*
 *  libtcc_api_stubs.h - link stubs for the libtcc-api/ binary
 *  (build_libtcc_api/run_unit_tests_libtcc_api), which links the REAL
 *  libtcc.c directly. Unlike every other stub file in this directory, this
 *  one does NOT coexist with stubs.c/tcc_state_stub.c: libtcc.c itself
 *  defines tcc_state, tcc_malloc/tcc_free/tcc_mallocz/tcc_realloc/
 *  tcc_strdup/libc_free, tcc_enter_state, and _tcc_error/_tcc_error_noabort
 *  for real -- linking stubs.c or tcc_state_stub.c here would be a
 *  multiple-definition error against those.
 *
 *  This binary tests libtcc.c's "A-bucket" surface (per the design
 *  investigation that scoped it): pure state/option/path manipulation that
 *  doesn't require a real preprocessor, parser, or ELF writer. Every
 *  pipeline entry point below (preprocess/tccgen/tccelf/tcc_load/
 *  tcc_assemble and friends) is a no-op stub -- deliberately never reached
 *  by an A-bucket test; if one ever is, that's a sign the test strayed into
 *  B-bucket territory.
 */

#ifndef TCC_UT_LIBTCC_API_STUBS_H
#define TCC_UT_LIBTCC_API_STUBS_H

#define USING_GLOBALS
#include "tcc.h"
#include "tccld.h"

/* set_global_sym() call log — lets tcc_add_symbol() tests assert on the name
 * (and leading-underscore transform) libtcc.c passed through. */
int lapi_set_global_sym_call_count(void);
const char *lapi_set_global_sym_last_name(void);
void lapi_reset(void);

#endif /* TCC_UT_LIBTCC_API_STUBS_H */

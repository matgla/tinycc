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

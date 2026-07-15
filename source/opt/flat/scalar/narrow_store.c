/*
 *  TCC IR - Narrow a plain STORE's value btype to its access width (pre-SSA)
 *
 *  A plain STORE derives its width from the DEST lvalue, but later transforms
 *  rewrite it into a STORE_INDEXED, which takes its width from the VALUE.  A
 *  wider (INT32) value forwarded into a char/short store would then widen the
 *  indexed store and clobber adjacent bytes.  Clamping the value btype to the
 *  access width keeps every later conversion narrow; it only narrows, never
 *  widens.  See docs/debugging_fuzz_divergences.md for the packed-bitfield case.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_dsl.h"
#include "opt/flat/narrow_store.h"

/* Access width in bytes for a narrow integer btype (INT8/INT16/INT32). */
static int narrow_store_int_width(int bt)
{
  switch (bt) {
  case IROP_BTYPE_INT8:  return 1;
  case IROP_BTYPE_INT16: return 2;
  case IROP_BTYPE_INT32: return 4;
  default:               return 0; /* INT64 / FP: not a narrowable integer */
  }
}

OPT_GEN_FLAT(narrow_store_value_btype, TCCIR_OP_STORE) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  int dbt = irop_get_btype(dest);
  int sw = narrow_store_int_width(irop_get_btype(src1));
  GUARD(
    when(dest.is_lval);
    and(dbt == IROP_BTYPE_INT8 || dbt == IROP_BTYPE_INT16);
    and(sw > narrow_store_int_width(dbt)));
  src1.btype = (uint8_t)dbt;
  REWRITE(.src1 = src1);
}

const IROptGen narrow_store_gens[] = {
    OPT_GEN_ENTRY_FLAT(narrow_store_value_btype, TCCIR_OP_STORE),
};

const int narrow_store_gens_count = sizeof(narrow_store_gens) / sizeof(narrow_store_gens[0]);

int tcc_ir_opt_narrow_store_value_btype(TCCIRState *ir)
{
  if (!ir)
    return 0;
  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_run_gens(&ctx, narrow_store_gens, narrow_store_gens_count);
  tcc_ir_opt_ctx_free(&ctx);
  return changes;
}

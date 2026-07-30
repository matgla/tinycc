/*
 *  TCC IR - SSA DCE: shared helpers
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "dce_common.h"


int sl_temp_has_live_uses(IRSSAOptCtx *ctx, int32_t vreg)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vreg);
  if (!vi)
    return 1;
  TCCIRState *ir = ctx->ir;
  for (int u = 0; u < vi->use_count; u++) {
    if (vi->uses[u].kind == SSA_USE_PHI)
      return 1;
    if (ir->compact_instructions[vi->uses[u].idx].op != TCCIR_OP_NOP)
      return 1;
  }
  return 0;
}

int sl_store_byte_width(int btype)
{
  switch (btype) {
  case IROP_BTYPE_INT8:   return 1;
  case IROP_BTYPE_INT16:  return 2;
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32: return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64: return 8;
  default: return 0;
  }
}

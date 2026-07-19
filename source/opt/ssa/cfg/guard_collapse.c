/*
 *  TCC IR - SSA sequential const-guard chain collapse
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
#include "opt/ssa/branch.h"
#include "load_cse.h"
#include "opt/ssa/fold.h"
#include "opt/ssa/cprop.h"
#include "global_store_dse.h"

extern int tcc_ir_opt_pass_disabled(const char *name);

/* Const-guard chains fold one guard per round, so iterate until branch folding runs dry. */
int tcc_ir_ssa_opt_guard_collapse(IRSSAOptCtx *ctx)
{
  if (tcc_ir_opt_pass_disabled("ssa:guard_collapse"))
    return 0;
  int total = 0;
  for (int guard = 0; guard < 4096; guard++) {
    total += ssa_opt_load_cse(ctx);
    total += ssa_opt_cprop(ctx);
    total += ssa_opt_fold(ctx);
    int br = ssa_opt_branch(ctx);
    if (!br)
      break;
    total += br + ssa_opt_dce_light(ctx);
  }
  if (total) {
    TCCIRState *ir = ctx->ir;
    for (int round = 0; round < 8; round++) {
      int c = 0;
      /* Retarget jumps at NOP'd instructions: eliminate_fallthrough compares raw indices. */
      int n = ir->next_instruction_index;
      for (int i = 0; i < n; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
          continue;
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int t = (int)irop_get_imm64_ex(ir, d);
        int nt = t;
        while (nt >= 0 && nt < n && ir->compact_instructions[nt].op == TCCIR_OP_NOP)
          nt++;
        if (nt != t && nt >= 0) {
          tcc_ir_set_dest(ir, i, irop_make_imm32(0, nt, irop_get_btype(d)));
          c++;
        }
      }
      c += tcc_ir_opt_eliminate_fallthrough(ir);
      c += ssa_opt_global_store_dse(ctx);
      total += c;
      if (!c)
        break;
    }
    total += ssa_opt_dce(ctx);
  }
  tcc_ir_dump_after_pass(ctx->ir, "ssa:guard_collapse");
  return total;
}

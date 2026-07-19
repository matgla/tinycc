/*
 *  TCC IR - SSA DCE: pass driver
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
#include "dce_passes.h"

extern int tcc_ir_opt_pass_disabled(const char *name);

int ssa_opt_dce_light(IRSSAOptCtx *ctx)
{
  int changes = dce_unreachable(ctx);
  changes += dce_temp_worklist(ctx);
  return changes;
}

int ssa_opt_dce(IRSSAOptCtx *ctx)
{
  int changes = 0;

  changes += dce_temp_worklist(ctx);
  changes += dce_unreachable(ctx);
  if (tcc_state->optimize >= 1) {
    int inner;
    do {
      inner = 0;
      inner += dce_dead_var_stores(ctx);
      if (inner)
        inner += dce_temp_worklist(ctx);
      changes += inner;
    } while (inner > 0);
    changes += dce_dead_overwrite_stores(ctx);
    if (!tcc_ir_opt_pass_disabled("ssa:dce:global_store"))
      changes += dce_dead_global_stores(ctx);
    if (!tcc_ir_opt_pass_disabled("ssa:dce:orphan_params"))
      changes += dce_orphan_params(ctx);
    changes += dce_dead_stackloc_stores(ctx);
    if (!tcc_ir_opt_pass_disabled("ssa:dce:ret_store"))
      changes += dce_ret_path_frame_store(ctx);
    if (!tcc_ir_opt_pass_disabled("ssa:dce:var_live")) {
      int vl = dce_var_liveness(ctx);
      if (vl)
        changes += vl + dce_temp_worklist(ctx);
    }
    if (changes) {
      /* Rebuild FULL use lists: resetting only use_count desyncs it from uses[]. */
      for (int p = 0; p < ctx->vinfo_cap; p++)
        ctx->vinfo[p].use_count = 0;
      for (int i = 0; i < ctx->ir->next_instruction_index; i++) {
        IRQuadCompact *q = &ctx->ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        ssa_opt_scan_instr_uses(ctx, i, q);
      }
      for (int b = 0; b < ctx->cfg->num_blocks; b++) {
        for (IRPhiNode *phi = ctx->ssa->block_phis[b]; phi; phi = phi->next) {
          for (int pi = 0; pi < phi->num_operands; pi++) {
            IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->operands[pi].vreg);
            if (vi)
              ssa_opt_add_use_phi(vi, b, pi);
          }
        }
      }
      changes += dce_temp_worklist(ctx);
    }
    if (!tcc_ir_opt_pass_disabled("ssa:dce:phi_cycles"))
      changes += dce_dead_phi_cycles(ctx);
  }

  return changes;
}

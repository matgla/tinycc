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
  return tcc_ir_ssa_opt_guard_collapse_ex(ctx, 0);
}

int tcc_ir_ssa_opt_guard_collapse_ex(IRSSAOptCtx *ctx, int idle_cleanup)
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
  {
    TCCIRState *ir = ctx->ir;
    /* The retarget/eliminate_fallthrough cleanup below rewrites jump targets
     * and removes terminators, which changes CFG block structure.  This SSA
     * engine only ever runs inside regalloc, BEFORE ra_resolve_phis consumes
     * ssa->block_phis against the construction-time CFG — so cleaning around
     * a live JUMPIF retargets it mid-(stale)-block, the phi edge match in
     * ra_resolve_phis fails, and that arm's phi copy is SILENTLY dropped
     * (fuzz seed int:3453, test 382).  ra_resolve_phis only succ-filters
     * JUMPIF edges (plain-JUMP preds take the unfiltered path), so the
     * residue-shape check below — no JUMPIF anywhere, no backward JUMP — is
     * the correctness gate for EVERY cleanup trigger, not just the idle one.
     * (History agrees: both looser variants miscompiled — see the memory
     * note on 20040629/20040705.) */
    int cleanup = 0;
    if (total != 0 || idle_cleanup) {
      /* The driver's own branch/dce passes may have folded the guards in an
       * earlier pipeline iteration, leaving JUMPs that only hop over NOP'd
       * abort arms.  Such a fall-through JUMP fences global_store_dse's
       * pending window, keeping every re-store of a global alive
       * (20040629-1 main: 49 stores instead of 3) — so `total == 0` does
       * not mean the cleanup below has nothing to do.  Detect exactly that
       * shape (a JUMP forward onto/over nothing but NOPs) cheaply, and ONLY
       * in functions whose entire remaining control flow is such residue:
       * any JUMPIF, or any backward JUMP, disqualifies.  In a fully-folded
       * guard chain every conditional is already gone, so this still covers
       * the target shape — while perturbing NOTHING that still has live
       * branches: cleaning around live control flow shifted downstream
       * shapes and exposed real latent miscompiles (930529-1 /
       * builtin-bitops-1 with loops admitted; fuzz seeds 344/504/648/996
       * diamond phi-join reading the arm-clobbered condition register with
       * forward JUMPIFs admitted). */
      int n = ir->next_instruction_index;
      int fallthrough_jmp = 0;
      for (int i = 0; i < n; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_JUMPIF) {
          fallthrough_jmp = 0;
          break;
        }
        if (q->op != TCCIR_OP_JUMP)
          continue;
        int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
        if (t <= i) {
          fallthrough_jmp = 0;
          break;
        }
        int nt = i + 1;
        while (nt < n && ir->compact_instructions[nt].op == TCCIR_OP_NOP)
          nt++;
        if (t <= nt)
          fallthrough_jmp = 1;
      }
      cleanup = fallthrough_jmp;
    }
    if (cleanup) {
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
  }
  tcc_ir_dump_after_pass(ctx->ir, "ssa:guard_collapse");
  return total;
}

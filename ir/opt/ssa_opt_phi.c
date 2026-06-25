/*
 *  TCC IR - SSA Phi Simplification
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

/* ============================================================================
 * Phi Simplification
 *
 * Eliminates trivial phi nodes:
 *   1. All operands are the same vreg → replace dest with that vreg
 *   2. All operands are the same vreg or the phi dest itself → same
 *   3. Single operand (degenerate) → replace dest with that operand
 *
 * Example:
 *   phi T5 = [T3, T3, T3]  →  replace all uses of T5 with T3
 *   phi T5 = [T3, T5, T3]  →  replace all uses of T5 with T3 (self-ref)
 * ============================================================================ */

int ssa_opt_phi_simplify(IRSSAOptCtx *ctx)
{
  IRSSAState *ssa = ctx->ssa;
  IRCFG *cfg = ctx->cfg;
  int changes = 0;
  int progress;

  do {
    progress = 0;
    for (int b = 0; b < cfg->num_blocks; b++) {
      IRPhiNode **pp = &ssa->block_phis[b];
      while (*pp) {
        IRPhiNode *phi = *pp;
        int32_t unique = -1;
        int trivial = 1;

        for (int i = 0; i < phi->num_operands; i++) {
          int32_t v = phi->operands[i].vreg;
          if (v < 0 || v == phi->dest_vreg)
            continue;
          if (unique < 0) {
            unique = v;
          } else if (v != unique) {
            trivial = 0;
            break;
          }
        }

        if (!trivial || unique < 0) {
          pp = &(*pp)->next;
          continue;
        }

        /* Replace all uses of phi->dest_vreg with unique */
        ssa_opt_replace_all_uses(ctx, phi->dest_vreg, unique);

        /* Remove phi from the list */
        *pp = phi->next;
        tcc_free(phi->operands);
        tcc_free(phi);

        progress++;
        changes++;
      }
    }
  } while (progress > 0);

  return changes;
}

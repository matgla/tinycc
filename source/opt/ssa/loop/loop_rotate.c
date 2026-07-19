/*
 *  TCC IR - SSA loop: rotation (top-tested to bottom-tested)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_loop_utils.h"
#include "opt_utils.h"

#define SSA_LOOP_ROTATE_MAX_PASSES 4

int ssa_opt_loop_rotate(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;
  if (!tcc_ir_cfg_flat_has_backedge(ir))
    return 0;

  int total = 0;
  for (int pass = 0; pass < SSA_LOOP_ROTATE_MAX_PASSES; pass++) {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1) {
      tcc_ir_cfg_free(cfg);
      break;
    }
    tcc_ir_cfg_compute_dominators(cfg);

    int cand_cap = cfg->num_blocks;
    IRLoop *cands = tcc_mallocz(sizeof(IRLoop) * (size_t)cand_cap);
    int ncands = 0;
    for (int b = 0; b < cfg->num_blocks && ncands < cand_cap; b++) {
      IRBasicBlock *bb = &cfg->blocks[b];
      for (int si = 0; si < bb->num_succs && ncands < cand_cap; si++) {
        int h = bb->succs[si];
        if (h < 0 || h >= cfg->num_blocks)
          continue;
        if (!tcc_ir_cfg_dominates(cfg, h, b))
          continue; /* not a back-edge */
        IRLoop *lp = &cands[ncands++];
        lp->header_idx = cfg->blocks[h].start_idx;
        lp->start_idx = cfg->blocks[h].start_idx;
        lp->end_idx = cfg->blocks[b].end_idx;
        lp->preheader_idx = -1;
        lp->body_instrs = NULL;
        lp->num_body_instrs = 0;
        lp->body_instrs_capacity = 0;
        lp->depth = 0;
      }
    }

    /* The in-place rewrite works on flat IR and invalidates this CFG. */
    tcc_ir_cfg_free(cfg);

    /* Rotate inner (smallest) loops before outer ones. */
    if (ncands > 1)
      qsort(cands, ncands, sizeof(IRLoop), loop_size_cmp);

    int pass_rotated = 0;
    for (int i = 0; i < ncands; i++) {
      /* Rotation is the stronger transform (it also drops the per-iteration
       * top test), so try it first; relayout picks up the many shapes its
       * safety gates decline and removes the trampoline jumps at least. */
      if (try_rotate_loop(ir, &cands[i]))
        pass_rotated++;
      else if (!tcc_ir_opt_pass_disabled("loop_relayout") && try_relayout_loop(ir, &cands[i]))
        pass_rotated++;
    }
    tcc_free(cands);

    total += pass_rotated;
    if (pass_rotated == 0)
      break;
  }
  return total;
}

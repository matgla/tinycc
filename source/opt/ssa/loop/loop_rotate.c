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

/* Order candidates innermost-first, and siblings in source order.
 *
 * `loop_size_cmp` measures end_idx - start_idx, and in tcc's un-rotated shape
 * end_idx is the LATCH block's end — the body sits past the back-edge and is
 * not counted at all.  Every top-tested loop then measures the same handful of
 * instructions, so nesting is invisible and an outer loop can be rotated first.
 * try_rotate_loop then refuses the inner one outright ("nested inside an
 * already-rotated loop"), which is what kept bubble sort's inner loop — 2016 of
 * its 2079 iterations — top-tested.
 *
 * `depth` carries the containment depth computed below from the flood-filled
 * extents.  Siblings tie on depth and fall back to header order: a chain of
 * sequential loops over one IV must be rotated front to back, because
 * rot_guard_provably_folds reads the PRECEDING loop's exit value to fold the
 * next one's zero-trip guard (the memcpy-a* / memclr shape; ordering them by
 * size instead costs 60k cycles each). */
static int loop_nest_cmp(const void *a, const void *b)
{
  const IRLoop *la = (const IRLoop *)a;
  const IRLoop *lb = (const IRLoop *)b;
  if (la->depth != lb->depth)
    return lb->depth - la->depth; /* deeper first */
  return la->header_idx - lb->header_idx;
}

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
    /* scratch for the per-candidate natural-loop flood fill (see loop_nest_cmp) */
    uint8_t *in_loop = tcc_mallocz((size_t)cfg->num_blocks);
    int *wl = tcc_mallocz(sizeof(int) * (size_t)cfg->num_blocks);
    int *extent = tcc_mallocz(sizeof(int) * (size_t)cand_cap);
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
        /* Flood-fill the natural loop and record its full extent. */
        memset(in_loop, 0, (size_t)cfg->num_blocks);
        in_loop[h] = 1;
        int nwl = 0;
        if (b != h) {
          in_loop[b] = 1;
          wl[nwl++] = b;
        }
        int hi_idx = cfg->blocks[h].end_idx;
        if (cfg->blocks[b].end_idx > hi_idx)
          hi_idx = cfg->blocks[b].end_idx;
        while (nwl > 0) {
          int node = wl[--nwl];
          IRBasicBlock *nb = &cfg->blocks[node];
          if (nb->end_idx > hi_idx)
            hi_idx = nb->end_idx;
          for (int pi = 0; pi < nb->num_preds; pi++) {
            int pr = nb->preds[pi];
            if (pr >= 0 && pr < cfg->num_blocks && !in_loop[pr]) {
              in_loop[pr] = 1;
              wl[nwl++] = pr;
            }
          }
        }
        /* The body lives past the back-edge and is reached by a forward JUMP
         * from the header, so a predecessor flood fill does not always see it;
         * take the header's outgoing jump targets into account too. */
        {
          for (int ii = cfg->blocks[h].start_idx; ii < cfg->blocks[h].end_idx; ii++) {
            IRQuadCompact *q = &ir->compact_instructions[ii];
            if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
              continue;
            int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
            if (t > hi_idx && t < ir->next_instruction_index)
              hi_idx = t;
          }
        }
        extent[ncands - 1] = hi_idx;
      }
    }
    tcc_free(in_loop);
    tcc_free(wl);
    /* containment depth: how many other candidates fully enclose this one */
    for (int i = 0; i < ncands; i++) {
      int d = 0;
      for (int j = 0; j < ncands; j++) {
        if (j == i)
          continue;
        if (cands[j].header_idx <= cands[i].header_idx && extent[j] >= extent[i] &&
            (cands[j].header_idx < cands[i].header_idx || extent[j] > extent[i]))
          d++;
      }
      cands[i].depth = d;
    }
    tcc_free(extent);

    /* The in-place rewrite works on flat IR and invalidates this CFG. */
    tcc_ir_cfg_free(cfg);

    /* Rotate inner (smallest) loops before outer ones. */
    if (ncands > 1)
      qsort(cands, ncands, sizeof(IRLoop), loop_nest_cmp);

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

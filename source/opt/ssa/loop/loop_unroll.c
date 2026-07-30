/*
 *  TCC IR - SSA loop: unrolling / constant-trip elimination
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
#include "loop_cand.h"

#define SSA_LOOP_UNROLL_MAX_PASSES 4
#define SSA_UNROLL_MAX_SPAN 256

static int unroll_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
                                uint8_t *member, uint8_t *scratch)
{
  IRBasicBlock *hb = &cfg->blocks[header_b];
  lcs_collect_header_members(cfg, header_b, member, scratch);

  int eff_start = ir->next_instruction_index;
  int eff_end = -1;
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!member[b])
      continue;
    if (cfg->blocks[b].start_idx < eff_start)
      eff_start = cfg->blocks[b].start_idx;
    if (cfg->blocks[b].end_idx - 1 > eff_end)
      eff_end = cfg->blocks[b].end_idx - 1;
  }
  if (eff_end < eff_start)
    return 0;
  if (eff_end - eff_start > SSA_UNROLL_MAX_SPAN)
    return 0;

  /* Contiguity: every non-NOP in the span must be a member. */
  for (int i = eff_start; i <= eff_end; i++) {
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    int b = cfg->instr_to_block[i];
    if (b < 0 || b >= cfg->num_blocks || !member[b])
      return 0;
  }
  /* Single-entry: header must dominate every member. */
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (member[b] && !tcc_ir_cfg_dominates(cfg, header_b, b))
      return 0;
  }

  int preheader = eff_start - 1;
  while (preheader >= 0) {
    int pop = ir->compact_instructions[preheader].op;
    if (pop != TCCIR_OP_JUMP && pop != TCCIR_OP_JUMPIF)
      break;
    preheader--;
  }

  IRLoop loop = {0};
  loop.header_idx = hb->start_idx;
  loop.start_idx = eff_start;
  loop.end_idx = eff_end; /* latch back-edge instruction */
  loop.preheader_idx = preheader;
  loop.depth = 1;

  if (try_eliminate_loop(ir, &loop))
    return 1;
  if (try_eliminate_loop_symbolic(ir, &loop))
    return 1;

  /* try_unroll_loop_ex enables its IR-growth path only when num_loops==1. */
  IRLoops one = {0};
  one.loops = &loop;
  one.num_loops = 1;
  one.capacity = 1;
  return try_unroll_loop_ex(ir, &loop, &one, 0);
}

int ssa_opt_loop_unroll(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;
  if (!tcc_ir_cfg_flat_has_backedge(ir))
    return 0;

  int total = 0;
  for (int pass = 0; pass < SSA_LOOP_UNROLL_MAX_PASSES; pass++) {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1) {
      tcc_ir_cfg_free(cfg);
      break;
    }
    tcc_ir_cfg_compute_dominators(cfg);

    int nb = cfg->num_blocks;
    uint8_t *is_header = tcc_mallocz((size_t)nb);
    for (int b = 0; b < nb; b++) {
      IRBasicBlock *bb = &cfg->blocks[b];
      for (int si = 0; si < bb->num_succs; si++) {
        int h = bb->succs[si];
        if (h >= 0 && h < nb && tcc_ir_cfg_dominates(cfg, h, b))
          is_header[h] = 1;
      }
    }

    uint8_t *member = tcc_malloc((size_t)nb);
    uint8_t *scratch = tcc_malloc((size_t)nb);
    uint8_t *other = tcc_malloc((size_t)nb);

    int changed = 0;
    for (int h = 0; h < nb && !changed; h++) {
      if (!is_header[h])
        continue;
      /* Outermost only, for soundness: a nested loop's IV init would be read from the enclosing preheader. */
      int inner = 0;
      for (int h2 = 0; h2 < nb && !inner; h2++) {
        if (h2 == h || !is_header[h2])
          continue;
        lcs_collect_header_members(cfg, h2, other, scratch);
        if (other[h])
          inner = 1;
      }
      if (inner)
        continue;
      changed = unroll_try_candidate(ir, cfg, h, member, scratch);
      if (changed)
        LOG_IR_GEN("[ssa:loop_unroll] transformed header_b=%d", h);
    }

    tcc_free(is_header);
    tcc_free(member);
    tcc_free(scratch);
    tcc_free(other);
    tcc_ir_cfg_free(cfg);

    total += changed;
    if (!changed)
      break;
  }
  return total;
}

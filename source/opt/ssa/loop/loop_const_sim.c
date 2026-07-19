/*
 *  TCC IR - SSA loop: constant simulation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_loop_const_sim.h"
#include "loop_cand.h"

#define SSA_LOOP_CONST_SIM_MAX_PASSES 4
#define SSA_LCS_MAX_SPAN 256

static int lcs_span_has_memory(TCCIRState *ir, int start_idx, int end_idx)
{
  for (int i = start_idx; i <= end_idx; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_STORE ||
        q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC ||
        q->op == TCCIR_OP_BLOCK_COPY)
      return 1;
    if (irop_config[q->op].has_src1 && tcc_ir_op_get_src1(ir, q).is_lval)
      return 1;
    if (irop_config[q->op].has_src2 && tcc_ir_op_get_src2(ir, q).is_lval)
      return 1;
    if (q->op == TCCIR_OP_MLA && tcc_ir_op_get_accum(ir, q).is_lval)
      return 1;
  }
  return 0;
}

/* `member`/`scratch` are num_blocks-sized caller scratch. */
static int lcs_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
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
  if (eff_end - eff_start > SSA_LCS_MAX_SPAN)
    return 0;

  /* Contiguity: every non-NOP instruction in the span belongs to a member. */
  for (int i = eff_start; i <= eff_end; i++) {
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    int b = cfg->instr_to_block[i];
    if (b < 0 || b >= cfg->num_blocks || !member[b])
      return 0;
  }

  /* Single-entry: only the header may be entered from outside the span. */
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!member[b])
      continue;
    if (!tcc_ir_cfg_dominates(cfg, header_b, b))
      return 0;
  }

  if (lcs_span_has_memory(ir, eff_start, eff_end))
    return 0;

  /* Preheader: last non-jump instruction before the span, where the IV init is sought. */
  int preheader = eff_start - 1;
  while (preheader >= 0) {
    int pop = ir->compact_instructions[preheader].op;
    if (pop != TCCIR_OP_JUMP && pop != TCCIR_OP_JUMPIF)
      break;
    preheader--;
  }

  return lcs_fold_region(ir, eff_start, eff_end, hb->start_idx, preheader,
                         /*allow_extension*/ 0);
}

int ssa_opt_loop_const_sim(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;
  if (!tcc_ir_cfg_flat_has_backedge(ir))
    return 0;

  int total = 0;
  for (int pass = 0; pass < SSA_LOOP_CONST_SIM_MAX_PASSES; pass++) {
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

    int folded = 0;
    for (int h = 0; h < nb; h++) {
      if (!is_header[h])
        continue;
      /* Outermost only: decline a header nested in another header's loop. */
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
      if (lcs_try_candidate(ir, cfg, h, member, scratch)) {
        folded++;
        LOG_IR_GEN("[ssa:loop_const_sim] folded header_b=%d", h);
      }
    }

    tcc_free(is_header);
    tcc_free(member);
    tcc_free(scratch);
    tcc_free(other);
    tcc_ir_cfg_free(cfg);

    total += folded;
    if (!folded)
      break;
  }
  return total;
}

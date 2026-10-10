/*
 *  TCC IR - SSA loop: natural-loop candidate helpers
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ir.h"
#include "ssa_opt.h"
#include "loop_cand.h"

int fie_cand_cmp(const void *a, const void *b)
{
  const FieCand *ca = a, *cb = b;
  if (ca->size != cb->size)
    return ca->size - cb->size;
  return ca->header_b - cb->header_b;
}

/* Back-edge candidates, smallest span first; caller frees. */
FieCand *fie_collect_cands(IRCFG *cfg, int *out_nc)
{
  int cap = cfg->num_blocks;
  FieCand *cands = tcc_mallocz(sizeof(FieCand) * (size_t)cap);
  int nc = 0;
  for (int b = 0; b < cfg->num_blocks && nc < cap; b++) {
    IRBasicBlock *bb = &cfg->blocks[b];
    for (int si = 0; si < bb->num_succs && nc < cap; si++) {
      int h = bb->succs[si];
      if (h < 0 || h >= cfg->num_blocks)
        continue;
      if (!tcc_ir_cfg_dominates(cfg, h, b))
        continue;
      cands[nc].header_b = h;
      cands[nc].latch_b = b;
      cands[nc].size = cfg->blocks[b].end_idx - cfg->blocks[h].start_idx;
      nc++;
    }
  }
  if (nc > 0)
    tcc_qsort(cands, nc, sizeof(FieCand), fie_cand_cmp);
  *out_nc = nc;
  return cands;
}

/* The one out-of-loop predecessor of a header whose preds are exactly {entry, latch}, else -1. */
int fie_entry_pred(IRBasicBlock *hb, int latch_b, const uint8_t *member)
{
  if (hb->num_preds != 2)
    return -1;
  int latch_seen = 0, entry_pred = -1;
  for (int i = 0; i < hb->num_preds; i++) {
    int p = hb->preds[i];
    if (p == latch_b && !latch_seen)
      latch_seen = 1;
    else
      entry_pred = p;
  }
  if (!latch_seen || entry_pred < 0 || member[entry_pred])
    return -1;
  return entry_pred;
}

/* Requires header_b to dominate latch_b, so the backward walk cannot escape. */
void fie_collect_members(IRCFG *cfg, int header_b, int latch_b,
                                uint8_t *member)
{
  memset(member, 0, (size_t)cfg->num_blocks);
  member[header_b] = 1;
  if (latch_b == header_b)
    return;

  int *stack = tcc_malloc(sizeof(int) * (size_t)cfg->num_blocks);
  int sp = 0;
  member[latch_b] = 1;
  stack[sp++] = latch_b;
  while (sp > 0) {
    IRBasicBlock *bb = &cfg->blocks[stack[--sp]];
    for (int i = 0; i < bb->num_preds; i++) {
      int p = bb->preds[i];
      if (p >= 0 && p < cfg->num_blocks && !member[p]) {
        member[p] = 1;
        stack[sp++] = p;
      }
    }
  }
  tcc_free(stack);
}
/* `scratch` is a num_blocks caller-provided buffer. */
void lcs_collect_header_members(IRCFG *cfg, int header_b, uint8_t *member,
                                       uint8_t *scratch)
{
  memset(member, 0, (size_t)cfg->num_blocks);
  for (int b = 0; b < cfg->num_blocks; b++) {
    IRBasicBlock *bb = &cfg->blocks[b];
    for (int si = 0; si < bb->num_succs; si++) {
      if (bb->succs[si] != header_b)
        continue;
      if (!tcc_ir_cfg_dominates(cfg, header_b, b))
        continue;
      fie_collect_members(cfg, header_b, b, scratch);
      for (int k = 0; k < cfg->num_blocks; k++)
        if (scratch[k])
          member[k] = 1;
    }
  }
}

void lcs_mark_headers(IRCFG *cfg, uint8_t *is_header)
{
  int nb = cfg->num_blocks;
  for (int b = 0; b < nb; b++) {
    IRBasicBlock *bb = &cfg->blocks[b];
    for (int si = 0; si < bb->num_succs; si++) {
      int h = bb->succs[si];
      if (h >= 0 && h < nb && tcc_ir_cfg_dominates(cfg, h, b))
        is_header[h] = 1;
    }
  }
}

/* Contiguous single-entry span of the loop headed at header_b; max_span 0 = unbounded. */
int lcs_cand_span(TCCIRState *ir, IRCFG *cfg, int header_b, uint8_t *member,
                  uint8_t *scratch, int max_span, LcsSpan *out)
{
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
  if (max_span && eff_end - eff_start > max_span)
    return 0;

  for (int i = eff_start; i <= eff_end; i++) {
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    int b = cfg->instr_to_block[i];
    if (b < 0 || b >= cfg->num_blocks || !member[b])
      return 0;
  }
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
  out->start = eff_start;
  out->end = eff_end;
  out->header = cfg->blocks[header_b].start_idx;
  out->preheader = preheader;
  return 1;
}

/* Rebuilds the CFG per pass and offers every outermost loop header to fn. */
int lcs_run_outermost(TCCIRState *ir, int max_passes, int stop_first,
                      LcsCandFn fn)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;
  if (!tcc_ir_cfg_flat_has_backedge(ir))
    return 0;

  int total = 0;
  for (int pass = 0; pass < max_passes; pass++) {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1) {
      tcc_ir_cfg_free(cfg);
      break;
    }
    tcc_ir_cfg_compute_dominators(cfg);

    int nb = cfg->num_blocks;
    uint8_t *is_header = tcc_mallocz((size_t)nb);
    lcs_mark_headers(cfg, is_header);

    uint8_t *member = tcc_malloc((size_t)nb);
    uint8_t *scratch = tcc_malloc((size_t)nb);
    uint8_t *other = tcc_malloc((size_t)nb);

    int fired = 0;
    for (int h = 0; h < nb && !(stop_first && fired); h++) {
      if (!is_header[h])
        continue;
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
      if (fn(ir, cfg, h, member, scratch))
        fired++;
    }

    tcc_free(is_header);
    tcc_free(member);
    tcc_free(scratch);
    tcc_free(other);
    tcc_ir_cfg_free(cfg);

    total += fired;
    if (!fired)
      break;
  }
  return total;
}

/* Like lcs_run_outermost, but over the LEAF loops: those whose body holds no
 * other loop header.  For a transformation of the innermost loop only (a
 * constant-trip loop nested in a loop): an outer loop becomes a leaf, and a
 * candidate, once its inner loops are gone. */
int lcs_run_leaf(TCCIRState *ir, int max_passes, int stop_first, LcsCandFn fn)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;
  if (!tcc_ir_cfg_flat_has_backedge(ir))
    return 0;

  int total = 0;
  for (int pass = 0; pass < max_passes; pass++) {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1) {
      tcc_ir_cfg_free(cfg);
      break;
    }
    tcc_ir_cfg_compute_dominators(cfg);

    int nb = cfg->num_blocks;
    uint8_t *is_header = tcc_mallocz((size_t)nb);
    lcs_mark_headers(cfg, is_header);

    uint8_t *member = tcc_malloc((size_t)nb);
    uint8_t *scratch = tcc_malloc((size_t)nb);

    int fired = 0;
    for (int h = 0; h < nb && !(stop_first && fired); h++) {
      if (!is_header[h])
        continue;
      lcs_collect_header_members(cfg, h, member, scratch);
      int nested = 0;
      for (int h2 = 0; h2 < nb && !nested; h2++)
        nested = h2 != h && is_header[h2] && member[h2];
      if (nested)
        continue;
      if (fn(ir, cfg, h, member, scratch))
        fired++;
    }

    tcc_free(is_header);
    tcc_free(member);
    tcc_free(scratch);
    tcc_ir_cfg_free(cfg);

    total += fired;
    if (!fired)
      break;
  }
  return total;
}

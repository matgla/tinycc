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

/*
 *  TCC IR - Control Flow Graph and Dominator Tree
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"

static void cfg_add_edge(IRCFG *cfg, int from, int to)
{
  IRBasicBlock *fb = &cfg->blocks[from];
  IRBasicBlock *tb = &cfg->blocks[to];
  if (fb->num_succs < 2)
    fb->succs[fb->num_succs++] = to;
  if (tb->num_preds >= tb->preds_cap) {
    int nc = tb->preds_cap ? tb->preds_cap * 2 : 4;
    tb->preds = tcc_realloc(tb->preds, nc * sizeof(int));
    tb->preds_cap = nc;
  }
  tb->preds[tb->num_preds++] = to == from ? from : from;
}

IRCFG *tcc_ir_cfg_build(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return NULL;

  IRCFG *cfg = tcc_mallocz(sizeof(IRCFG));
  cfg->num_instrs = n;

  /* Mark leaders — recompute jump targets from scratch (don't trust
   * stale is_jump_target flags from previous optimization passes). */
  uint8_t *is_leader = tcc_mallocz(n);
  is_leader[0] = 1;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);
      if (target >= 0 && target < n) {
        is_leader[target] = 1;
      }
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF ||
        q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
        q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE) {
      if (i + 1 < n) {
        is_leader[i + 1] = 1;
      }
    }
  }

  /* Count blocks */
  int nb = 0;
  for (int i = 0; i < n; i++)
    if (is_leader[i])
      nb++;

  cfg->capacity = nb;
  cfg->blocks = tcc_mallocz(nb * sizeof(IRBasicBlock));
  cfg->instr_to_block = tcc_mallocz(n * sizeof(int));

  /* Create blocks */
  int bi = -1;
  for (int i = 0; i < n; i++) {
    if (is_leader[i]) {
      if (bi >= 0)
        cfg->blocks[bi].end_idx = i;
      bi++;
      cfg->blocks[bi].start_idx = i;
      cfg->blocks[bi].succs[0] = -1;
      cfg->blocks[bi].succs[1] = -1;
      cfg->blocks[bi].idom = -1;
      cfg->blocks[bi].rpo_number = -1;
    }
    cfg->instr_to_block[i] = bi;
  }
  if (bi >= 0)
    cfg->blocks[bi].end_idx = n;
  cfg->num_blocks = bi + 1;

  tcc_free(is_leader);

  /* Build edges */
  for (int b = 0; b < cfg->num_blocks; b++) {
    int last = cfg->blocks[b].end_idx - 1;
    if (last < cfg->blocks[b].start_idx)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[last];

    if (q->op == TCCIR_OP_JUMP) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);
      if (target >= 0 && target < n)
        cfg_add_edge(cfg, b, cfg->instr_to_block[target]);
    }
    else if (q->op == TCCIR_OP_JUMPIF) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);
      if (target >= 0 && target < n)
        cfg_add_edge(cfg, b, cfg->instr_to_block[target]);
      if (b + 1 < cfg->num_blocks)
        cfg_add_edge(cfg, b, b + 1);
    }
    else if (q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
             q->op == TCCIR_OP_IJUMP) {
      /* no successors (IJUMP: conservative — skip loops containing it) */
    }
    else {
      if (b + 1 < cfg->num_blocks)
        cfg_add_edge(cfg, b, b + 1);
    }
  }

  return cfg;
}

void tcc_ir_cfg_free(IRCFG *cfg)
{
  if (!cfg)
    return;
  for (int i = 0; i < cfg->num_blocks; i++)
    tcc_free(cfg->blocks[i].preds);
  tcc_free(cfg->blocks);
  tcc_free(cfg->rpo_order);
  tcc_free(cfg->instr_to_block);
  tcc_free(cfg);
}

/* Iterative DFS for reverse postorder */
static void cfg_compute_rpo(IRCFG *cfg)
{
  int nb = cfg->num_blocks;
  if (nb == 0)
    return;

  uint8_t *visited = tcc_mallocz(nb);
  int *postorder = tcc_mallocz(nb * sizeof(int));
  int po_count = 0;

  /* Iterative DFS using explicit stack: (block, child_index) */
  typedef struct { int block; int ci; } DFSFrame;
  DFSFrame *stack = tcc_mallocz(nb * sizeof(DFSFrame));
  int sp = 0;

  visited[0] = 1;
  stack[sp++] = (DFSFrame){0, 0};

  while (sp > 0) {
    DFSFrame *top = &stack[sp - 1];
    IRBasicBlock *bb = &cfg->blocks[top->block];
    if (top->ci < bb->num_succs) {
      int s = bb->succs[top->ci];
      top->ci++;
      if (s >= 0 && s < nb && !visited[s]) {
        visited[s] = 1;
        stack[sp++] = (DFSFrame){s, 0};
      }
    }
    else {
      postorder[po_count++] = top->block;
      sp--;
    }
  }

  /* Reverse postorder */
  cfg->rpo_order = tcc_mallocz(po_count * sizeof(int));
  cfg->rpo_count = po_count;
  for (int i = 0; i < po_count; i++) {
    int b = postorder[po_count - 1 - i];
    cfg->rpo_order[i] = b;
    cfg->blocks[b].rpo_number = i;
  }

  tcc_free(visited);
  tcc_free(postorder);
  tcc_free(stack);
}

/* Cooper-Harvey-Kennedy dominator tree */
static int cfg_intersect(IRCFG *cfg, int b1, int b2)
{
  while (b1 != b2) {
    while (cfg->blocks[b1].rpo_number > cfg->blocks[b2].rpo_number)
      b1 = cfg->blocks[b1].idom;
    while (cfg->blocks[b2].rpo_number > cfg->blocks[b1].rpo_number)
      b2 = cfg->blocks[b2].idom;
  }
  return b1;
}

void tcc_ir_cfg_compute_dominators(IRCFG *cfg)
{
  if (!cfg || cfg->num_blocks == 0)
    return;

  cfg_compute_rpo(cfg);

  /* Entry dominates itself */
  cfg->blocks[0].idom = 0;

  int changed = 1;
  while (changed) {
    changed = 0;
    for (int ri = 0; ri < cfg->rpo_count; ri++) {
      int b = cfg->rpo_order[ri];
      if (b == 0)
        continue;
      IRBasicBlock *bb = &cfg->blocks[b];
      int new_idom = -1;
      for (int pi = 0; pi < bb->num_preds; pi++) {
        int p = bb->preds[pi];
        if (cfg->blocks[p].idom == -1)
          continue;
        if (new_idom == -1)
          new_idom = p;
        else
          new_idom = cfg_intersect(cfg, new_idom, p);
      }
      if (new_idom >= 0 && new_idom != bb->idom) {
        bb->idom = new_idom;
        changed = 1;
      }
    }
  }
}

int tcc_ir_cfg_dominates(IRCFG *cfg, int a, int b)
{
  if (!cfg || a < 0 || b < 0 || a >= cfg->num_blocks || b >= cfg->num_blocks)
    return 0;
  while (b >= 0) {
    if (b == a)
      return 1;
    if (b == cfg->blocks[b].idom)
      return b == a;
    b = cfg->blocks[b].idom;
  }
  return 0;
}

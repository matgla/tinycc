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
  /* Avoid duplicate successor edges */
  for (int i = 0; i < fb->num_succs; i++)
    if (fb->succs[i] == to)
      goto add_pred;
  if (fb->num_succs >= fb->succs_cap) {
    int nc = fb->succs_cap ? fb->succs_cap * 2 : 4;
    fb->succs = tcc_realloc(fb->succs, nc * sizeof(int));
    fb->succs_cap = nc;
  }
  fb->succs[fb->num_succs++] = to;
add_pred:
  if (tb->num_preds >= tb->preds_cap) {
    int nc = tb->preds_cap ? tb->preds_cap * 2 : 4;
    tb->preds = tcc_realloc(tb->preds, nc * sizeof(int));
    tb->preds_cap = nc;
  }
  tb->preds[tb->num_preds++] = from;
}

int tcc_ir_cfg_flat_has_backedge(TCCIRState *ir)
{
  if (!ir)
    return 0;
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_SWITCH_LOAD)
      return 1;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      int target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (target >= 0 && target <= i)
        return 1;
    }
  }
  return 0;
}

IRCFG *tcc_ir_cfg_build(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return NULL;

  IRCFG *cfg = tcc_mallocz(sizeof(IRCFG));
  cfg->num_instrs = n;

  /* Recompute jump targets from scratch — don't trust stale flags. */
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
    /* Switch case/default targets must be leaders; otherwise SCCP folds
     * values along the wrong chain via merged blocks. */
    if (q->op == TCCIR_OP_SWITCH_TABLE) {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, src2);
      if (table_id >= 0 && table_id < ir->num_switch_tables) {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int ti = 0; ti < table->num_entries; ti++) {
          int target = table->targets[ti];
          if (target >= 0 && target < n)
            is_leader[target] = 1;
        }
        if (table->default_target >= 0 && table->default_target < n)
          is_leader[table->default_target] = 1;
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
    else if (q->op == TCCIR_OP_SWITCH_TABLE) {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, src2);
      if (table_id >= 0 && table_id < ir->num_switch_tables) {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int ti = 0; ti < table->num_entries; ti++) {
          int target = table->targets[ti];
          if (target >= 0 && target < n)
            cfg_add_edge(cfg, b, cfg->instr_to_block[target]);
        }
      }
    }
    else if (q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
             q->op == TCCIR_OP_IJUMP) {
      /* no successors */
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
  for (int i = 0; i < cfg->num_blocks; i++) {
    tcc_free(cfg->blocks[i].succs);
    tcc_free(cfg->blocks[i].preds);
    tcc_free(cfg->blocks[i].dom_frontier);
    tcc_free(cfg->blocks[i].dom_children);
  }
  tcc_free(cfg->blocks);
  tcc_free(cfg->rpo_order);
  tcc_free(cfg->instr_to_block);
  tcc_free(cfg->dom_tin);
  tcc_free(cfg->dom_tout);
  tcc_free(cfg);
}

/* Iterative DFS for reverse postorder.  Public: consumers that only need
 * rpo_order/rpo_count (e.g. a plain dataflow ordering) call this instead of
 * tcc_ir_cfg_compute_dominators, whose fixpoint is quadratic on
 * many-predecessor join blocks and was being paid just for this ordering. */
void tcc_ir_cfg_compute_rpo(IRCFG *cfg)
{
  if (!cfg)
    return;
  int nb = cfg->num_blocks;
  if (nb == 0)
    return;
  tcc_free(cfg->rpo_order);
  cfg->rpo_order = NULL;
  cfg->rpo_count = 0;

  uint8_t *visited = tcc_mallocz(nb);
  int *postorder = tcc_mallocz(nb * sizeof(int));
  int po_count = 0;

  /* Explicit-stack DFS */
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

/* Semidominator EVAL with iterative path compression (simple unbalanced
 * Lengauer-Tarjan forest).  Returns the label of minimum semi on the ancestor
 * path of v, excluding the forest root; an unlinked v returns itself.
 * `stk` is caller scratch of at least np ints (avoids recursion: the ancestor
 * chain can be as deep as the function has blocks). */
static int cfg_snca_eval(int v, int *ancestor, int *label, const int *semi, int *stk)
{
  if (ancestor[v] < 0)
    return v;
  /* collect the not-yet-compressed path v .. child-of-root */
  int sp = 0, u = v;
  while (ancestor[ancestor[u]] >= 0) {
    stk[sp++] = u;
    u = ancestor[u];
  }
  /* unwind top-down so each node merges an already-compressed ancestor */
  while (sp > 0) {
    int x = stk[--sp];
    int a = ancestor[x];
    if (semi[label[a]] < semi[label[x]])
      label[x] = label[a];
    ancestor[x] = ancestor[a];
  }
  return label[v];
}

void tcc_ir_cfg_compute_dominators(IRCFG *cfg)
{
  if (!cfg || cfg->num_blocks == 0)
    return;

  tcc_ir_cfg_compute_rpo(cfg);

  /* Entry dominates itself */
  cfg->blocks[0].idom = 0;

  /* Lengauer-Tarjan semidominators + semi-NCA construction, replacing the
   * Cooper-Harvey-Kennedy fixpoint: CHK's intersect re-walks idom chains,
   * which is quadratic on a many-predecessor join block over a deep
   * dominator chain — an 8k-pred `goto` ladder spent 25% of its whole
   * compile in the fixpoint.  The dominator tree is unique, so the result
   * is identical to CHK's; only the construction cost changes (path
   * compression makes the pred sweep near-linear).
   *
   * Every array below is indexed by DFS preorder number over the succ
   * edges; `vertex` maps preorder back to block index.  Unreachable blocks
   * are never visited and keep idom == -1, exactly as under CHK, whose
   * reachable set (rpo over the same succ edges) is the same. */
  int nb = cfg->num_blocks;
  if (nb > 1) {
    int *pre = tcc_malloc(nb * sizeof(int));    /* block -> preorder, -1 unreachable */
    int *vertex = tcc_malloc(nb * sizeof(int)); /* preorder -> block */
    int *parent = tcc_malloc(nb * sizeof(int)); /* preorder -> preorder of DFS parent */
    for (int i = 0; i < nb; i++)
      pre[i] = -1;

    typedef struct { int block; int ci; } DomFrame;
    DomFrame *stack = tcc_malloc(nb * sizeof(DomFrame));
    int sp = 0, np = 0;
    pre[0] = 0; vertex[0] = 0; parent[0] = 0; np = 1;
    stack[sp++] = (DomFrame){0, 0};
    while (sp > 0) {
      DomFrame *top = &stack[sp - 1];
      IRBasicBlock *bb = &cfg->blocks[top->block];
      if (top->ci < bb->num_succs) {
        int s = bb->succs[top->ci];
        top->ci++;
        if (s >= 0 && s < nb && pre[s] < 0) {
          pre[s] = np; vertex[np] = s; parent[np] = pre[top->block];
          np++;
          stack[sp++] = (DomFrame){s, 0};
        }
      } else {
        sp--;
      }
    }
    tcc_free(stack);

    /* Semidominators, processing vertices in decreasing preorder.  The
     * uniform eval covers both theorem cases: an unlinked (smaller-preorder)
     * pred still has semi == its own preorder, so eval returning it yields
     * exactly the pred's number. */
    int *semi = tcc_malloc(np * sizeof(int));
    int *ancestor = tcc_malloc(np * sizeof(int));
    int *label = tcc_malloc(np * sizeof(int));
    int *estk = tcc_malloc(np * sizeof(int));
    for (int i = 0; i < np; i++) {
      semi[i] = i;
      ancestor[i] = -1;
      label[i] = i;
    }
    for (int w = np - 1; w >= 1; w--) {
      IRBasicBlock *bb = &cfg->blocks[vertex[w]];
      for (int pi = 0; pi < bb->num_preds; pi++) {
        int p = bb->preds[pi];
        if (p < 0 || p >= nb || pre[p] < 0)
          continue; /* unreachable pred contributes nothing (CHK skipped it too) */
        int u = cfg_snca_eval(pre[p], ancestor, label, semi, estk);
        if (semi[u] < semi[w])
          semi[w] = semi[u];
      }
      ancestor[w] = parent[w]; /* LINK(parent, w) */
    }

    /* Semi-NCA: idom(w) = NCA(parent(w), sdom(w)) on the partially built
     * idom tree.  Preorder processing makes every consulted ancestor's idom
     * final; the ascent target strictly decreases, bounded by the root. */
    int *idom_pre = ancestor; /* reuse: linking is done */
    idom_pre[0] = 0;
    for (int w = 1; w < np; w++) {
      int a = parent[w];
      while (a > semi[w])
        a = idom_pre[a];
      idom_pre[w] = a;
      cfg->blocks[vertex[w]].idom = vertex[a];
    }

    tcc_free(pre);
    tcc_free(vertex);
    tcc_free(parent);
    tcc_free(semi);
    tcc_free(ancestor);
    tcc_free(label);
    tcc_free(estk);
  }

  /* Pre/post-order stamps over the idom tree for O(1) dominance queries. */
  {
    int nb = cfg->num_blocks;
    tcc_free(cfg->dom_tin);
    tcc_free(cfg->dom_tout);
    cfg->dom_tin = tcc_malloc(sizeof(int) * nb);
    cfg->dom_tout = tcc_malloc(sizeof(int) * nb);
    cfg->dom_dfs_count = nb;
    for (int i = 0; i < nb; i++) { cfg->dom_tin[i] = -1; cfg->dom_tout[i] = -1; }

    int *ccount = tcc_mallocz(sizeof(int) * nb);
    for (int ri = 0; ri < cfg->rpo_count; ri++) {
      int b = cfg->rpo_order[ri];
      int p = cfg->blocks[b].idom;
      if (b != 0 && p >= 0 && p != b)
        ccount[p]++;
    }
    int *cstart = tcc_malloc(sizeof(int) * (nb + 1));
    cstart[0] = 0;
    for (int i = 0; i < nb; i++) cstart[i + 1] = cstart[i] + ccount[i];
    int *chld = tcc_malloc(sizeof(int) * (cstart[nb] > 0 ? cstart[nb] : 1));
    int *nxt = ccount; /* reuse as per-parent write cursor, then DFS child cursor */
    for (int i = 0; i < nb; i++) nxt[i] = cstart[i];
    for (int ri = 0; ri < cfg->rpo_count; ri++) {
      int b = cfg->rpo_order[ri];
      int p = cfg->blocks[b].idom;
      if (b != 0 && p >= 0 && p != b)
        chld[nxt[p]++] = b;
    }
    for (int i = 0; i < nb; i++) nxt[i] = cstart[i];

    if (cfg->rpo_count > 0) {
      int *stack = tcc_malloc(sizeof(int) * (cfg->rpo_count + 1));
      int sp = 0, clock = 0;
      stack[sp++] = 0;
      cfg->dom_tin[0] = clock++;
      while (sp > 0) {
        int b = stack[sp - 1];
        if (nxt[b] < cstart[b + 1]) {
          int c = chld[nxt[b]++];
          cfg->dom_tin[c] = clock++;
          stack[sp++] = c;
        } else {
          cfg->dom_tout[b] = clock++;
          sp--;
        }
      }
      tcc_free(stack);
    }
    tcc_free(ccount);
    tcc_free(cstart);
    tcc_free(chld);
  }
}

int tcc_ir_cfg_dominates(IRCFG *cfg, int a, int b)
{
  if (!cfg || a < 0 || b < 0 || a >= cfg->num_blocks || b >= cfg->num_blocks)
    return 0;
  if (a == b)
    return 1;
  if (cfg->dom_tin && a < cfg->dom_dfs_count && b < cfg->dom_dfs_count) {
    if (cfg->dom_tin[a] < 0 || cfg->dom_tin[b] < 0)
      return 0;
    return cfg->dom_tin[a] < cfg->dom_tin[b] && cfg->dom_tout[b] < cfg->dom_tout[a];
  }
  while (b >= 0) {
    if (b == a)
      return 1;
    if (b == cfg->blocks[b].idom)
      return b == a;
    b = cfg->blocks[b].idom;
  }
  return 0;
}

static void cfg_add_df(IRBasicBlock *bb, int df_block)
{
  if (bb->num_df >= bb->df_cap) {
    int nc = bb->df_cap ? bb->df_cap * 2 : 4;
    bb->dom_frontier = tcc_realloc(bb->dom_frontier, nc * sizeof(int));
    bb->df_cap = nc;
  }
  bb->dom_frontier[bb->num_df++] = df_block;
}

static void cfg_add_dom_child(IRBasicBlock *bb, int child)
{
  if (bb->num_dom_children >= bb->dom_children_cap) {
    int nc = bb->dom_children_cap ? bb->dom_children_cap * 2 : 4;
    bb->dom_children = tcc_realloc(bb->dom_children, nc * sizeof(int));
    bb->dom_children_cap = nc;
  }
  bb->dom_children[bb->num_dom_children++] = child;
}

void tcc_ir_cfg_compute_dom_frontiers(IRCFG *cfg)
{
  if (!cfg || cfg->num_blocks == 0)
    return;

  /* Build dominator tree children */
  for (int b = 1; b < cfg->num_blocks; b++) {
    int idom = cfg->blocks[b].idom;
    if (idom >= 0 && idom != b)
      cfg_add_dom_child(&cfg->blocks[idom], b);
  }

  /* Standard dominance-frontier runner walk, memoized.  The outer loop visits
   * join blocks `b` in ascending order and each runner receives a given `b` at
   * most once, so a per-runner "last target recorded" int replaces the old
   * nb×(nb/8) bitset matrix (which was 34 MB of zeroed PSRAM on a 16k-block
   * function).  When the runner already carries `b`, the walk that recorded it
   * also climbed on to idom(b), so the whole remaining suffix is done: break.
   * That bound makes the total work O(Σ|DF|) instead of O(n²) on
   * many-predecessor joins (8k-pred `goto` ladders were 38% of the compile). */
  int nb = cfg->num_blocks;
  int *last_df = tcc_malloc(nb * sizeof(int));
  for (int i = 0; i < nb; i++)
    last_df[i] = -1;

  for (int b = 0; b < nb; b++) {
    IRBasicBlock *bb = &cfg->blocks[b];
    if (bb->num_preds < 2)
      continue;
    if (bb->idom < 0)
      continue;
    for (int pi = 0; pi < bb->num_preds; pi++) {
      int runner = bb->preds[pi];
      if (runner < 0 || cfg->blocks[runner].idom < 0)
        continue;
      int steps = 0;
      while (runner != bb->idom && steps < nb) {
        if (last_df[runner] == b)
          break;
        last_df[runner] = b;
        cfg_add_df(&cfg->blocks[runner], b);
        if (runner == cfg->blocks[runner].idom)
          break;
        runner = cfg->blocks[runner].idom;
        if (runner < 0)
          break;
        steps++;
      }
    }
  }
  tcc_free(last_df);
}

/*
 *  TCC IR - Optimization framework: dominator-tree scoped walk
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "ssa_opt.h"

/* Shared skeleton for passes that walk the dominator tree carrying per-subtree
 * scoped state (an equality-fact stack, a GVN table, ...).  The engine owns the
 * stack-overflow-safe iterative DFS and the scope lifecycle; the pass supplies
 * hooks describing how to mark/restore its scope and what to do per block.
 *
 * Rationale for the iterative worklist: native recursion here was
 * once-per-dominator-child deep and overflowed the 32 KB target process stack
 * on deeply branch-nested functions.  A POP marker (kind==1) pushed below a
 * block's children runs only after the entire subtree completes, reproducing
 * the post-recursion scope restore it replaces.
 *
 * Per block, in order:
 *   wm = mark(state)                    record the scope watermark
 *   enter(ctx, block, state)            seed edge facts for this block
 *   visit(ctx, block, state)            rewrite instructions; return #changes
 *   ... whole dominator subtree runs ...
 *   reset(state, wm)                    restore the scope to the watermark
 *
 * `enter`/`visit` return the number of IR rewrites they performed; the engine
 * sums them.  Seeding scope (a non-rewrite) must return 0.  Any hook may be
 * NULL.  The walk always roots at block 0. */
typedef struct {
  void *state;
  int  (*mark)(void *state);
  void (*reset)(void *state, int watermark);
  int  (*enter)(IRSSAOptCtx *ctx, int block, void *state);
  int  (*visit)(IRSSAOptCtx *ctx, int block, void *state);
} OptSSADomWalk;

typedef struct {
  int kind;   /* 0 = process block, 1 = restore scope to watermark */
  int value;  /* block index (kind 0) or watermark (kind 1) */
} OptSSADomWalkItem;

static inline int opt_ssa_domwalk(IRSSAOptCtx *ctx, const OptSSADomWalk *w)
{
  IRCFG *cfg = ctx->cfg;
  if (!cfg || cfg->num_blocks == 0)
    return 0;

  int changes = 0;
  int sp = 0, cap = 16;
  OptSSADomWalkItem *stack = tcc_malloc(sizeof *stack * cap);
  stack[sp].kind = 0;
  stack[sp].value = 0;
  sp++;

  while (sp > 0) {
    sp--;
    if (stack[sp].kind == 1) {
      if (w->reset)
        w->reset(w->state, stack[sp].value);
      continue;
    }

    int b = stack[sp].value;
    IRBasicBlock *bb = &cfg->blocks[b];
    int wm = w->mark ? w->mark(w->state) : 0;

    if (w->enter)
      changes += w->enter(ctx, b, w->state);
    if (w->visit)
      changes += w->visit(ctx, b, w->state);

    if (sp + 1 + bb->num_dom_children > cap) {
      while (sp + 1 + bb->num_dom_children > cap)
        cap *= 2;
      stack = tcc_realloc(stack, sizeof *stack * cap);
    }
    stack[sp].kind = 1;
    stack[sp].value = wm;
    sp++;
    for (int ci = 0; ci < bb->num_dom_children; ci++) {
      stack[sp].kind = 0;
      stack[sp].value = bb->dom_children[ci];
      sp++;
    }
  }

  tcc_free(stack);
  return changes;
}


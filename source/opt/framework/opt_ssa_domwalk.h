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

int opt_ssa_domwalk(IRSSAOptCtx *ctx, const OptSSADomWalk *w);

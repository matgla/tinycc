/*
 *  test_opt_ssa_domwalk.c - dominator-tree scoped-walk engine
 *
 *  Covers the shared framework engine opt_ssa_domwalk():
 *    - empty / NULL CFG guards
 *    - preorder traversal over dom_children (single, linear, tree)
 *    - POP-marker ordering: reset() runs after the whole subtree
 *    - scope isolation between sibling subtrees (mark/enter/reset bracket)
 *    - enter()/visit() return values summed as the change count
 *    - NULL hooks tolerated
 *    - deep chain completes without native recursion (iterative worklist)
 *
 *  The engine only reads ctx->cfg->blocks[b].dom_children, so these fixtures
 *  build dominator trees directly and use recording hooks; no IR is needed.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ssa_build.h"

#include "opt_ssa_domwalk.h"

#include "ut.h"

/* ------------------------------------------------------------------ fixtures */

static IRCFG *dw_cfg_new(int nblocks)
{
  IRCFG *cfg = tcc_mallocz(sizeof *cfg);
  cfg->num_blocks = nblocks;
  cfg->capacity = nblocks;
  cfg->blocks = tcc_mallocz(sizeof(IRBasicBlock) * nblocks);
  return cfg;
}

static void dw_set_children(IRCFG *cfg, int b, const int *ch, int n)
{
  cfg->blocks[b].dom_children = tcc_malloc(sizeof(int) * (n ? n : 1));
  for (int i = 0; i < n; i++)
    cfg->blocks[b].dom_children[i] = ch[i];
  cfg->blocks[b].num_dom_children = n;
}

static void dw_cfg_free(IRCFG *cfg)
{
  for (int b = 0; b < cfg->num_blocks; b++)
    tcc_free(cfg->blocks[b].dom_children);
  tcc_free(cfg->blocks);
  tcc_free(cfg);
}

/* ------------------------------------------------------------------ recorder */

#define DW_MAX 4096

typedef struct {
  int enter_order[DW_MAX]; int enter_n;
  int visit_order[DW_MAX]; int visit_n;
  int reset_wm[DW_MAX];    int reset_n;
  int mark_calls;
  int scope[DW_MAX];       int scope_depth;   /* scope model: pushed block ids */
  int visit_scope_top[512];                   /* top-of-scope id when block visited */
  int visit_ret[512];                         /* configured return per block */
} Rec;

static int rec_mark(void *s)
{
  Rec *r = s;
  r->mark_calls++;
  return r->scope_depth;
}

static void rec_reset(void *s, int wm)
{
  Rec *r = s;
  r->reset_wm[r->reset_n++] = wm;
  r->scope_depth = wm;
}

static int rec_enter(IRSSAOptCtx *ctx, int b, void *s)
{
  (void)ctx;
  Rec *r = s;
  r->enter_order[r->enter_n++] = b;
  r->scope[r->scope_depth++] = b;
  return 0;
}

static int rec_visit(IRSSAOptCtx *ctx, int b, void *s)
{
  (void)ctx;
  Rec *r = s;
  r->visit_order[r->visit_n++] = b;
  if (b < 512)
    r->visit_scope_top[b] = r->scope_depth > 0 ? r->scope[r->scope_depth - 1] : -1;
  return (b < 512) ? r->visit_ret[b] : 0;
}

static OptSSADomWalk dw_walk(Rec *r)
{
  OptSSADomWalk w = {0};
  w.state = r;
  w.mark = rec_mark;
  w.reset = rec_reset;
  w.enter = rec_enter;
  w.visit = rec_visit;
  return w;
}

/* ======================================================================
 * Empty and NULL CFG return 0 and touch no hooks.
 * ====================================================================== */

UT_TEST(test_domwalk_empty_cfg)
{
  IRCFG *cfg = dw_cfg_new(0);
  IRSSAOptCtx ctx = {0};
  ctx.cfg = cfg;
  Rec r = {0};
  OptSSADomWalk w = dw_walk(&r);

  UT_ASSERT_EQ(opt_ssa_domwalk(&ctx, &w), 0);
  UT_ASSERT_EQ(r.enter_n, 0);
  UT_ASSERT_EQ(r.visit_n, 0);

  dw_cfg_free(cfg);
  return 0;
}

UT_TEST(test_domwalk_null_cfg)
{
  IRSSAOptCtx ctx = {0};
  ctx.cfg = NULL;
  Rec r = {0};
  OptSSADomWalk w = dw_walk(&r);

  UT_ASSERT_EQ(opt_ssa_domwalk(&ctx, &w), 0);
  UT_ASSERT_EQ(r.enter_n, 0);
  return 0;
}

/* ======================================================================
 * Single block: enter + visit once for block 0, one mark, one reset.
 * ====================================================================== */

UT_TEST(test_domwalk_single_block)
{
  IRCFG *cfg = dw_cfg_new(1);
  IRSSAOptCtx ctx = {0};
  ctx.cfg = cfg;
  Rec r = {0};
  OptSSADomWalk w = dw_walk(&r);

  UT_ASSERT_EQ(opt_ssa_domwalk(&ctx, &w), 0);
  UT_ASSERT_EQ(r.enter_n, 1);
  UT_ASSERT_EQ(r.enter_order[0], 0);
  UT_ASSERT_EQ(r.visit_n, 1);
  UT_ASSERT_EQ(r.visit_order[0], 0);
  UT_ASSERT_EQ(r.mark_calls, 1);
  UT_ASSERT_EQ(r.reset_n, 1);

  dw_cfg_free(cfg);
  return 0;
}

/* ======================================================================
 * Linear chain 0->1->2 visits in preorder; resets nest inside-out.
 * ====================================================================== */

UT_TEST(test_domwalk_linear_chain)
{
  IRCFG *cfg = dw_cfg_new(3);
  dw_set_children(cfg, 0, (int[]){1}, 1);
  dw_set_children(cfg, 1, (int[]){2}, 1);
  dw_set_children(cfg, 2, NULL, 0);
  IRSSAOptCtx ctx = {0};
  ctx.cfg = cfg;
  Rec r = {0};
  OptSSADomWalk w = dw_walk(&r);

  opt_ssa_domwalk(&ctx, &w);

  UT_ASSERT_EQ(r.visit_n, 3);
  UT_ASSERT_EQ(r.visit_order[0], 0);
  UT_ASSERT_EQ(r.visit_order[1], 1);
  UT_ASSERT_EQ(r.visit_order[2], 2);
  /* Each block schedules one reset; deepest fires first. */
  UT_ASSERT_EQ(r.reset_n, 3);
  UT_ASSERT_EQ(r.reset_wm[0], 2);   /* leave block 2 (scope was depth 2) */
  UT_ASSERT_EQ(r.reset_wm[1], 1);   /* leave block 1 */
  UT_ASSERT_EQ(r.reset_wm[2], 0);   /* leave block 0 */

  dw_cfg_free(cfg);
  return 0;
}

/* ======================================================================
 * Sibling subtrees are scope-isolated: block 1 must not see block 2's
 * pushed scope and vice versa (POP-marker restores between siblings).
 * ====================================================================== */

UT_TEST(test_domwalk_sibling_scope_isolation)
{
  IRCFG *cfg = dw_cfg_new(3);
  dw_set_children(cfg, 0, (int[]){1, 2}, 2);
  dw_set_children(cfg, 1, NULL, 0);
  dw_set_children(cfg, 2, NULL, 0);
  IRSSAOptCtx ctx = {0};
  ctx.cfg = cfg;
  Rec r = {0};
  OptSSADomWalk w = dw_walk(&r);

  opt_ssa_domwalk(&ctx, &w);

  UT_ASSERT_EQ(r.enter_n, 3);
  UT_ASSERT_EQ(r.visit_n, 3);
  /* When each sibling is visited, the scope top is that sibling's own id,
   * never the other sibling's. */
  UT_ASSERT_EQ(r.visit_scope_top[0], 0);
  UT_ASSERT_EQ(r.visit_scope_top[1], 1);
  UT_ASSERT_EQ(r.visit_scope_top[2], 2);
  /* Every subtree closes: scope fully unwinds to 0. */
  UT_ASSERT_EQ(r.scope_depth, 0);

  dw_cfg_free(cfg);
  return 0;
}

/* ======================================================================
 * Every block is visited exactly once across a branching dom tree.
 * ====================================================================== */

UT_TEST(test_domwalk_tree_covers_all_once)
{
  /* 0 -> {1,2}; 1 -> {3,4}; 2 -> {5} */
  IRCFG *cfg = dw_cfg_new(6);
  dw_set_children(cfg, 0, (int[]){1, 2}, 2);
  dw_set_children(cfg, 1, (int[]){3, 4}, 2);
  dw_set_children(cfg, 2, (int[]){5}, 1);
  dw_set_children(cfg, 3, NULL, 0);
  dw_set_children(cfg, 4, NULL, 0);
  dw_set_children(cfg, 5, NULL, 0);
  IRSSAOptCtx ctx = {0};
  ctx.cfg = cfg;
  Rec r = {0};
  OptSSADomWalk w = dw_walk(&r);

  opt_ssa_domwalk(&ctx, &w);

  UT_ASSERT_EQ(r.visit_n, 6);
  int seen[6] = {0};
  for (int i = 0; i < r.visit_n; i++)
    seen[r.visit_order[i]]++;
  for (int b = 0; b < 6; b++)
    UT_ASSERT_EQ(seen[b], 1);
  /* Parent is always visited before its children (preorder). */
  int pos[6];
  for (int i = 0; i < r.visit_n; i++)
    pos[r.visit_order[i]] = i;
  UT_ASSERT(pos[0] < pos[1] && pos[0] < pos[2]);
  UT_ASSERT(pos[1] < pos[3] && pos[1] < pos[4]);
  UT_ASSERT(pos[2] < pos[5]);

  dw_cfg_free(cfg);
  return 0;
}

/* ======================================================================
 * enter()/visit() return values are summed into the walk's change count.
 * ====================================================================== */

UT_TEST(test_domwalk_change_count_sums_hooks)
{
  IRCFG *cfg = dw_cfg_new(3);
  dw_set_children(cfg, 0, (int[]){1, 2}, 2);
  dw_set_children(cfg, 1, NULL, 0);
  dw_set_children(cfg, 2, NULL, 0);
  IRSSAOptCtx ctx = {0};
  ctx.cfg = cfg;
  Rec r = {0};
  r.visit_ret[0] = 2;
  r.visit_ret[1] = 3;
  r.visit_ret[2] = 5;
  OptSSADomWalk w = dw_walk(&r);

  /* enter always returns 0 here, so the total is the visit sum. */
  UT_ASSERT_EQ(opt_ssa_domwalk(&ctx, &w), 10);

  dw_cfg_free(cfg);
  return 0;
}

/* ======================================================================
 * NULL hooks are tolerated (each is guarded); returns 0, no crash.
 * ====================================================================== */

UT_TEST(test_domwalk_null_hooks)
{
  IRCFG *cfg = dw_cfg_new(2);
  dw_set_children(cfg, 0, (int[]){1}, 1);
  dw_set_children(cfg, 1, NULL, 0);
  IRSSAOptCtx ctx = {0};
  ctx.cfg = cfg;
  OptSSADomWalk w = {0};   /* all hooks NULL */

  UT_ASSERT_EQ(opt_ssa_domwalk(&ctx, &w), 0);

  dw_cfg_free(cfg);
  return 0;
}

/* ======================================================================
 * A deep linear dom chain completes (iterative worklist; native recursion
 * would overflow the small target stack this engine exists to avoid).
 * ====================================================================== */

UT_TEST(test_domwalk_deep_chain_no_overflow)
{
  int n = 3000;
  IRCFG *cfg = dw_cfg_new(n);
  for (int b = 0; b < n - 1; b++)
    dw_set_children(cfg, b, (int[]){b + 1}, 1);
  dw_set_children(cfg, n - 1, NULL, 0);
  IRSSAOptCtx ctx = {0};
  ctx.cfg = cfg;
  Rec r = {0};
  /* Only mark/reset/visit-count matter here; skip per-block recording arrays
   * beyond their capacity by using count-only hooks. */
  OptSSADomWalk w = {0};
  w.state = &r;
  w.mark = rec_mark;
  w.reset = rec_reset;
  w.visit = NULL;
  w.enter = NULL;

  opt_ssa_domwalk(&ctx, &w);

  /* One mark + one reset per block; scope fully unwound. */
  UT_ASSERT_EQ(r.mark_calls, n);
  UT_ASSERT_EQ(r.reset_n, n);
  UT_ASSERT_EQ(r.scope_depth, 0);

  dw_cfg_free(cfg);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_ssa_domwalk)
{
  UT_COVERS("opt_ssa_domwalk");
  UT_RUN(test_domwalk_empty_cfg);
  UT_RUN(test_domwalk_null_cfg);
  UT_RUN(test_domwalk_single_block);
  UT_RUN(test_domwalk_linear_chain);
  UT_RUN(test_domwalk_sibling_scope_isolation);
  UT_RUN(test_domwalk_tree_covers_all_once);
  UT_RUN(test_domwalk_change_count_sums_hooks);
  UT_RUN(test_domwalk_null_hooks);
  UT_RUN(test_domwalk_deep_chain_no_overflow);
}

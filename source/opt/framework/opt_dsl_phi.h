/*
 *  TCC IR - Optimization DSL: SSA phi support
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_DSL_PHI_H
#define TCC_OPT_DSL_PHI_H

#include "ssa_opt.h"
#include "opt_dsl_entry.h"

typedef enum {
  IR_PHI_PATTERN_TRIVIAL,
} IROptPhiPattern;

typedef struct {
  IROptPhiPattern kind;
} IROptPhiPatternSpec;

typedef struct {
  int32_t replacement;
} IROptPhiRewriteSpec;

typedef int (*OptDslPhiRule)(IRSSAOptCtx *ctx, IRPhiNode *phi,
                             IROptPhiRewriteSpec *rewrite);

static inline int opt_dsl_phi_count(IRSSAOptCtx *ctx, int *operand_count)
{
  int phis = 0;
  int operands = 0;

  for (int b = 0; b < ctx->cfg->num_blocks; b++) {
    for (IRPhiNode *phi = ctx->ssa->block_phis[b]; phi; phi = phi->next) {
      phis++;
      operands += phi->num_operands;
    }
  }

  if (operand_count)
    *operand_count = operands;
  return phis;
}

static inline int opt_dsl_phi_has_use(IRSSAVregInfo *vi, int block, int slot)
{
  if (!vi)
    return 1;

  for (int i = 0; i < vi->use_count; i++) {
    IRSSAUse use = vi->uses[i];
    if (use.kind == SSA_USE_PHI && use.idx == block && use.slot == slot)
      return 1;
  }
  return 0;
}

static inline void opt_dsl_phi_remove_use(IRSSAVregInfo *vi, int block,
                                           int slot)
{
  if (!vi)
    return;

  for (int i = 0; i < vi->use_count; i++) {
    IRSSAUse use = vi->uses[i];
    if (use.kind == SSA_USE_PHI && use.idx == block && use.slot == slot) {
      vi->uses[i] = vi->uses[--vi->use_count];
      return;
    }
  }
}

static inline int opt_dsl_phi_metadata_valid(IRSSAOptCtx *ctx, int block,
                                              IRPhiNode *phi)
{
  for (int i = 0; i < phi->num_operands; i++) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->operands[i].vreg);
    if (!opt_dsl_phi_has_use(vi, block, i))
      return 0;
  }
  return 1;
}

static inline int opt_dsl_phi_remove(IRSSAOptCtx *ctx, int block,
                                     IRPhiNode **link)
{
  IRPhiNode *phi = link ? *link : NULL;
  if (!phi)
    return 0;

  if (!opt_dsl_phi_metadata_valid(ctx, block, phi))
    return 0;

  for (int i = 0; i < phi->num_operands; i++) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->operands[i].vreg);
    opt_dsl_phi_remove_use(vi, block, i);
  }

  IRSSAVregInfo *dvi = ssa_opt_vinfo(ctx, phi->dest_vreg);
  if (dvi && dvi->def_phi_block == block)
    dvi->def_phi_block = -1;

  *link = phi->next;
  tcc_free(phi->operands);
  tcc_free(phi);
  return 1;
}

static inline int opt_dsl_phi_match(IRPhiNode *phi, IROptPhiPatternSpec spec,
                                    int32_t *replacement)
{
  int32_t unique = -1;

  if (spec.kind != IR_PHI_PATTERN_TRIVIAL)
    return 0;

  for (int i = 0; i < phi->num_operands; i++) {
    int32_t v = phi->operands[i].vreg;
    if (v < 0 || v == phi->dest_vreg)
      continue;
    if (unique < 0)
      unique = v;
    else if (v != unique)
      return 0;
  }

  if (unique < 0)
    return 0;
  *replacement = unique;
  return 1;
}

static inline int opt_dsl_run_phi_rules(IRSSAOptCtx *ctx,
                                        const OptDslPhiRule *rules, int count)
{
  IRSSAState *ssa = ctx->ssa;
  IRCFG *cfg = ctx->cfg;
  int changes = 0;
  int progress;

  do {
    progress = 0;
    for (int b = 0; b < cfg->num_blocks; b++) {
      IRPhiNode **pp = &ssa->block_phis[b];
      while (*pp) {
        IRPhiNode *phi = *pp;
        IROptPhiRewriteSpec rewrite;
        int matched = 0;

        for (int r = 0; r < count; r++) {
          if (rules[r](ctx, phi, &rewrite)) {
            matched = 1;
            break;
          }
        }
        if (!matched) {
          pp = &phi->next;
          continue;
        }

        if (!opt_dsl_phi_metadata_valid(ctx, b, phi)) {
          pp = &phi->next;
          continue;
        }

        ssa_opt_replace_all_uses(ctx, phi->dest_vreg, rewrite.replacement);
        IRSSAVregInfo *dvi = ssa_opt_vinfo(ctx, phi->dest_vreg);
        if (dvi && dvi->use_count > 0) {
          pp = &phi->next;
          continue;
        }

        if (!opt_dsl_phi_remove(ctx, b, pp)) {
          pp = &phi->next;
          continue;
        }
        progress++;
        changes++;
      }
    }
  } while (progress > 0);

  return changes;
}

#define OPT_GEN_PHI(name) \
  static int opt_dsl_dispatch_##name(IRSSAOptCtx *ctx, IRPhiNode *phi, \
                                      IROptPhiRewriteSpec *rewrite); \
  static int opt_dsl_dispatch_##name(IRSSAOptCtx *ctx, IRPhiNode *phi, \
                                      IROptPhiRewriteSpec *rewrite)

#define PATTERN_PHI(...) \
  int32_t replacement_vreg = -1; \
  do { \
    const IROptPhiPatternSpec _opt_dsl_phi_pat = { __VA_ARGS__ }; \
    if (!opt_dsl_phi_match(phi, _opt_dsl_phi_pat, &replacement_vreg)) \
      return 0; \
  } while (0); \
  (void)ctx

#define REWRITE_PHI(...) \
  do { \
    const IROptPhiRewriteSpec _opt_dsl_phi_rw = { __VA_ARGS__ }; \
    *rewrite = _opt_dsl_phi_rw; \
    return 1; \
  } while (0)

#define OPT_PHI_ENTRY(name) opt_dsl_dispatch_##name

#endif /* TCC_OPT_DSL_PHI_H */

/*
 *  TCC IR - SSA DCE: dead phi cycle elimination
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "opt_dsl_phi.h"
#include "dce_passes.h"


TCC_DBG_ENV_FLAG(dbg_phi_cycles, "TCC_DBG_PHI_CYCLES")

static int ssa_dce_block_in_backedge_region(IRCFG *cfg, int block)
{
  if (!cfg || block < 0 || block >= cfg->num_blocks)
    return 0;

  IRBasicBlock *bb = &cfg->blocks[block];
  for (int h = 0; h < cfg->num_blocks; h++) {
    IRBasicBlock *header = &cfg->blocks[h];
    for (int i = 0; i < header->num_preds; i++) {
      int pred = header->preds[i];
      if (pred < 0 || pred >= cfg->num_blocks)
        continue;
      IRBasicBlock *latch = &cfg->blocks[pred];
      if (pred != h && latch->start_idx < header->start_idx)
        continue;
      if (bb->start_idx >= header->start_idx &&
          bb->start_idx <= latch->start_idx)
        return 1;
    }
  }
  return 0;
}

/* Direct IR scan, not use-def chains: those under-report after passes that NOP without unlinking uses. */
static int ssa_dce_vreg_unread(IRSSAOptCtx *ctx, const IRPhiNode *self,
                               int32_t v, int reject_redef)
{
  TCCIRState *ir = ctx->ir;

  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[q->op].has_src1 &&
        irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == v)
      return 0;
    if (irop_config[q->op].has_src2 &&
        irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == v)
      return 0;
    if (q->op == TCCIR_OP_MLA &&
        irop_get_vreg(tcc_ir_op_get_accum(ir, q)) == v)
      return 0;
    if (irop_config[q->op].has_dest &&
        irop_get_vreg(tcc_ir_op_get_dest(ir, q)) == v) {
      if (reject_redef || q->op == TCCIR_OP_STORE ||
          q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
        return 0;
    }
  }

  for (int b = 0; b < ctx->cfg->num_blocks; b++) {
    for (IRPhiNode *phi = ctx->ssa->block_phis[b]; phi; phi = phi->next) {
      if (phi == self)
        continue;
      for (int pi = 0; pi < phi->num_operands; pi++)
        if (phi->operands[pi].vreg == v)
          return 0;
    }
  }
  return 1;
}

static int ssa_dce_phi_dest_unread(IRSSAOptCtx *ctx, const IRPhiNode *self)
{
  return ssa_dce_vreg_unread(ctx, self, self->dest_vreg, 1);
}

/* A backedge phi glues its operands' live ranges across iterations: removing it while an operand is still read costs the VAR its register promotion (pr95731). */
static int ssa_dce_phi_operands_unread(IRSSAOptCtx *ctx, const IRPhiNode *self)
{
  for (int oi = 0; oi < self->num_operands; oi++) {
    int32_t ov = self->operands[oi].vreg;
    if (ov < 0 || ov == self->dest_vreg)
      continue;
    int dup = 0;
    for (int oj = 0; oj < oi; oj++)
      if (self->operands[oj].vreg == ov)
        dup = 1;
    if (dup)
      continue;
    if (!ssa_dce_vreg_unread(ctx, self, ov, 0))
      return 0;
  }
  return 1;
}

int dce_dead_phi_cycles(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  IRSSAState *ssa = ctx->ssa;
  int cap = ctx->vinfo_cap;

  if (!ssa || !ssa->block_phis || !cfg || cap <= 0)
    return 0;

  int has_phi = 0;
  for (int b = 0; b < cfg->num_blocks && !has_phi; b++)
    if (ssa->block_phis[b])
      has_phi = 1;
  if (!has_phi)
    return 0;

  int bm_size = (cap + 7) / 8;
  uint8_t *live = tcc_mallocz(bm_size);

#define BM_SET(pos)  (live[(pos) / 8] |= (1u << ((pos) % 8)))
#define BM_TEST(pos) (live[(pos) / 8] &  (1u << ((pos) % 8)))

#define MARK_TEMP_LIVE(vr) do { \
    int32_t _v = (vr); \
    if (_v >= 0 && TCCIR_DECODE_VREG_TYPE(_v) == TCCIR_VREG_TYPE_TEMP) { \
      int _p = TCCIR_DECODE_VREG_POSITION(_v); \
      if (_p < cap && !BM_TEST(_p)) { BM_SET(_p); } \
    } \
  } while (0)

  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_ASSIGN) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dv = irop_get_vreg(d);
      if (dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP) {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        MARK_TEMP_LIVE(irop_get_vreg(s));
      }
      continue;
    }
    if (irop_config[q->op].has_src1)
      MARK_TEMP_LIVE(irop_get_vreg(tcc_ir_op_get_src1(ir, q)));
    if (irop_config[q->op].has_src2)
      MARK_TEMP_LIVE(irop_get_vreg(tcc_ir_op_get_src2(ir, q)));
    if (q->op == TCCIR_OP_MLA)
      MARK_TEMP_LIVE(irop_get_vreg(tcc_ir_op_get_accum(ir, q)));
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC)
      MARK_TEMP_LIVE(irop_get_vreg(tcc_ir_op_get_dest(ir, q)));
  }

  int changed = 1;
  while (changed) {
    changed = 0;
    for (int i = 0; i < ir->next_instruction_index; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ASSIGN)
        continue;
      int32_t dv = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      if (dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int dp = TCCIR_DECODE_VREG_POSITION(dv);
      if (dp >= cap || !BM_TEST(dp))
        continue;
      int32_t sv = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (sv < 0 || TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int sp = TCCIR_DECODE_VREG_POSITION(sv);
      if (sp >= cap || BM_TEST(sp))
        continue;
      BM_SET(sp);
      changed = 1;
    }
    for (int b = 0; b < cfg->num_blocks; b++) {
      for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next) {
        int32_t dv = phi->dest_vreg;
        if (dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP)
          continue;
        int dp = TCCIR_DECODE_VREG_POSITION(dv);
        if (dp >= cap || !BM_TEST(dp))
          continue;
        for (int pi = 0; pi < phi->num_operands; pi++) {
          int32_t ov = phi->operands[pi].vreg;
          if (ov < 0 || TCCIR_DECODE_VREG_TYPE(ov) != TCCIR_VREG_TYPE_TEMP)
            continue;
          int op = TCCIR_DECODE_VREG_POSITION(ov);
          if (op >= cap || BM_TEST(op))
            continue;
          BM_SET(op);
          changed = 1;
        }
      }
    }
  }

  int changes = 0;
  int round_removed;
  do {
    round_removed = 0;
    for (int b = 0; b < cfg->num_blocks; b++) {
      int in_backedge_region = ssa_dce_block_in_backedge_region(cfg, b);
      IRPhiNode **pp = &ssa->block_phis[b];
      while (*pp) {
        IRPhiNode *phi = *pp;
        int32_t dv = phi->dest_vreg;
        if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP) {
          int dp = TCCIR_DECODE_VREG_POSITION(dv);
          if (dp < cap && !BM_TEST(dp)) {
            /* Graph liveness can call a back-edge phi dead while a use remains, so demand direct-scan proof (seed 18960, pr49049). */
            if (in_backedge_region && (!ssa_dce_phi_dest_unread(ctx, phi) ||
                                       !ssa_dce_phi_operands_unread(ctx, phi))) {
              pp = &phi->next;
              continue;
            }
            TCC_DBG_BLOCK(dbg_phi_cycles) {
              fprintf(stderr, "[phi_cycles] remove phi block=%d dest=T%d ops:", b, dp);
              for (int pi = 0; pi < phi->num_operands; pi++)
                fprintf(stderr, " %d", phi->operands[pi].vreg);
              fprintf(stderr, "\n");
            }
            if (!opt_dsl_phi_remove(ctx, b, pp)) {
              pp = &phi->next;
              continue;
            }
            round_removed++;
            continue;
          }
        }
        pp = &phi->next;
      }
    }
    changes += round_removed;
  } while (round_removed);

  if (changes) {
    for (int p = 0; p < ctx->vinfo_cap; p++)
      ctx->vinfo[p].use_count = 0;
    for (int i = 0; i < ir->next_instruction_index; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      ssa_opt_scan_instr_uses(ctx, i, q);
    }
    for (int b = 0; b < cfg->num_blocks; b++) {
      for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next) {
        for (int pi = 0; pi < phi->num_operands; pi++) {
          IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->operands[pi].vreg);
          if (vi)
            ssa_opt_add_use_phi(vi, b, pi);
        }
      }
    }
    changes += dce_temp_worklist(ctx);
  }

#undef BM_SET
#undef BM_TEST
#undef MARK_TEMP_LIVE

  tcc_free(live);
  return changes;
}

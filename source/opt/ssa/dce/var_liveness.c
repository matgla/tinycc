/*
 *  TCC IR - SSA DCE: local VAR slot liveness
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
#include "dce_passes.h"


#define VL_BIT(bm, p) ((bm)[(p) >> 5] & (1u << ((p) & 31)))
#define VL_SET(bm, p) ((bm)[(p) >> 5] |= (1u << ((p) & 31)))
#define VL_CLR(bm, p) ((bm)[(p) >> 5] &= ~(1u << ((p) & 31)))

static int vl_var_pos(IROperand op, int num_vars)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
    return -1;
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  return pos < num_vars ? pos : -1;
}

/* True only for a write to the VAR's own slot, not through a pointer held in it. */
static int vl_store_is_slot_def(IROperand d)
{
  if (d.is_sym)
    return 0;
  return !d.is_lval || (d.is_local && !d.is_llocal);
}

/* Every def returned must be safe to NOP when dead: side-effect ops never kill, except the STORE slot-def form. */
static int vl_def_pos(TCCIRState *ir, IRQuadCompact *q, int num_vars)
{
  if (!irop_config[q->op].has_dest)
    return -1;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (ssa_opt_has_side_effects(q->op)) {
    if (q->op != TCCIR_OP_STORE || !vl_store_is_slot_def(d))
      return -1;
  } else {
    if (d.is_sym || (d.is_lval && (!d.is_local || d.is_llocal)))
      return -1;
  }
  int p = vl_var_pos(d, num_vars);
  if (p < 0)
    return -1;
  int bt = irop_get_btype(d);
  if (bt == IROP_BTYPE_STRUCT)
    return -1;
  IRLiveInterval *iv =
      tcc_ir_get_live_interval(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, p));
  if (iv && (iv->is_llong || iv->is_double) &&
      bt != IROP_BTYPE_INT64 && bt != IROP_BTYPE_FLOAT64)
    return -1;
  return p;
}

static void vl_mark_uses(TCCIRState *ir, IRQuadCompact *q, int num_vars,
                         uint32_t *live)
{
  int p;
  if (irop_config[q->op].has_src1 &&
      (p = vl_var_pos(tcc_ir_op_get_src1(ir, q), num_vars)) >= 0)
    VL_SET(live, p);
  if (irop_config[q->op].has_src2 &&
      (p = vl_var_pos(tcc_ir_op_get_src2(ir, q), num_vars)) >= 0)
    VL_SET(live, p);
  if (q->op == TCCIR_OP_MLA &&
      (p = vl_var_pos(tcc_ir_op_get_accum(ir, q), num_vars)) >= 0)
    VL_SET(live, p);
  if (irop_config[q->op].has_dest &&
      vl_def_pos(ir, q, num_vars) < 0 &&
      (p = vl_var_pos(tcc_ir_op_get_dest(ir, q), num_vars)) >= 0)
    VL_SET(live, p);
}

static int vl_removable(TCCIRState *ir, IRQuadCompact *q)
{
  for (int side = 0; side < 2; side++) {
    IROperand s;
    if (side == 0 && irop_config[q->op].has_src1)
      s = tcc_ir_op_get_src1(ir, q);
    else if (side == 1 && irop_config[q->op].has_src2)
      s = tcc_ir_op_get_src2(ir, q);
    else
      continue;
    int32_t vr = irop_get_vreg(s);
    if (vr >= 0) {
      IRLiveInterval *iv = tcc_ir_get_live_interval(ir, vr);
      if (iv && iv->is_volatile)
        return 0;
    }
  }
  return 1;
}

int dce_var_liveness(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  int num_vars = ir->next_local_variable;
  int changes = 0;

  if (num_vars <= 0 || n == 0 || ir->has_static_chain)
    return 0;

  for (int i = 0; i < n; i++) {
    switch (ir->compact_instructions[i].op) {
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_INIT_CHAIN_SLOT:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
      return 0;
    default:
      break;
    }
  }

  /* ctx->cfg may be stale after branch rewrites — build a fresh one */
  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg || cfg->num_blocks == 0) {
    tcc_ir_cfg_free(cfg);
    return 0;
  }

  int nb = cfg->num_blocks;
  int nw = (num_vars + 31) / 32;
  size_t bmsz = (size_t)nw * sizeof(uint32_t);
  uint32_t *excl = tcc_mallocz(bmsz);
  uint32_t *use = tcc_mallocz((size_t)nb * bmsz);
  uint32_t *def = tcc_mallocz((size_t)nb * bmsz);
  uint32_t *lin = tcc_mallocz((size_t)nb * bmsz);
  uint32_t *live = tcc_mallocz(bmsz);

  /* sticky iv->addrtaken deliberately NOT consulted: escapes are re-derived below */
  for (int p = 0; p < num_vars; p++) {
    IRLiveInterval *iv =
        tcc_ir_get_live_interval(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, p));
    if (!iv || iv->is_volatile)
      VL_SET(excl, p);
  }
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int side = 0; side < 3; side++) {
      IROperand s;
      if (side == 0 && irop_config[q->op].has_src1)
        s = tcc_ir_op_get_src1(ir, q);
      else if (side == 1 && irop_config[q->op].has_src2)
        s = tcc_ir_op_get_src2(ir, q);
      else if (side == 2 && q->op == TCCIR_OP_MLA)
        s = tcc_ir_op_get_accum(ir, q);
      else
        continue;
      int p = vl_var_pos(s, num_vars);
      if (p < 0)
        continue;
      /* Addr[V] escapes; any VAR feeding a LEA is address-taken */
      if ((s.is_local && !s.is_lval) || q->op == TCCIR_OP_LEA)
        VL_SET(excl, p);
    }
  }

  for (int b = 0; b < nb; b++) {
    uint32_t *ub = use + (size_t)b * nw;
    uint32_t *db = def + (size_t)b * nw;
    for (int i = cfg->blocks[b].start_idx; i < cfg->blocks[b].end_idx; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      memset(live, 0, bmsz);
      vl_mark_uses(ir, q, num_vars, live);
      for (int w = 0; w < nw; w++)
        ub[w] |= live[w] & ~db[w];
      int dp = vl_def_pos(ir, q, num_vars);
      if (dp >= 0 && !VL_BIT(excl, dp))
        VL_SET(db, dp);
    }
  }

  int unstable = 1, rounds = 0;
  while (unstable && rounds++ <= nb + 2) {
    unstable = 0;
    for (int b = nb - 1; b >= 0; b--) {
      memset(live, 0, bmsz);
      for (int s = 0; s < cfg->blocks[b].num_succs; s++) {
        uint32_t *sin = lin + (size_t)cfg->blocks[b].succs[s] * nw;
        for (int w = 0; w < nw; w++)
          live[w] |= sin[w];
      }
      uint32_t *ub = use + (size_t)b * nw;
      uint32_t *db = def + (size_t)b * nw;
      uint32_t *ib = lin + (size_t)b * nw;
      for (int w = 0; w < nw; w++) {
        uint32_t v = ub[w] | (live[w] & ~db[w]);
        if (v != ib[w]) {
          ib[w] = v;
          unstable = 1;
        }
      }
    }
  }

  if (!unstable) {
    for (int b = 0; b < nb; b++) {
      memset(live, 0, bmsz);
      for (int s = 0; s < cfg->blocks[b].num_succs; s++) {
        uint32_t *sin = lin + (size_t)cfg->blocks[b].succs[s] * nw;
        for (int w = 0; w < nw; w++)
          live[w] |= sin[w];
      }
      for (int i = cfg->blocks[b].end_idx - 1; i >= cfg->blocks[b].start_idx; i--) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        int dp = vl_def_pos(ir, q, num_vars);
        if (dp >= 0 && !VL_BIT(excl, dp)) {
          if (!VL_BIT(live, dp) && vl_removable(ir, q)) {
            ssa_opt_nop_instr(ctx, i);
            changes++;
            continue;
          }
          VL_CLR(live, dp);
        }
        vl_mark_uses(ir, q, num_vars, live);
      }
    }
  }

  tcc_free(excl);
  tcc_free(use);
  tcc_free(def);
  tcc_free(lin);
  tcc_free(live);
  tcc_ir_cfg_free(cfg);
  return changes;
}

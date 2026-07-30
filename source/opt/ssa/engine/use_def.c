/*
 *  TCC IR - SSA use-def chain construction
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

IRSSAVregInfo *ssa_opt_vinfo(IRSSAOptCtx *ctx, int32_t vreg)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_TEMP)
    return NULL;
  int pos = TCCIR_DECODE_VREG_POSITION(vreg);
  if (pos >= ctx->vinfo_cap)
    return NULL;
  return &ctx->vinfo[pos];
}

void ssa_opt_add_use_instr(IRSSAVregInfo *vi, int instr_idx)
{
  if (vi->use_count >= vi->use_cap) {
    int nc = vi->use_cap ? vi->use_cap * 2 : 4;
    vi->uses = tcc_realloc(vi->uses, nc * sizeof(IRSSAUse));
    vi->use_cap = nc;
  }
  vi->uses[vi->use_count++] = (IRSSAUse){ .idx = instr_idx, .kind = SSA_USE_INSTR };
}

void ssa_opt_add_use_phi(IRSSAVregInfo *vi, int block, int slot)
{
  if (vi->use_count >= vi->use_cap) {
    int nc = vi->use_cap ? vi->use_cap * 2 : 4;
    vi->uses = tcc_realloc(vi->uses, nc * sizeof(IRSSAUse));
    vi->use_cap = nc;
  }
  vi->uses[vi->use_count++] = (IRSSAUse){ .idx = block, .slot = slot, .kind = SSA_USE_PHI };
}

void ssa_opt_remove_use_instr(IRSSAVregInfo *vi, int instr_idx)
{
  for (int i = 0; i < vi->use_count; i++) {
    if (vi->uses[i].kind == SSA_USE_INSTR && vi->uses[i].idx == instr_idx) {
      vi->uses[i] = vi->uses[--vi->use_count];
      return;
    }
  }
}

static void ssa_opt_record_use(IRSSAOptCtx *ctx, int32_t vreg, int instr_idx)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vreg);
  if (vi)
    ssa_opt_add_use_instr(vi, instr_idx);
}

void ssa_opt_scan_instr_uses(IRSSAOptCtx *ctx, int i, IRQuadCompact *q)
{
  TCCIRState *ir = ctx->ir;

  if (irop_config[q->op].has_src1) {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    ssa_opt_record_use(ctx, irop_get_vreg(s), i);
  }
  if (irop_config[q->op].has_src2) {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    ssa_opt_record_use(ctx, irop_get_vreg(s), i);
  }
  if (q->op == TCCIR_OP_MLA) {
    IROperand a = tcc_ir_op_get_accum(ir, q);
    ssa_opt_record_use(ctx, irop_get_vreg(a), i);
  }
  if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
      q->op == TCCIR_OP_STORE_POSTINC) {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    /* STORE with a non-lval dest is a value def, not a use of the dest. */
    int dest_is_use = 1;
    if (q->op == TCCIR_OP_STORE && !d.is_lval)
      dest_is_use = 0;
    if (dest_is_use)
      ssa_opt_record_use(ctx, irop_get_vreg(d), i);
  }
}

static int ssa_opt_is_def_op(int op)
{
  if (!irop_config[op].has_dest)
    return 0;
  if (op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_STORE_POSTINC ||
      op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_FUNCPARAMVOID)
    return 0;
  /* For STORE the caller must additionally check dest.is_lval == 0. */
  return 1;
}

static int ssa_opt_quad_defines_value(const TCCIRState *ir, const IRQuadCompact *q)
{
  if (!ssa_opt_is_def_op(q->op))
    return 0;
  if (q->op == TCCIR_OP_STORE) {
    IROperand d = tcc_ir_op_get_dest((TCCIRState *)ir, (IRQuadCompact *)q);
    if (d.is_lval)
      return 0;
  }
  return 1;
}

static void ssa_opt_build_chains(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  IRSSAState *ssa = ctx->ssa;

  for (int i = 0; i < ctx->vinfo_cap; i++) {
    ctx->vinfo[i].def_instr = -1;
    ctx->vinfo[i].def_phi_block = -1;
    ctx->vinfo[i].def_count = 0;
    ctx->vinfo[i].use_count = 0;
  }

  for (int b = 0; b < cfg->num_blocks; b++) {
    for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next) {
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->dest_vreg);
      if (vi)
        vi->def_phi_block = b;
    }
  }

  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (ssa_opt_quad_defines_value(ir, q)) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(d));
      if (vi) {
        vi->def_instr = i;
        vi->def_count++;
      }
    }

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
}

void tcc_ir_ssa_opt_init(IRSSAOptCtx *ctx, TCCIRState *ir,
                         IRSSAState *ssa, IRCFG *cfg)
{
  memset(ctx, 0, sizeof(*ctx));
  ctx->ir = ir;
  ctx->ssa = ssa;
  ctx->cfg = cfg;
  ctx->vinfo_cap = ir->next_temporary_variable;
  if (ctx->vinfo_cap <= 0)
    ctx->vinfo_cap = 1;
  ctx->vinfo = tcc_mallocz(ctx->vinfo_cap * sizeof(IRSSAVregInfo));
  ssa_opt_build_chains(ctx);
}

void tcc_ir_ssa_opt_rebuild(IRSSAOptCtx *ctx)
{
  for (int i = 0; i < ctx->vinfo_cap; i++) {
    tcc_free(ctx->vinfo[i].uses);
    ctx->vinfo[i].uses = NULL;
    ctx->vinfo[i].use_count = 0;
    ctx->vinfo[i].use_cap = 0;
  }

  int new_cap = ctx->ir->next_temporary_variable;
  if (new_cap > ctx->vinfo_cap) {
    ctx->vinfo = tcc_realloc(ctx->vinfo, new_cap * sizeof(IRSSAVregInfo));
    memset(&ctx->vinfo[ctx->vinfo_cap], 0,
           (new_cap - ctx->vinfo_cap) * sizeof(IRSSAVregInfo));
    ctx->vinfo_cap = new_cap;
  }

  ssa_opt_build_chains(ctx);
}

void tcc_ir_ssa_opt_free(IRSSAOptCtx *ctx)
{
  if (!ctx->vinfo)
    return;
  for (int i = 0; i < ctx->vinfo_cap; i++)
    tcc_free(ctx->vinfo[i].uses);
  tcc_free(ctx->vinfo);
  ctx->vinfo = NULL;
}


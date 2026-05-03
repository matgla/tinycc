/*
 *  TCC IR - SSA Copy Propagation
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

/* ============================================================================
 * Generator: ssa_gen_cprop_assign
 *
 * Pattern: ASSIGN dest = src  (both TEMP vregs, no lval/deref)
 * Action:  replace all uses of dest with src, making the ASSIGN dead
 * ============================================================================ */

static int ssa_gen_cprop_assign(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];

  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand src = tcc_ir_op_get_src1(ir, q);

  if (src.is_lval || src.is_llocal || src.is_local)
    return 0;

  int32_t dest_vr = irop_get_vreg(dest);
  int32_t src_vr = irop_get_vreg(src);
  if (dest_vr < 0 || src_vr < 0)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  if (src.tag != IROP_TAG_VREG)
    return 0;

  /* Only propagate TEMP sources. PARAM and VAR vregs are not SSA-renamed,
   * so they may have multiple definitions; replacing uses of dest with a
   * non-versioned source is unsafe when the source is redefined between
   * the copy and a use (e.g. pointer increment in a loop body). */
  if (TCCIR_DECODE_VREG_TYPE(src_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, dest_vr);
  if (vi && vi->def_count > 1)
    return 0;

  int replaced = ssa_opt_replace_all_uses(ctx, dest_vr, src_vr);
  return replaced > 0 ? 1 : 0;
}

/* ============================================================================
 * Generator: ssa_gen_cprop_imm
 *
 * Pattern: ASSIGN dest = #imm32  (TEMP dest, immediate src)
 * Action:  replace all uses of dest with the immediate directly
 * ============================================================================ */

static int ssa_gen_cprop_imm(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];

  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand src = tcc_ir_op_get_src1(ir, q);

  int32_t dest_vr = irop_get_vreg(dest);
  if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  if (src.tag != IROP_TAG_IMM32 && src.tag != IROP_TAG_F32)
    return 0;
  if (src.is_lval)
    return 0;

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, dest_vr);
  if (!vi || vi->use_count == 0)
    return 0;
  if (vi->def_count > 1)
    return 0;

  int count = 0;
  while (vi->use_count > 0) {
    IRSSAUse use = vi->uses[vi->use_count - 1];
    if (use.kind != SSA_USE_INSTR) {
      /* phi uses keep the vreg */
      break;
    }

    IRQuadCompact *uq = &ir->compact_instructions[use.idx];

    int rewrote = 0;
    if (irop_config[uq->op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, uq);
      if (irop_get_vreg(s) == dest_vr && !s.is_lval) {
        tcc_ir_op_set_src1(ir, uq, src);
        rewrote = 1;
      }
    }
    if (!rewrote && irop_config[uq->op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, uq);
      if (irop_get_vreg(s) == dest_vr && !s.is_lval) {
        tcc_ir_op_set_src2(ir, uq, src);
        rewrote = 1;
      }
    }

    if (rewrote) {
      vi->use_count--;
      count++;
    } else {
      break;
    }
  }

  return count > 0 ? 1 : 0;
}

/* ============================================================================
 * Generator Table
 * ============================================================================ */

static const IRSSAOptGen cprop_gens[] = {
  { TCCIR_OP_ASSIGN, ssa_gen_cprop_assign, "cprop_assign" },
  { TCCIR_OP_ASSIGN, ssa_gen_cprop_imm,    "cprop_imm" },
};

/* ============================================================================
 * Pass Entry Point
 * ============================================================================ */

int ssa_opt_cprop(IRSSAOptCtx *ctx)
{
  return ssa_opt_run_gens(ctx, cprop_gens, sizeof(cprop_gens) / sizeof(cprop_gens[0]));
}

/* ============================================================================
 * VAR Forwarding: propagate single-def VAR values into their uses.
 *
 * Pattern:  Vn <-- Tx [STORE]   (single def within block)
 *           Ty <-- Vn [ASSIGN]  (use in same block, after def)
 * Action:   Ty <-- Tx [ASSIGN]
 *
 * VARs from inline expansion are typically single-def and used within
 * the same block.  Without SSA promotion, the optimizer can't see through
 * them; this pass makes their values visible to load_cse and SCCP.
 * ============================================================================ */

int ssa_opt_var_forward(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  if (!cfg || cfg->num_blocks == 0)
    return 0;

  int num_vars = ir->next_local_variable;
  if (num_vars <= 0)
    return 0;

  /* Count defs per VAR across entire function */
  int *var_def_count = tcc_mallocz(num_vars * sizeof(int));
  int *var_def_instr = tcc_mallocz(num_vars * sizeof(int));
  uint8_t *var_addrtaken = tcc_mallocz((num_vars + 7) / 8);

  for (int i = 0; i < num_vars; i++)
    var_def_instr[i] = -1;

  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Mark address-taken VARs — check all instructions for src operands
     * that reference a VAR with is_local && !is_lval (address-of-local). */
    {
      int nops = irop_config[q->op].has_src1 + irop_config[q->op].has_src2;
      for (int oi = 0; oi < nops; oi++) {
        IROperand s = oi == 0 ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
        if (s.is_local && !s.is_lval) {
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos < num_vars)
              var_addrtaken[pos / 8] |= (1 << (pos % 8));
          }
        }
      }
      if (q->op == TCCIR_OP_LEA) {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        int32_t vr = irop_get_vreg(s);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos < num_vars)
            var_addrtaken[pos / 8] |= (1 << (pos % 8));
        }
      }
    }

    /* Count ALL defs to VARs (any instruction with a VAR as dest) */
    if (irop_config[q->op].has_dest &&
        q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (pos < num_vars) {
          var_def_count[pos]++;
          var_def_instr[pos] = i;
        }
      }
    }
  }

  int changes = 0;

  /* For each single-def, non-address-taken VAR: replace uses with stored value */
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
      continue;

    IROperand src = tcc_ir_op_get_src1(ir, q);
    int32_t src_vr = irop_get_vreg(src);
    if (src_vr < 0 || TCCIR_DECODE_VREG_TYPE(src_vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    int var_pos = TCCIR_DECODE_VREG_POSITION(src_vr);
    if (var_pos >= num_vars) { continue; }
    if (var_def_count[var_pos] != 1)
      continue;
    if (var_addrtaken[var_pos / 8] & (1 << (var_pos % 8)))
      continue;

    int def_idx = var_def_instr[var_pos];
    if (def_idx < 0 || def_idx >= i)
      continue;

    /* Def must dominate use. Same-block is always safe; cross-block
     * requires the def's block to dominate the use's block. */
    {
      int def_blk = cfg->instr_to_block[def_idx];
      int use_blk = cfg->instr_to_block[i];
      if (def_blk != use_blk) {
        int dominated = 0;
        IRBasicBlock *ub = &cfg->blocks[use_blk];
        int d = ub->idom;
        while (d >= 0) {
          if (d == def_blk) { dominated = 1; break; }
          if (d == cfg->blocks[d].idom) break;
          d = cfg->blocks[d].idom;
        }
        if (!dominated)
          continue;
      }
    }

    /* A function call between def and use may modify the VAR through
     * a closure chain (nested functions).  Skip forwarding in that case. */
    {
      int has_call = 0;
      for (int k = def_idx + 1; k < i && !has_call; k++) {
        int op = ir->compact_instructions[k].op;
        if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL)
          has_call = 1;
      }
      if (has_call)
        continue;
    }

    IRQuadCompact *def_q = &ir->compact_instructions[def_idx];
    if (def_q->op != TCCIR_OP_STORE && def_q->op != TCCIR_OP_ASSIGN)
      continue;

    IROperand stored_val = tcc_ir_op_get_src1(ir, def_q);
    if (stored_val.is_lval)
      continue;
    int32_t stored_vr = irop_get_vreg(stored_val);
    if (stored_vr >= 0 && TCCIR_DECODE_VREG_TYPE(stored_vr) == TCCIR_VREG_TYPE_VAR)
      continue;

    /* Replace the source with the stored value */
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, stored_val);
    tcc_ir_set_src2(ir, i, IROP_NONE);

    if (stored_vr >= 0) {
      IRSSAVregInfo *svi = ssa_opt_vinfo(ctx, stored_vr);
      if (svi)
        ssa_opt_add_use_instr(svi, i);
    }
    changes++;
  }

  tcc_free(var_def_count);
  tcc_free(var_def_instr);
  tcc_free(var_addrtaken);
  return changes;
}

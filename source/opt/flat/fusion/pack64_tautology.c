/*
 *  TCC IR - 64-bit Register Pair Optimization
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"



/* tcc_ir_opt_pack64_tautology: fold `PACK64(low_half(X), X SHR #32)` into
 * `ASSIGN X` — the case where C source repacks a 64-bit value from its own
 * halves (e.g. `((uint64_t)(v >> 32) << 32) | (uint32_t)v == v`).
 *
 * After pack64 has produced a PACK64, both source TEMPs trace back through
 * ASSIGN/LOAD copies to the same u64 vreg X (lo via a narrowing read, hi via
 * `X SHR #32`), so the pack is the identity.  Rewrite it to `ASSIGN X` and let
 * copy-prop + identity-CMP folding clean up any later compare against X. */

/* Follow ASSIGN/LOAD copy chains from a vreg.  Returns the index of the first
 * defining op that is NOT a pure pass-through copy, or -1 if the chain is
 * ambiguous / hits a multiply-defined slot.  prev_didx tracks the last copy
 * passed through and is returned as the chain endpoint when the next vreg has
 * no def (e.g. a PARAM). */
static int p64taut_trace_back(TCCIRState *ir, int *temp_def_idx, int max_temp_pos,
                              int *var_def_idx, int max_var_pos,
                              int32_t vreg)
{
  int prev_didx = -1;
  for (int hops = 0; hops < 32; hops++)
  {
    int type = TCCIR_DECODE_VREG_TYPE(vreg);
    int pos = TCCIR_DECODE_VREG_POSITION(vreg);
    int didx = -1;
    if (type == TCCIR_VREG_TYPE_TEMP)
    {
      if (pos > max_temp_pos)
        return prev_didx;
      didx = temp_def_idx[pos];
    }
    else if (type == TCCIR_VREG_TYPE_VAR)
    {
      if (pos > max_var_pos)
        return prev_didx;
      didx = var_def_idx[pos];
    }
    else
    {
      /* PARAM or other — no IR-defining op; return the previous copy index. */
      return prev_didx;
    }
    if (didx < 0)
      return prev_didx;
    IRQuadCompact *q = &ir->compact_instructions[didx];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
      return didx; /* hit a real producing op */
    /* Pure copy — continue through src1 only if it's a value reference (VAR
     * storage read or TEMP value); a deref of a computed pointer / symbol /
     * immediate is a real memory access we must not trace through. */
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    int32_t src_vr = irop_get_vreg(src1);
    int src_tag = irop_get_tag(src1);
    int is_value_copy = (src_vr >= 0) &&
                        ((src_tag == IROP_TAG_VREG && !src1.is_lval) ||
                         (src_tag == IROP_TAG_STACKOFF && src1.is_lval));
    if (!is_value_copy)
      return prev_didx >= 0 ? prev_didx : didx;
    prev_didx = didx;
    vreg = src_vr;
  }
  return -1;
}

int tcc_ir_opt_pack64_tautology(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

  int max_temp_pos = 0, max_var_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    int t = TCCIR_DECODE_VREG_TYPE(vr);
    int p = TCCIR_DECODE_VREG_POSITION(vr);
    if (t == TCCIR_VREG_TYPE_TEMP && p > max_temp_pos)
      max_temp_pos = p;
    else if (t == TCCIR_VREG_TYPE_VAR && p > max_var_pos)
      max_var_pos = p;
  }

  int temp_stride = max_temp_pos + 1;
  int var_stride = max_var_pos + 1;
  int *temp_def_idx = tcc_malloc(temp_stride * sizeof(int));
  int *var_def_idx = tcc_malloc(var_stride * sizeof(int));
  uint16_t *temp_use_count = tcc_mallocz(temp_stride * sizeof(uint16_t));
  for (int i = 0; i < temp_stride; i++)
    temp_def_idx[i] = -1;
  for (int i = 0; i < var_stride; i++)
    var_def_idx[i] = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[q->op].has_src1)
    {
      int32_t vr = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_temp_pos && temp_use_count[pos] < 0xFFFF)
          temp_use_count[pos]++;
      }
    }
    if (irop_config[q->op].has_src2)
    {
      int32_t vr = irop_get_vreg(tcc_ir_op_get_src2(ir, q));
      if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_temp_pos && temp_use_count[pos] < 0xFFFF)
          temp_use_count[pos]++;
      }
    }
    if (irop_config[q->op].has_dest)
    {
      int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      int t = TCCIR_DECODE_VREG_TYPE(vr);
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      /* STORE-like ops use dest as an address sink, not a vreg def. */
      int is_real_def = (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED &&
                        q->op != TCCIR_OP_FUNCPARAMVAL);
      if (is_real_def)
      {
        int *tbl = NULL;
        int max_pos = -1;
        if (t == TCCIR_VREG_TYPE_TEMP) { tbl = temp_def_idx; max_pos = max_temp_pos; }
        else if (t == TCCIR_VREG_TYPE_VAR) { tbl = var_def_idx; max_pos = max_var_pos; }
        if (tbl && pos <= max_pos)
        {
          if (tbl[pos] >= 0)
            tbl[pos] = -2; /* multiply-defined */
          else
            tbl[pos] = i;
        }
      }
    }
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_PACK64)
      continue;
    IROperand pk_dest = tcc_ir_op_get_dest(ir, q);
    if (pk_dest.is_lval)
      continue;

    IROperand lo_op = tcc_ir_op_get_src1(ir, q);
    IROperand hi_op = tcc_ir_op_get_src2(ir, q);
    int32_t lo_vr = irop_get_vreg(lo_op);
    int32_t hi_vr = irop_get_vreg(hi_op);
    if (lo_vr < 0 || hi_vr < 0)
      continue;

    /* Trace lo and hi back through ASSIGN/LOAD copy chains. */
    int lo_def_i = p64taut_trace_back(ir, temp_def_idx, max_temp_pos, var_def_idx, max_var_pos, lo_vr);
    int hi_def_i = p64taut_trace_back(ir, temp_def_idx, max_temp_pos, var_def_idx, max_var_pos, hi_vr);
    if (lo_def_i < 0 || hi_def_i < 0)
      continue;

    IRQuadCompact *lo_def = &ir->compact_instructions[lo_def_i];
    IRQuadCompact *hi_def = &ir->compact_instructions[hi_def_i];

    /* hi_def must be `T_hi = X SHR #32`. */
    if (hi_def->op != TCCIR_OP_SHR)
      continue;
    IROperand hi_src = tcc_ir_op_get_src1(ir, hi_def);
    IROperand hi_amt = tcc_ir_op_get_src2(ir, hi_def);
    if (!irop_is_immediate(hi_amt) || irop_get_imm64_ex(ir, hi_amt) != 32)
      continue;
    int32_t x_hi_vr = irop_get_vreg(hi_src);
    if (x_hi_vr < 0)
      continue;

    /* lo_def's chain root: an ASSIGN/LOAD pulling from X. */
    if (lo_def->op != TCCIR_OP_ASSIGN && lo_def->op != TCCIR_OP_LOAD)
      continue;
    IROperand lo_src = tcc_ir_op_get_src1(ir, lo_def);
    int32_t x_lo_vr = irop_get_vreg(lo_src);
    if (x_lo_vr < 0)
      continue;

    /* Endpoints must reference X with matching access semantics: if one reads X
     * as an lvalue (value at storage) and the other as an address, the pack is
     * not the identity. */
    if (lo_src.is_lval != hi_src.is_lval)
      continue;

    if (x_lo_vr != x_hi_vr)
      continue;

    /* X must be 64-bit, else `X SHR #32` is 0 and the pack is not the identity. */
    IRLiveInterval *x_interval = tcc_ir_get_live_interval(ir, x_lo_vr);
    if (!x_interval || !(x_interval->is_llong || x_interval->is_double))
      continue;

    LOG_IR_GEN("OPTIMIZE: PACK64 tautology at i=%d (X vr=%d)", i, x_lo_vr);

    /* Rewrite to ASSIGN dest = lo_src (the u64 lvalue reference to X). */
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, lo_src);
    changes++;

    /* Forward-substitute the dest with X in later same-block uses until it's
     * redefined, so identity-CMP folding catches `CMP X, X` when the dest is a
     * VAR (which copy_prop does not track). */
    int32_t dest_vr = irop_get_vreg(pk_dest);
    if (dest_vr >= 0)
    {
      for (int j = i + 1; j < n; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op == TCCIR_OP_NOP)
          continue;
        if (jq->is_jump_target)
          break;
        /* Stop on control-flow ops (preserve correctness across BBs). */
        if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF || jq->op == TCCIR_OP_IJUMP ||
            jq->op == TCCIR_OP_RETURNVOID || jq->op == TCCIR_OP_RETURNVALUE)
          break;
        if (irop_config[jq->op].has_src1)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, jq);
          if (irop_get_vreg(s1) == dest_vr)
            tcc_ir_set_src1(ir, j, lo_src);
        }
        if (irop_config[jq->op].has_src2)
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, jq);
          if (irop_get_vreg(s2) == dest_vr)
            tcc_ir_set_src2(ir, j, lo_src);
        }
        if (irop_config[jq->op].has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, jq);
          if (irop_get_vreg(d) == dest_vr)
            break;
        }
      }
    }
  }

  tcc_free(temp_def_idx);
  tcc_free(var_def_idx);
  tcc_free(temp_use_count);
  return changes;
}
int tcc_ir_opt_pack64_tautology_ex(IROptCtx *ctx) { return tcc_ir_opt_pack64_tautology(ctx->ir); }

/*
 *  TCC IR - VAR→TMP local forwarding (flat DSL pass)
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
#include "opt_du.h"
#include "opt_utils.h"
#include "opt_dsl.h"
#include "opt/flat/var_tmp_fwd.h"

/* Facts computed once in vtf_begin, read per STORE/ASSIGN (per-store recompute
 * would be quadratic). */
typedef struct VtfState
{
  int n;
  uint8_t *is_target;
  int has_nested_or_chain;
  int max_var_for_lea;
  uint8_t *var_has_lea;
} VtfState;

static void *vtf_begin(IROptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  VtfState *st = tcc_mallocz(sizeof(VtfState));
  st->n = n;
  if (n < 2)
    return st;

  /* Fresh jump-target bitmap: the IRQuadCompact::is_jump_target flags desync
   * between passes, and a back-edge into the scan window must stop forwarding. */
  st->is_target = tcc_mallocz((size_t)((n + 7) / 8));
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *jq = &ir->compact_instructions[i];
    if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
    {
      IROperand d = tcc_ir_op_get_dest(ir, jq);
      int t = (int)d.u.imm32;
      if (t >= 0 && t < n)
        st->is_target[t >> 3] |= (uint8_t)(1u << (t & 7));
    }
  }
  for (int tbl = 0; tbl < ir->num_switch_tables; tbl++)
  {
    TCCIRSwitchTable *swt = &ir->switch_tables[tbl];
    for (int k = 0; k < swt->num_entries; k++)
    {
      int t = swt->targets[k];
      if (t >= 0 && t < n)
        st->is_target[t >> 3] |= (uint8_t)(1u << (t & 7));
    }
    if (swt->default_target >= 0 && swt->default_target < n)
      st->is_target[swt->default_target >> 3] |= (uint8_t)(1u << (swt->default_target & 7));
  }

  /* addrtaken refinement: only an actual aliasing path (SET_CHAIN, or a LEA of
   * the VAR) blocks forwarding — a bare frontend addrtaken flag can be stale. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *sq = &ir->compact_instructions[i];
    if (sq->op == TCCIR_OP_SET_CHAIN || sq->op == TCCIR_OP_INIT_CHAIN_SLOT)
    {
      st->has_nested_or_chain = 1;
      break;
    }
    if (sq->op == TCCIR_OP_LEA)
    {
      IROperand ls = tcc_ir_op_get_src1(ir, sq);
      int32_t vr = irop_get_vreg(ls);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > st->max_var_for_lea)
          st->max_var_for_lea = pos;
      }
    }
  }
  if (st->max_var_for_lea > 0 && !st->has_nested_or_chain)
  {
    st->var_has_lea = tcc_mallocz((st->max_var_for_lea + 8) / 8);
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *sq = &ir->compact_instructions[i];
      if (sq->op == TCCIR_OP_LEA)
      {
        IROperand ls = tcc_ir_op_get_src1(ir, sq);
        int32_t vr = irop_get_vreg(ls);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos <= st->max_var_for_lea)
            st->var_has_lea[pos / 8] |= (1 << (pos % 8));
        }
      }
    }
  }
  return st;
}

static void vtf_end(IROptCtx *ctx)
{
  VtfState *st = ctx->pass_state;
  if (!st)
    return;
  tcc_free(st->var_has_lea);
  tcc_free(st->is_target);
  tcc_free(st);
}

OPT_GEN_FLAT(var_tmp_fwd, -1)
{
  VtfState *st = ctx->pass_state;
  TCCIRState *ir = ctx->ir;
  int n = st->n;
  IRQuadCompact *store_q = &ir->compact_instructions[i];

  if (store_q->op != TCCIR_OP_STORE && store_q->op != TCCIR_OP_ASSIGN)
    return 0;
  if (!irop_config[store_q->op].has_dest || !irop_config[store_q->op].has_src1)
    return 0;

  IROperand dest = tcc_ir_op_get_dest(ir, store_q);
  int32_t dest_vr = irop_get_vreg(dest);
  if (TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_VAR)
    return 0;

  {
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vr);
    if (interval && interval->addrtaken)
    {
      int dpos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      if (st->has_nested_or_chain ||
          (st->var_has_lea && dpos <= st->max_var_for_lea &&
           (st->var_has_lea[dpos / 8] & (1 << (dpos % 8)))))
        return 0;
    }
  }

  IROperand src1 = tcc_ir_op_get_src1(ir, store_q);
  int32_t src_vr = irop_get_vreg(src1);
  if (TCCIR_DECODE_VREG_TYPE(src_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  /* Only register-held sources: forwarding an lval TEMP (*T) would duplicate a
   * memory read that later passes may misfold to the initializer (seed 588). */
  if (src1.is_lval)
    return 0;

  /* Skip LEA-address TEMPs, and forward only the immediately-preceding producer:
   * extending a TEMP across intervening stores can miscompile store-heavy code
   * (seed 814). */
  {
    int t_def = tcc_ir_find_defining_instruction(ir, src_vr, i);
    if (t_def >= 0 && ir->compact_instructions[t_def].op == TCCIR_OP_LEA)
      return 0;
    int prev = i - 1;
    while (prev >= 0 && ir->compact_instructions[prev].op == TCCIR_OP_NOP)
      prev--;
    if (t_def != prev)
      return 0;
  }

  int src_btype = irop_get_btype(src1);
  int changes = 0;

  for (int j = i + 1; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];

    /* Must precede the NOP-skip: a jump target can land on a NOP. */
    if (st->is_target[j >> 3] & (1u << (j & 7)))
      break;

    if (q->op == TCCIR_OP_NOP)
      continue;
    /* A JMP to the next real instruction is a fallthrough — keep scanning. */
    if (q->op == TCCIR_OP_JUMP)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int jt = (int)jd.u.imm32;
      while (jt < n && ir->compact_instructions[jt].op == TCCIR_OP_NOP)
        jt++;
      int next_real = j + 1;
      while (next_real < n && ir->compact_instructions[next_real].op == TCCIR_OP_NOP)
        next_real++;
      if (jt == next_real)
        continue;
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_RETURNVALUE ||
        q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE)
      break;
    /* A call would force T to spill, defeating the purpose. */
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      break;

    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t d_vr = irop_get_vreg(d);
      if (d_vr == src_vr)
        break;
    }

    /* Skip the operand slots that encode callee sym / call id, not a value. */
    int can_rewrite_src1 = (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID);
    int can_rewrite_src2 = (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID &&
                            q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID);

    /* Ops whose src1 is an address, not a value — the TEMP holds the value. */
    int src1_is_address = (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_LEA ||
                           q->op == TCCIR_OP_LOAD_INDEXED);

    if (!src1_is_address && can_rewrite_src1 && irop_config[q->op].has_src1)
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(s) == dest_vr && irop_get_btype(s) == src_btype && !s.is_llocal)
      {
        tcc_ir_set_src1(ir, j, src1);
        changes++;
        LOG_IR_GEN("VAR→TMP FWD: V=%d -> T at j=%d (src1) after store at i=%d", dest_vr, j, i);
      }
    }
    if (can_rewrite_src2 && irop_config[q->op].has_src2)
    {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      if (irop_get_vreg(s) == dest_vr && irop_get_btype(s) == src_btype && !s.is_llocal)
      {
        tcc_ir_set_src2(ir, j, src1);
        changes++;
        LOG_IR_GEN("VAR→TMP FWD: V=%d -> T at j=%d (src2) after store at i=%d", dest_vr, j, i);
      }
    }

    /* A redef of V ends the forwarding — later reads see the new value. */
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t d_vr = irop_get_vreg(d);
      if (d_vr == dest_vr)
        break;
    }
  }

  return changes;
}

static const IROptStatefulOps vtf_ops = {
    vtf_begin,
    NULL,
    vtf_end,
};

const IROptGen var_tmp_fwd_gens[] = {
    OPT_GEN_ENTRY_FLAT(var_tmp_fwd, TCCIR_OP_STORE),
    OPT_GEN_ENTRY_FLAT(var_tmp_fwd, TCCIR_OP_ASSIGN),
};

const int var_tmp_fwd_gens_count = (int)(sizeof(var_tmp_fwd_gens) / sizeof(var_tmp_fwd_gens[0]));

int tcc_ir_opt_var_tmp_fwd_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_stateful_gens(ctx, var_tmp_fwd_gens, var_tmp_fwd_gens_count, &vtf_ops);
}

int tcc_ir_opt_var_tmp_fwd(TCCIRState *ir)
{
  if (!ir)
    return 0;
  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_var_tmp_fwd_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);
  return changes;
}

#undef USING_GLOBALS

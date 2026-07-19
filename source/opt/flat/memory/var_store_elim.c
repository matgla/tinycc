/*
 *  TCC IR - dead VAR-store + redundant VAR-assign elimination (flat, pre-SSA)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

static int ir_dce_addrof_var_pos(TCCIRState *ir, IRQuadCompact *q)
{
  if (q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ASSIGN)
    return -1;
  if (!irop_config[q->op].has_src1)
    return -1;

  IROperand s = tcc_ir_op_get_src1(ir, q);
  int32_t vr = irop_get_vreg(s);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
    return -1;
  if (q->op == TCCIR_OP_ASSIGN && !(s.is_local && !s.is_lval))
    return -1;
  return TCCIR_DECODE_VREG_POSITION(vr);
}
/* VAR-position of op if it's a VAR with pos<=max_var, else -1 */
static int var_pos_in_range(IROperand op, int max_var)
{
  int32_t vr = irop_get_vreg(op);
  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
  {
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos <= max_var)
      return pos;
  }
  return -1;
}
/* 1 if q's only effect is defining its dest (safe to NOP when dest unread) */
static int ir_op_pure_for_dead_var_dest(TCCIRState *ir, IRQuadCompact *q)
{
  switch (q->op) {
  case TCCIR_OP_NOP:
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_STORE_POSTINC:
  case TCCIR_OP_LOAD_POSTINC:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_SET_CHAIN:
  case TCCIR_OP_INIT_CHAIN_SLOT:
  case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_ASM_INPUT:
  case TCCIR_OP_ASM_OUTPUT:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_VLA_SP_SAVE:
  case TCCIR_OP_VLA_SP_RESTORE:
  case TCCIR_OP_PREFETCH:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_SETJMP:
  case TCCIR_OP_LONGJMP:
  case TCCIR_OP_CALLSEQ_BEGIN:
  case TCCIR_OP_CALLARG_REG:
  case TCCIR_OP_CALLARG_STACK:
  case TCCIR_OP_CALLSEQ_END:
  case TCCIR_OP_CMP:
    return 0;
  case TCCIR_OP_LOAD: {
    /* no MMIO / volatile risk */
    IROperand s = tcc_ir_op_get_src1(ir, q);
    int32_t v = irop_get_vreg(s);
    return irop_is_immediate(s) || s.is_sym ||
           (v <= -2 && v >= -9) ||
           (irop_get_tag(s) == IROP_TAG_STACKOFF && s.is_local);
  }
  case TCCIR_OP_STORE: {
    /* only local stores are safe; pointer stores are observable */
    IROperand d = tcc_ir_op_get_dest(ir, q);
    return d.is_local;
  }
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID: {
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee) return 0;
    const char *name = get_tok_str(callee->v, NULL);
    return name && tcc_ir_is_pure_aeabi(name);
  }
  default:
    return 1;
  }
}

int tcc_ir_opt_dead_var_store_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;

  int max_var = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int nops = (q->op == TCCIR_OP_MLA) ? 4 : 3;
    for (int k = 0; k < nops; k++)
    {
      IROperand op = (k == 0)   ? tcc_ir_op_get_dest(ir, q)
                     : (k == 1) ? tcc_ir_op_get_src1(ir, q)
                     : (k == 2) ? tcc_ir_op_get_src2(ir, q)
                                : tcc_ir_op_get_accum(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var)
          max_var = pos;
      }
    }
  }
  if (max_var == 0)
    return 0;

  uint8_t *var_read = tcc_mallocz((max_var + 8) / 8);
  uint8_t *var_has_lea = tcc_mallocz((max_var + 8) / 8);
  int has_set_chain = 0;

  /* volatile VAR store is a mandated side effect; mark it read */
  for (int p = 0; p <= max_var && p < ir->variables_live_intervals_size; p++)
    if (ir->variables_live_intervals[p].is_volatile)
      var_read[p / 8] |= (1 << (p % 8));

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_SET_CHAIN || q->op == TCCIR_OP_INIT_CHAIN_SLOT)
      has_set_chain = 1;
    int addrof_pos = ir_dce_addrof_var_pos(ir, q);
    if (addrof_pos >= 0 && addrof_pos <= max_var)
      var_has_lea[addrof_pos / 8] |= (1 << (addrof_pos % 8));
    if (irop_config[q->op].has_src1)
    {
      int pos = var_pos_in_range(tcc_ir_op_get_src1(ir, q), max_var);
      if (pos >= 0)
        var_read[pos / 8] |= (1 << (pos % 8));
    }
    if (irop_config[q->op].has_src2)
    {
      int pos = var_pos_in_range(tcc_ir_op_get_src2(ir, q), max_var);
      if (pos >= 0)
        var_read[pos / 8] |= (1 << (pos % 8));
    }
    /* MLA accum is a third read not covered by src1/src2; count it */
    if (q->op == TCCIR_OP_MLA)
    {
      int pos = var_pos_in_range(tcc_ir_op_get_accum(ir, q), max_var);
      if (pos >= 0)
        var_read[pos / 8] |= (1 << (pos % 8));
    }
    /* STORE_INDEXED/STORE_POSTINC dest is a pointer read, not a def */
    if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
    {
      int pos = var_pos_in_range(tcc_ir_op_get_dest(ir, q), max_var);
      if (pos >= 0)
        var_read[pos / 8] |= (1 << (pos % 8));
    }
  }

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!ir_op_pure_for_dead_var_dest(ir, q))
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos > max_var)
      continue;
    if (var_read[pos / 8] & (1 << (pos % 8)))
      continue;
    /* addrtaken VAR is live via pointer only if a LEA or SET_CHAIN exists */
    {
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
      if (interval && interval->addrtaken &&
          ((var_has_lea[pos / 8] & (1 << (pos % 8))) || has_set_chain))
        continue;
    }
    LOG_IR_GEN("=== DEAD VAR STORE: eliminating V%d at i=%d (op=%d) ===", pos, i, q->op);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  LOG_IR_GEN("=== DEAD VAR STORE ELIM: eliminated %d dead VAR stores ===", changes);
  tcc_free(var_has_lea);
  tcc_free(var_read);
  return changes;
}
static int tcc_ir_opt_redundant_var_assign__timed(TCCIRState *ir);
/* "redundant_assign" is this pass's name everywhere else (pass table, docs,
 * TCC_DISABLE_PASS); timing under it too keeps the two layers on one row. */
int tcc_ir_opt_redundant_var_assign(TCCIRState *ir)
{
  int r;
  TCC_PASS_TIMED(r, "redundant_assign", tcc_ir_opt_redundant_var_assign__timed(ir));
  return r;
}
static int tcc_ir_opt_redundant_var_assign__timed(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  int max_var = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand ops[3];
    ops[0] = tcc_ir_op_get_dest(ir, q);
    ops[1] = tcc_ir_op_get_src1(ir, q);
    ops[2] = tcc_ir_op_get_src2(ir, q);
    for (int k = 0; k < 3; k++)
    {
      int32_t vr = irop_get_vreg(ops[k]);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var)
          max_var = pos;
      }
    }
  }

  if (max_var == 0)
    return 0;

  /* Mark jump targets as merge points — must flush pending at these */
  uint8_t *is_target = tcc_mallocz((n + 7) / 8);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)dest.u.imm32;
      if (target >= 0 && target < n)
        is_target[target / 8] |= (1 << (target % 8));
    }
  }

  /* pending[v] valid only while pending_epoch[v]==cur_epoch; flush = cur_epoch++ */
  int *pending = tcc_malloc(sizeof(int) * (max_var + 1));
  int *pending_epoch = tcc_mallocz(sizeof(int) * (max_var + 1));
  int cur_epoch = 1;

  uint8_t *var_addr_taken = tcc_mallocz((max_var + 8) / 8);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int pos = ir_dce_addrof_var_pos(ir, q);
    if (pos >= 0 && pos <= max_var)
      var_addr_taken[pos / 8] |= (1 << (pos % 8));
  }

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (is_target[i / 8] & (1 << (i % 8)))
      cur_epoch++;

    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
        q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_SWITCH_TABLE)
    {
      cur_epoch++;
      continue;
    }

    if (irop_config[q->op].has_src1)
    {
      int pos = var_pos_in_range(tcc_ir_op_get_src1(ir, q), max_var);
      if (pos >= 0)
        pending_epoch[pos] = 0;
    }
    if (irop_config[q->op].has_src2)
    {
      int pos = var_pos_in_range(tcc_ir_op_get_src2(ir, q), max_var);
      if (pos >= 0)
        pending_epoch[pos] = 0;
    }

    /* MLA accum is a read not surfaced by src1/src2; must clear pending */
    if (q->op == TCCIR_OP_MLA)
    {
      int pos = var_pos_in_range(tcc_ir_op_get_accum(ir, q), max_var);
      if (pos >= 0)
        pending_epoch[pos] = 0;
    }

    /* STORE dest is a pointer USE — if it's a VAR, count as read */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
    {
      int pos = var_pos_in_range(tcc_ir_op_get_dest(ir, q), max_var);
      if (pos >= 0)
        pending_epoch[pos] = 0;
      continue;
    }

    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
        {
          if (var_addr_taken[pos / 8] & (1 << (pos % 8)))
          {
            pending_epoch[pos] = 0;
            continue;
          }
          if (pending_epoch[pos] == cur_epoch)
          {
            ir->compact_instructions[pending[pos]].op = TCCIR_OP_NOP;
            changes++;
          }
          pending[pos] = i;
          pending_epoch[pos] = cur_epoch;
        }
      }
    }
  }

  LOG_IR_GEN("=== REDUNDANT VAR ASSIGN: eliminated %d dead assigns ===", changes);

  tcc_free(var_addr_taken);
  tcc_free(pending_epoch);
  tcc_free(pending);
  tcc_free(is_target);
  return changes;
}
int tcc_ir_opt_redundant_var_assign_ex(IROptCtx *ctx) { return tcc_ir_opt_redundant_var_assign(ctx->ir); }
int tcc_ir_opt_dead_var_store_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_dead_var_store_elim(ctx->ir); }

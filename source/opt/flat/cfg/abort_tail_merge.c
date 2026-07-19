/*
 *  TCC IR - Abort tail-merge: share one noreturn-call sink per callee, invert the other guards
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
#include "opt_loop_utils.h"


/* Matches `JUMPIF cond -> CONT; FUNCPARAMVOID*; FUNCCALL* noreturn; CONT`; returns the entry index (i+1) or -1. */
/* CONT == j+1 keeps this a clean guarded diamond and excludes loop-latch / entry-guard JUMPIFs. */
static int ir_abort_guard_site(TCCIRState *ir, int i, int n, int *call_idx, int *cond_out,
                               Sym **callee_out, int *is_zero)
{
  IRQuadCompact *jif = &ir->compact_instructions[i];
  if (jif->op != TCCIR_OP_JUMPIF)
    return -1;

  int cont = (int)irop_get_imm32(tcc_ir_op_get_dest(ir, jif));
  int cond = (int)tcc_ir_op_get_src1(ir, jif).u.imm32;

  int j = i + 1;
  while (j < n && ir->compact_instructions[j].op == TCCIR_OP_FUNCPARAMVOID)
    j++;
  if (j >= n)
    return -1;
  /* __builtin_abort lowers to FUNCCALLVAL; its dest vreg is dead since the callee never returns. */
  /* Both forms read alike below: the operand accessors are config-aware and offset src1/src2 past the dest. */
  TccIrOp callop = ir->compact_instructions[j].op;
  if (callop != TCCIR_OP_FUNCCALLVOID && callop != TCCIR_OP_FUNCCALLVAL)
    return -1;

  IRQuadCompact *call = &ir->compact_instructions[j];
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, call));
  if (!tcc_ir_callee_is_noreturn(callee))
    return -1;
  if (TCCIR_DECODE_CALL_ARGC((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call))) != 0)
    return -1;
  if (cont != j + 1)
    return -1;
  /* A jump landing INSIDE the sequence (i+2..j) can't be redirected safely; the entry (i+1) may be a target. */
  for (int k = i + 2; k <= j; k++)
    if (ir->compact_instructions[k].is_jump_target)
      return -1;

  /* cbz/cbnz-eligible: cheap inline as one fused cbz, but the fusion is lost if inverted — best sink. */
  int zero = 0;
  if (i >= 1 && (cond == TOK_EQ || cond == TOK_NE))
  {
    IRQuadCompact *prev = &ir->compact_instructions[i - 1];
    if (prev->op == TCCIR_OP_TEST_ZERO)
      zero = 1;
    else if (prev->op == TCCIR_OP_CMP)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, prev);
      if (s2.tag == IROP_TAG_IMM32 && s2.u.imm32 == 0)
        zero = 1;
    }
  }

  *call_idx = j;
  *cond_out = cond;
  *callee_out = callee;
  *is_zero = zero;
  return i + 1;
}

/* Per noreturn callee, keep ONE site inline as the shared sink; invert + retarget the rest and NOP their param+call. */
/* Sink choice prefers a cbz/cbnz-eligible site: inverting it would cost cmp+bne (cbz/cbnz are forward-only), saving nothing. */
/* A kept-inline site is a safe sink: it keeps its fall-through-into-call layout, and argc==0 means no arg setup differs per edge. */
/* Runs post-regalloc, before the jump-thread / fallthrough / DCE cleanup; deliberately no compact_nops (renumbering breaks index-keyed peepholes). */
int tcc_ir_opt_abort_tail_merge(TCCIRState *ir)
{
  if (getenv("TCC_NO_ABORT_MERGE"))
    return 0;

  int n = ir->next_instruction_index;
  if (n < 3)
    return 0;

  /* Un-enumerable control flow could branch into a guarded region we assume is entered only by fall-through. */
  for (int i = 0; i < n; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD)
      return 0;
  }

  enum { MAX_SINKS = 8 };
  Sym *cal[MAX_SINKS];
  int first_entry[MAX_SINKS]; /* fallback sink */
  int zero_entry[MAX_SINKS];  /* first cbz-eligible site, or -1 */
  int num = 0;

  int j, cond, is_zero;
  Sym *callee;

  for (int i = 0; i + 1 < n; i++)
  {
    if (ir_abort_guard_site(ir, i, n, &j, &cond, &callee, &is_zero) < 0)
      continue;
    int s = -1;
    for (int t = 0; t < num; t++)
      if (cal[t] == callee)
      {
        s = t;
        break;
      }
    if (s < 0)
    {
      if (num >= MAX_SINKS)
        continue;
      s = num++;
      cal[s] = callee;
      first_entry[s] = i + 1;
      zero_entry[s] = -1;
    }
    if (is_zero && zero_entry[s] < 0)
      zero_entry[s] = i + 1;
  }
  if (num == 0)
    return 0;

  int sink_used[MAX_SINKS] = {0};
  int changes = 0;
  for (int i = 0; i + 1 < n; i++)
  {
    int entry = ir_abort_guard_site(ir, i, n, &j, &cond, &callee, &is_zero);
    if (entry < 0)
      continue;
    int s = -1;
    for (int t = 0; t < num; t++)
      if (cal[t] == callee)
      {
        s = t;
        break;
      }
    if (s < 0)
      continue; /* callee overflowed MAX_SINKS */

    int sink = (zero_entry[s] >= 0) ? zero_entry[s] : first_entry[s];
    if (entry == sink)
      continue; /* this IS the sink — leave it inline */

    int inv = invert_condition(cond);
    if (inv < 0)
      continue;

    /* `if (A || B) abort()`: redirect predecessors jumping here to the sink, else A-true falls through the NOPs to CONT. */
    if (ir->compact_instructions[entry].is_jump_target)
    {
      for (int p = 0; p < n; p++)
      {
        IRQuadCompact *pq = &ir->compact_instructions[p];
        if (pq->op != TCCIR_OP_JUMP && pq->op != TCCIR_OP_JUMPIF)
          continue;
        IROperand pd = tcc_ir_op_get_dest(ir, pq);
        if (pd.tag != IROP_TAG_IMM32 || pd.u.imm32 != entry)
          continue;
        IROperand sink_dest = {0};
        sink_dest.tag = IROP_TAG_IMM32;
        sink_dest.u.imm32 = sink;
        tcc_ir_op_set_dest(ir, pq, sink_dest);
      }
      ir->compact_instructions[entry].is_jump_target = 0;
    }

    IRQuadCompact *jif = &ir->compact_instructions[i];
    IROperand new_cond = {0};
    new_cond.tag = IROP_TAG_IMM32;
    new_cond.u.imm32 = inv;
    tcc_ir_op_set_src1(ir, jif, new_cond);

    IROperand new_dest = {0};
    new_dest.tag = IROP_TAG_IMM32;
    new_dest.u.imm32 = sink;
    tcc_ir_op_set_dest(ir, jif, new_dest);

    for (int k = i + 1; k <= j; k++)
      ir->compact_instructions[k].op = TCCIR_OP_NOP;

    sink_used[s] = 1;
    changes++;
  }

  /* Deferred: marking a sink mid-scan would make ir_abort_guard_site reject it before the entry==sink test runs. */
  for (int s = 0; s < num; s++)
    if (sink_used[s])
    {
      int sink = (zero_entry[s] >= 0) ? zero_entry[s] : first_entry[s];
      ir->compact_instructions[sink].is_jump_target = 1;
    }

  return changes;
}

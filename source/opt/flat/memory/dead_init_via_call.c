/*
 *  TCC IR - dead stack-slot init overwritten by a callee (flat, pre-SSA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_utils.h"
#include "func_write_summary.h"



/* Kill stack-slot stores fully overwritten by a later call, per its write summary. */
int tcc_ir_opt_dead_init_via_call(TCCIRState *ir)
{
  if (!ir)
    return 0;
  const int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  int changes = 0;

  for (int call_idx = 0; call_idx < n; call_idx++)
  {
    IRQuadCompact *call_q = &ir->compact_instructions[call_idx];
    if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, call_q));
    if (!callee)
      continue;
    FuncWriteSummary *summary = fws_lookup(callee);
    if (!summary)
      continue;

    for (int pi = 0; pi < summary->num_params; pi++)
    {
      FwsParamSummary *ps = &summary->params[pi];
      IROperand pop;
      if (!ir_opt_get_call_param_operand(ir, call_idx, ps->param_idx, &pop))
        continue;
      /* Param must be Addr[StackLoc[X]] — local, not lval, STACKOFF-tagged. */
      if (!pop.is_local || pop.is_lval)
        continue;
      if (irop_get_tag(pop) != IROP_TAG_STACKOFF)
        continue;
      int32_t base_off = (int32_t)irop_get_stack_offset(pop);

      for (int s = call_idx - 1; s >= 0; s--)
      {
        IRQuadCompact *sq = &ir->compact_instructions[s];
        if (sq->op == TCCIR_OP_NOP)
          continue;
        /* Bail at any control-flow boundary or call: other block / unknown effects. */
        if (sq->op == TCCIR_OP_JUMP || sq->op == TCCIR_OP_JUMPIF || sq->op == TCCIR_OP_IJUMP ||
            sq->op == TCCIR_OP_SWITCH_TABLE || sq->op == TCCIR_OP_FUNCCALLVAL || sq->op == TCCIR_OP_FUNCCALLVOID)
          break;
        if (sq->is_jump_target)
          break;
        if (sq->op != TCCIR_OP_STORE)
          continue;
        IROperand sdst = tcc_ir_op_get_dest(ir, sq);
        if (!sdst.is_local || irop_get_tag(sdst) != IROP_TAG_STACKOFF)
          continue;
        int32_t s_off = (int32_t)irop_get_stack_offset(sdst);
        int s_size = fws_btype_bytes(sdst.btype);
        if (s_size <= 0)
          continue;

        int32_t rel_off = s_off - base_off;
        if (!fws_range_fully_set(ps, rel_off, s_size))
          continue;

        /* Only an lval (deref) reference counts as a read; addr-of is covered by the summary. */
        int slot_read = 0;
        for (int t = s + 1; t < call_idx && !slot_read; t++)
        {
          IRQuadCompact *tq = &ir->compact_instructions[t];
          if (tq->op == TCCIR_OP_NOP)
            continue;
          for (int k = 1; k < 3 && !slot_read; k++)
          {
            if (k == 1 && !irop_config[tq->op].has_src1)
              continue;
            if (k == 2 && !irop_config[tq->op].has_src2)
              continue;
            IROperand op = (k == 1) ? tcc_ir_op_get_src1(ir, tq) : tcc_ir_op_get_src2(ir, tq);
            if (!op.is_local || irop_get_tag(op) != IROP_TAG_STACKOFF)
              continue;
            if (!op.is_lval)
              continue;
            int32_t op_off = (int32_t)irop_get_stack_offset(op);
            int op_sz = fws_btype_bytes(op.btype);
            if (op_sz <= 0)
              op_sz = 8; /* unknown size — assume worst case for overlap */
            if (op_off + op_sz > s_off && op_off < s_off + s_size)
              slot_read = 1;
          }
        }
        if (slot_read)
          continue;

        LOG_IR_GEN("DEAD INIT VIA CALL: nop STORE at i=%d (call i=%d, callee=%p, param=%d, rel_off=%d, size=%d)", s,
                   call_idx, (void *)callee, ps->param_idx, rel_off, s_size);
        sq->op = TCCIR_OP_NOP;
        changes++;
      }
    }
  }

  return changes;
}

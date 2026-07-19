/*
 *  TCC IR - Infinite Self-Recursion Collapse
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
#include "opt_engine.h"
#include "opt_xform.h"
#include "opt_alias.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "opt_loop_utils.h"
#include "cfg.h"
#include "licm.h"

int tcc_ir_opt_infinite_self_recursion(TCCIRState *ir, Sym *func_sym)
{
  int n = ir->next_instruction_index;
  if (n == 0 || !func_sym)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  int self_call_idx = -1;
  for (int i = 0; i < n && self_call_idx < 0; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (callee == func_sym)
      {
        self_call_idx = i;
        break;
      }
      /* Non-self call may return, then fall through to a RETURN — bail. */
      return 0;
    }
    /* Early exit/branch before the self-call breaks "unconditionally reached". */
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
      return 0;
    default:
      break;
    }

    /* Volatile sym access is observable regardless of return — preserve. */
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      if (op.is_sym)
      {
        Sym *sym = irop_get_sym_ex(ir, op);
        if (sym && (sym->type.t & VT_VOLATILE))
          return 0;
      }
    }
  }

  if (self_call_idx < 0)
    return 0;

  LOG_IR_GEN("INFINITE-RECURSION-COLLAPSE: function unconditionally self-calls "
             "at i=%d; collapsing body to `b .`", self_call_idx);

  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->compact_instructions[0].op = TCCIR_OP_JUMP;
  ir->compact_instructions[0].is_jump_target = 1;
  IROperand self = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
  tcc_ir_set_dest(ir, 0, self);
  tcc_ir_set_src1(ir, 0, IROP_NONE);
  tcc_ir_set_src2(ir, 0, IROP_NONE);

  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;
  ir->noreturn = 1;
  if (func_sym && func_sym->type.ref)
    func_sym->type.ref->f.func_noreturn = 1;

  return 1;
}

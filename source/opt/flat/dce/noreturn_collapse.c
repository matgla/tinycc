/*
 *  TCC IR - Noreturn Collapse Pass
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

/* True when a jump target, after skipping NOPs, lands past the last live op. */
static int nc_jump_exits(TCCIRState *ir, int n, int last_idx, int jt)
{
  int t = jt;
  while (t < n && ir->compact_instructions[t].op == TCCIR_OP_NOP)
    t++;
  return (t >= n || t > last_idx);
}

int tcc_ir_opt_noreturn_collapse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  /* O2-only: elide observable-but-unreachable side effects (function never returns). */
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  int has_jump = 0;
  int has_store = 0; /* observable work — gate for publishing func_noreturn */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_BLOCK_COPY)
      has_store = 1;

    switch (q->op)
    {
    /* Return: function CAN return, so collapse would skip returning-path side effects. */
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    /* Calls publish state we can't elide (writes through passed pointers, handlers). */
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_CALLSEQ_BEGIN:
    case TCCIR_OP_CALLARG_REG:
    case TCCIR_OP_CALLARG_STACK:
    case TCCIR_OP_CALLSEQ_END:
    /* Inline asm could do anything (including exit / sync). */
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    /* Non-local control flow can return to the caller through a longjmp. */
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    /* Trap may dispatch to a fault/signal handler that observes state. */
    case TCCIR_OP_TRAP:
    /* VLA / SP juggling: stack-pointer side effects the prologue tracks. */
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    /* Computed goto: unknown-at-compile-time target — defensive bail. */
    case TCCIR_OP_IJUMP:
      return 0;
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
      has_jump = 1;
      break;
    default:
      break;
    }

    /* A volatile access anywhere keeps the function alive.  The symbol scan below
     * only catches a symbol that is itself volatile; when just a MEMBER is
     * (`struct { volatile int a; }`), the symbol's type is not, and only the
     * access carries the bit. */
    if (tcc_ir_instr_access_is_volatile(ir, q))
      return 0;

    /* A volatile sym on any operand keeps the function alive (observable write). */
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

  /* Need at least one JUMP — otherwise the function falls off the end (implicit return). */
  if (!has_jump)
    return 0;

  /* Implicit return: if the last non-NOP op can fall through (not an unconditional JUMP), the function still returns. */
  int last_idx = -1;
  for (int i = n - 1; i >= 0; i--)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
    {
      last_idx = i;
      break;
    }
  }
  if (last_idx < 0)
    return 0;
  if (ir->compact_instructions[last_idx].op != TCCIR_OP_JUMP)
    return 0;

  /* Only collapse when the final JUMP demonstrably loops back to a live earlier op. */
  {
    IROperand jdest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[last_idx]);
    int jt = (int)irop_get_imm64_ex(ir, jdest);
    if (jt < 0 || jt >= n || jt > last_idx)
      return 0;
    if (nc_jump_exits(ir, n, last_idx, jt))
      return 0;
  }

  /* An earlier conditional branch can still exit to the epilogue: any JUMP/JUMPIF whose target lands past the last live op is a reachable return path. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int jt = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
    if (jt < 0)
      continue;
    if (nc_jump_exits(ir, n, last_idx, jt))
      return 0;
  }

  LOG_IR_GEN("NORETURN-COLLAPSE: collapsing function body to infinite loop "
             "(no RETURN, no calls/asm/volatile — side effects unobservable)");

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
  /* Suppress the unreachable `bx lr`: control never reaches the epilogue. */
  ir->noreturn = 1;
  /* Publish func_noreturn to callers only when the body had a real STORE (genuine work in an infinite loop). */
  if (has_store && tcc_state && tcc_state->cur_func_sym &&
      tcc_state->cur_func_sym->type.ref)
    tcc_state->cur_func_sym->type.ref->f.func_noreturn = 1;

  return 1;
}

int tcc_ir_opt_noreturn_collapse_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_noreturn_collapse(ctx->ir);
}

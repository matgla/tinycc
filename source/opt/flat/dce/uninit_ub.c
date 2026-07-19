/*
 *  TCC IR - uninitialized-local UB elision passes
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

static int udr_store_is_observable(IROperand d)
{
  if (d.is_local)
    return 0;
  if (!d.is_lval && !d.is_sym)
  {
    int32_t dvr = irop_get_vreg(d);
    if (dvr >= 0)
    {
      int vt = TCCIR_DECODE_VREG_TYPE(dvr);
      if (vt == TCCIR_VREG_TYPE_VAR || vt == TCCIR_VREG_TYPE_PARAM)
        return 0;
    }
  }
  return 1;
}

/* Fetch operand k (0=dest, 1=src1, 2=src2) of q; 0 if that slot is absent. */
static int udr_get_operand(TCCIRState *ir, IRQuadCompact *q, int k, IROperand *op)
{
  if (k == 0)
  {
    if (!irop_config[q->op].has_dest)
      return 0;
    *op = tcc_ir_op_get_dest(ir, q);
  }
  else if (k == 1)
  {
    if (!irop_config[q->op].has_src1)
      return 0;
    *op = tcc_ir_op_get_src1(ir, q);
  }
  else
  {
    if (!irop_config[q->op].has_src2)
      return 0;
    *op = tcc_ir_op_get_src2(ir, q);
  }
  return 1;
}

static int udr_instr_has_volatile_operand(TCCIRState *ir, IRQuadCompact *q)
{
  for (int k = 0; k <= 2; k++)
  {
    IROperand op;
    if (!udr_get_operand(ir, q, k, &op))
      continue;
    if (op.is_sym)
    {
      Sym *vs = irop_get_sym_ex(ir, op);
      if (vs && (vs->type.t & VT_VOLATILE))
        return 1;
    }
  }
  return 0;
}

static int udr_has_inline_asm_or_ijump(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_ASM_INPUT || op == TCCIR_OP_ASM_OUTPUT || op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_IJUMP)
      return 1;
  }
  return 0;
}

/* Observable side-effect guard shared by the uninit-UB collapse passes: 1 if the
 * function does work visible after return (call, asm, non-local flow, trap, VLA,
 * volatile access, or a frame-escaping STORE). Stores to own locals don't count. */
static int udr_has_observable_side_effects(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_CALLSEQ_BEGIN:
    case TCCIR_OP_CALLARG_REG:
    case TCCIR_OP_CALLARG_STACK:
    case TCCIR_OP_CALLSEQ_END:
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
      return 1;
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_BLOCK_COPY:
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (udr_store_is_observable(d))
        return 1;
      break;
    }
    default:
      break;
    }
    if (udr_instr_has_volatile_operand(ir, q))
      return 1;
  }
  return 0;
}

/* Advance t past a run of NOPs, bounded by n. Kept as a separate non-inlined
 * function: inlining this loop miscompiled the armv8m self-host cross. */
static int udr_nopskip_target(const TCCIRState *ir, int t, int n)
{
  while (t < n && ir->compact_instructions[t].op == TCCIR_OP_NOP)
    t++;
  return t;
}

/* Conservative test: does the function provably never return to its caller?
 * Requires no RETURN op, a final unconditional JUMP, and every control transfer
 * landing at or before that back-edge (never the past-end epilogue). */
static int udr_function_provably_noreturn(TCCIRState *ir)
{
  int n = ir->next_instruction_index;

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
  /* Final op other than an unconditional JUMP can fall through to the epilogue. */
  if (ir->compact_instructions[last_idx].op != TCCIR_OP_JUMP)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
    case TCCIR_OP_NOP:
      continue;
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
      return 0;
    /* Unknown / unmodelled control transfers: refuse to claim noreturn. */
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SWITCH_LOAD:
      return 0;
    /* A noreturn callee (exit/abort/...) would make `b .` hang instead of exit. */
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
      if (tcc_ir_callee_is_noreturn(irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q))))
        return 0;
      break;
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    {
      int jt = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (jt < 0)
        return 0;
      jt = udr_nopskip_target(ir, jt, n);
      if (jt >= n || jt > last_idx)
        return 0; /* exits to the epilogue == a reachable return */
      break;
    }
    case TCCIR_OP_SWITCH_TABLE:
    {
      int table_id = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
      if (table_id < 0 || table_id >= ir->num_switch_tables)
        return 0;
      TCCIRSwitchTable *t = &ir->switch_tables[table_id];
      for (int e = 0; e <= t->num_entries; e++)
      {
        int tgt = (e == t->num_entries) ? t->default_target : t->targets[e];
        if (tgt < 0)
          return 0;
        tgt = udr_nopskip_target(ir, tgt, n);
        if (tgt >= n || tgt > last_idx)
          return 0;
      }
      break;
    }
    default:
      break;
    }
  }
  return 1;
}

/* Can any observable side effect reach a function return?
 * Backward fixpoint reaches_ret[i]; reports whether any observable op can reach a
 * RETURN or the epilogue. Effects trapped in a non-returning region don't count.
 * Conservatively returns 1 on any unbounded transfer or unmodelled exotic op. */
static int udr_observable_effect_reaches_return(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  int last_idx = -1;
  for (int i = n - 1; i >= 0; i--)
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
    {
      last_idx = i;
      break;
    }
  if (last_idx < 0)
    return 0;

  /* Bail conservatively on unmodelled control transfers and exotic observable ops. */
  for (int i = 0; i < n; i++)
  {
    switch (ir->compact_instructions[i].op)
    {
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SWITCH_LOAD:
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
      return 1;
    default:
      break;
    }
  }

#define UDR_NOPSKIP(t) ((t) = udr_nopskip_target(ir, (t), n))
#define UDR_RR_GET(k) (reaches_ret[(k) / 8] & (1 << ((k) % 8)))

  uint8_t *reaches_ret = tcc_mallocz((n + 7) / 8);
  int changed = 1;
  while (changed)
  {
    changed = 0;
    for (int i = n - 1; i >= 0; i--)
    {
      if (UDR_RR_GET(i))
        continue;
      IRQuadCompact *q = &ir->compact_instructions[i];
      int r = 0;
      switch (q->op)
      {
      case TCCIR_OP_RETURNVALUE:
      case TCCIR_OP_RETURNVOID:
        r = 1;
        break;
      case TCCIR_OP_JUMP:
      {
        int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
        if (t < 0)
        {
          r = 1; /* malformed target -> conservative */
          break;
        }
        UDR_NOPSKIP(t);
        if (t > last_idx)
          r = 1; /* epilogue == return */
        else if (UDR_RR_GET(t))
          r = 1;
        break;
      }
      case TCCIR_OP_JUMPIF:
      {
        int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
        if (t < 0)
        {
          r = 1;
          break;
        }
        UDR_NOPSKIP(t);
        if (t > last_idx || UDR_RR_GET(t))
          r = 1;
        else
        {
          int f = i + 1;
          UDR_NOPSKIP(f);
          if (f > last_idx || UDR_RR_GET(f))
            r = 1;
        }
        break;
      }
      case TCCIR_OP_SWITCH_TABLE:
      {
        int table_id = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
        if (table_id < 0 || table_id >= ir->num_switch_tables)
        {
          r = 1;
          break;
        }
        TCCIRSwitchTable *tb = &ir->switch_tables[table_id];
        for (int e = 0; e <= tb->num_entries && !r; e++)
        {
          int tgt = (e == tb->num_entries) ? tb->default_target : tb->targets[e];
          if (tgt < 0)
          {
            r = 1;
            break;
          }
          UDR_NOPSKIP(tgt);
          if (tgt > last_idx || UDR_RR_GET(tgt))
            r = 1;
        }
        break;
      }
      default:
      {
        /* Fall-through op (NOP, arithmetic, load/store, call, ...). */
        int t = i + 1;
        UDR_NOPSKIP(t);
        if (t > last_idx || UDR_RR_GET(t))
          r = 1;
        break;
      }
      }
      if (r)
      {
        reaches_ret[i / 8] |= (uint8_t)(1 << (i % 8));
        changed = 1;
      }
    }
  }

  int result = 0;
  for (int i = 0; i < n && !result; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int observable = 0;
    switch (q->op)
    {
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_CALLSEQ_BEGIN:
    case TCCIR_OP_CALLARG_REG:
    case TCCIR_OP_CALLARG_STACK:
    case TCCIR_OP_CALLSEQ_END:
      observable = 1;
      break;
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_BLOCK_COPY:
      observable = udr_store_is_observable(tcc_ir_op_get_dest(ir, q));
      break;
    default:
      break;
    }
    if (!observable)
      observable = udr_instr_has_volatile_operand(ir, q);
    if (observable && UDR_RR_GET(i))
      result = 1;
  }

#undef UDR_NOPSKIP
#undef UDR_RR_GET
  tcc_free(reaches_ret);
  return result;
}

/* Mark every VAR whose address is taken (address-of or LEA) — pointer writes may
 * initialize it invisibly, so the passes conservatively exclude it. */
static void udr_prescan_addr_taken(TCCIRState *ir, uint8_t *addr_taken, int max_pos)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (!udr_get_operand(ir, q, k, &op))
        continue;
      int32_t vr = irop_get_vreg(op);
      if (vr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      if (op.is_local && !op.is_lval)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos >= 0 && pos < max_pos)
          addr_taken[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
    if (q->op == TCCIR_OP_LEA && irop_config[q->op].has_src1)
    {
      IROperand op = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos >= 0 && pos < max_pos)
          addr_taken[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
  }
}

/* Unconditional uninitialized-local UB exploit: if the entry block reads a VAR
 * before writing it, that read is C11 UB; collapse the body to `b .` (matching
 * GCC on gcc.c-torture/compile/931102-1.c). The scan stops at the first
 * branch/call/return or later jump-target, so conditional reads don't trigger. */
int tcc_ir_opt_uninit_local_ub(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  /* O2-only: UB exploitation is too aggressive for lower levels. */
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  /* Inline asm operands aren't modeled and IJUMP targets are unknown: bail. */
  if (udr_has_inline_asm_or_ijump(ir))
    return 0;

#define UNINIT_MAX_VAR_POS 1024
  uint8_t written[(UNINIT_MAX_VAR_POS + 7) / 8] = {0};
  uint8_t addr_taken[(UNINIT_MAX_VAR_POS + 7) / 8] = {0};
  udr_prescan_addr_taken(ir, addr_taken, UNINIT_MAX_VAR_POS);

  int found_uninit = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* Subsequent jump targets end the entry block. */
    if (i > 0 && q->is_jump_target)
      break;

    /* Check src1 / src2 for read of an unwritten VAR. */
    for (int k = 1; k <= 2 && !found_uninit; k++)
    {
      IROperand sop;
      if (!udr_get_operand(ir, q, k, &sop))
        continue;
      int32_t svr = irop_get_vreg(sop);
      if (svr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
        continue;
      /* Pure address-of (is_local && !is_lval) is not a value read. */
      if (sop.is_local && !sop.is_lval)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(svr);
      if (pos < 0 || pos >= UNINIT_MAX_VAR_POS)
        continue;
      /* Skip address-taken VARs — pointer writes may have initialized them. */
      if (addr_taken[pos >> 3] & (uint8_t)(1u << (pos & 7)))
        continue;
      if (!(written[pos >> 3] & (uint8_t)(1u << (pos & 7))))
      {
        found_uninit = 1;
        break;
      }
    }
    if (found_uninit)
      break;

    /* Apply this op's WRITE after the read check (program order). */
    if (irop_config[q->op].has_dest)
    {
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dop);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (pos >= 0 && pos < UNINIT_MAX_VAR_POS)
          written[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }

    /* Entry-block terminators end the scan (a CALL may init escaped VARs). */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
      break;
  }
#undef UNINIT_MAX_VAR_POS

  if (!found_uninit)
    return 0;

  /* Keep observable work only when it can actually reach a return; a function
   * that never returns (or whose effects are all trapped in dead infinite loops)
   * collapses to `b .` without losing terminating behavior. */
  if (udr_has_observable_side_effects(ir) && !udr_function_provably_noreturn(ir) &&
      udr_observable_effect_reaches_return(ir))
    return 0;

  LOG_IR_GEN("UNINIT-UB: collapsing function body to infinite loop (read of uninit local in entry block)");

  /* Replace the whole IR with a single self-jump; codegen emits `b .`. */
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

  ir->leaffunc = 1;

  return 1;
}

int tcc_ir_opt_uninit_local_ub_ex(IROptCtx *ctx) { return tcc_ir_opt_uninit_local_ub(ctx->ir); }

/* Uninit-read-dominates-return — extends uninit_local_ub from "entry block" to
 * any read of an uninit local that dominates every RETURN. Such a read makes
 * every returning execution UB, so collapse to `b .` (matching GCC -O2).
 * "Uninit" is approximated by a linear-order pre-scan: first read before any
 * write. Sound but misses conditional-uninit; address-taken VARs are excluded. */
int tcc_ir_opt_uninit_dominates_return(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  /* Inline asm / computed goto: same conservative bail as uninit_local_ub. */
  if (udr_has_inline_asm_or_ijump(ir))
    return 0;

  /* Require an explicit or implicit return (JUMP/JUMPIF past-end == epilogue). */
  int has_return = 0;
  int has_implicit_return = 0;
  for (int i = 0; i < n; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_RETURNVALUE || op == TCCIR_OP_RETURNVOID)
    {
      has_return = 1;
      break;
    }
    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF)
    {
      IROperand d = tcc_ir_op_get_dest(ir, &ir->compact_instructions[i]);
      int t = (int)irop_get_imm64_ex(ir, d);
      if (t >= n)
        has_implicit_return = 1;
    }
  }
  if (!has_return && !has_implicit_return)
    return 0;

  /* Don't exploit the UB when the function does observable work before return. */
  if (udr_has_observable_side_effects(ir))
    return 0;

#define UDR_MAX_VAR_POS 1024
  uint8_t written[(UDR_MAX_VAR_POS + 7) / 8] = {0};
  uint8_t addr_taken[(UDR_MAX_VAR_POS + 7) / 8] = {0};
  udr_prescan_addr_taken(ir, addr_taken, UDR_MAX_VAR_POS);

  /* Linear scan: find the FIRST instruction reading a VAR not yet written. */
  int uninit_read_idx = -1;
  for (int i = 0; i < n && uninit_read_idx < 0; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    for (int k = 1; k <= 2; k++)
    {
      IROperand sop;
      if (!udr_get_operand(ir, q, k, &sop))
        continue;
      int32_t svr = irop_get_vreg(sop);
      if (svr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
        continue;
      if (sop.is_local && !sop.is_lval)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(svr);
      if (pos < 0 || pos >= UDR_MAX_VAR_POS)
        continue;
      if (addr_taken[pos >> 3] & (uint8_t)(1u << (pos & 7)))
        continue;
      if (!(written[pos >> 3] & (uint8_t)(1u << (pos & 7))))
      {
        uninit_read_idx = i;
        break;
      }
    }

    if (irop_config[q->op].has_dest)
    {
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dop);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (pos >= 0 && pos < UDR_MAX_VAR_POS)
          written[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
  }
#undef UDR_MAX_VAR_POS

  if (uninit_read_idx < 0)
    return 0;

  /* Build CFG + dominators and verify the uninit read dominates every RETURN. */
  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg || cfg->num_blocks == 0)
  {
    if (cfg)
      tcc_ir_cfg_free(cfg);
    return 0;
  }
  tcc_ir_cfg_compute_dominators(cfg);

  int read_block = cfg->instr_to_block[uninit_read_idx];
  int ok = 1;
  for (int i = 0; i < n && ok; i++)
  {
    IRQuadCompact *rq = &ir->compact_instructions[i];
    TccIrOp op = rq->op;
    int is_ret = (op == TCCIR_OP_RETURNVALUE || op == TCCIR_OP_RETURNVOID);
    int is_implicit_ret = 0;
    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF)
    {
      IROperand d = tcc_ir_op_get_dest(ir, rq);
      int t = (int)irop_get_imm64_ex(ir, d);
      if (t >= n)
        is_implicit_ret = 1;
    }
    if (!is_ret && !is_implicit_ret)
      continue;
    int ret_block = cfg->instr_to_block[i];
    if (read_block == ret_block)
    {
      /* Same block: read must come before the RETURN in linear order. */
      if (uninit_read_idx >= i)
        ok = 0;
    }
    else if (!tcc_ir_cfg_dominates(cfg, read_block, ret_block))
    {
      ok = 0;
    }
  }
  tcc_ir_cfg_free(cfg);

  if (!ok)
    return 0;

  LOG_IR_GEN("UNINIT-DOM-RETURN: collapsing function body to infinite loop "
             "(uninit VAR read at i=%d dominates all RETURNs)", uninit_read_idx);

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
  if (tcc_state && tcc_state->cur_func_sym && tcc_state->cur_func_sym->type.ref)
    tcc_state->cur_func_sym->type.ref->f.func_noreturn = 1;

  return 1;
}

int tcc_ir_opt_uninit_dominates_return_ex(IROptCtx *ctx) { return tcc_ir_opt_uninit_dominates_return(ctx->ir); }

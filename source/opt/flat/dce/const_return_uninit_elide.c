/*
 *  TCC IR - Const-Return Uninit-Elide Pass
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

static int crue_offset_present(const int *offs, int n, int off)
{
  for (int j = 0; j < n; j++)
    if (offs[j] == off)
      return 1;
  return 0;
}

static void crue_mark_var(uint8_t *bits, int32_t vr, int maxpos)
{
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (pos >= 0 && pos < maxpos)
    bits[pos >> 3] |= (uint8_t)(1u << (pos & 7));
}

int tcc_ir_opt_const_return_uninit_elide(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;
  if (ir->has_static_chain)
    return 0;

#define CRUE_MAX_STORE_OFFSETS 512
#define CRUE_MAX_VAR_POS 1024
  int store_offsets[CRUE_MAX_STORE_OFFSETS];
  int n_store_offsets = 0;
  /* VARs whose address is taken: a pointer write may have initialised them, so a read isn't provably uninit. */
  uint8_t var_addr_taken[(CRUE_MAX_VAR_POS + 7) / 8] = {0};

  IROperand rv_src = IROP_NONE;
  int rv_count = 0;
  int64_t rv_val = 0;
  int rv_const_tag = 0;
  int rv_btype = 0;

  /* Pass 1: hard-bail scan, inventory STORE dest offsets, validate RETURNVALUE source, detect stack-address escapes. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_INIT_CHAIN_SLOT:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
    case TCCIR_OP_RETURNVOID:
      return 0;
    case TCCIR_OP_RETURNVALUE:
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int tag = irop_get_tag(s);
      if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_I64)
        return 0;
      int64_t v = irop_get_imm64_ex(ir, s);
      if (rv_count == 0)
      {
        rv_val = v;
        rv_btype = s.btype;
        rv_const_tag = tag;
        rv_src = s;
      }
      else if (v != rv_val || tag != rv_const_tag || s.btype != rv_btype)
        return 0;
      rv_count++;
      break;
    }
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    {
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      if (irop_get_tag(dop) == IROP_TAG_STACKOFF && dop.is_local && !dop.is_param)
      {
        int off = (int)irop_get_stack_offset(dop);
        if (n_store_offsets >= CRUE_MAX_STORE_OFFSETS)
          return 0;
        store_offsets[n_store_offsets++] = off;
      }
      break;
    }
    default:
      break;
    }

    /* Any lval local STACKOFF dest (not just STORE) initialises the slot; record its offset. */
    if (irop_config[q->op].has_dest)
    {
      IROperand wdop = tcc_ir_op_get_dest(ir, q);
      if (irop_get_tag(wdop) == IROP_TAG_STACKOFF && wdop.is_lval && wdop.is_local && !wdop.is_param)
      {
        int woff = (int)irop_get_stack_offset(wdop);
        if (!crue_offset_present(store_offsets, n_store_offsets, woff))
        {
          if (n_store_offsets >= CRUE_MAX_STORE_OFFSETS)
            return 0;
          store_offsets[n_store_offsets++] = woff;
        }
      }
    }

    /* Bail on Addr[StackLoc] (stack address escapes) and volatile sym access. */
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
      if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && !op.is_lval && !op.is_param)
        return 0;
      if (op.is_sym)
      {
        Sym *sym = irop_get_sym_ex(ir, op);
        if (sym && (sym->type.t & VT_VOLATILE))
          return 0;
      }
      /* Address-of a VAR-vreg local: exclude from the uninit-VAR check (pointer alias may write it). */
      {
        int32_t vr = irop_get_vreg(op);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && op.is_local && !op.is_lval)
          crue_mark_var(var_addr_taken, vr, CRUE_MAX_VAR_POS);
      }
    }

    /* LEA with a STACKOFF source materialises a stack address — same risk as Addr[StackLoc]. */
    if (q->op == TCCIR_OP_LEA && irop_config[q->op].has_src1)
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (irop_get_tag(s) == IROP_TAG_STACKOFF)
        return 0;
      int32_t vr = irop_get_vreg(s);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        crue_mark_var(var_addr_taken, vr, CRUE_MAX_VAR_POS);
    }
  }

  if (rv_count == 0)
    return 0;

  /* Pass 2: scan the entry-block prefix for the first uninit STACKOFF/VAR read vs. first observable side effect.
   * Jump targets do NOT stop the scan: on the first loop iteration the header is reached by fall-through. */
  int found_uninit = 0;
  /* VARs written by an earlier entry-block instruction, in program order. */
  uint8_t var_written[(CRUE_MAX_VAR_POS + 7) / 8] = {0};
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Check src1/src2 for uninit STACKOFF read. */
    for (int k = 1; k <= 2 && !found_uninit; k++)
    {
      if (k == 1 && !irop_config[q->op].has_src1)
        continue;
      if (k == 2 && !irop_config[q->op].has_src2)
        continue;
      IROperand sop = (k == 1) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      if (irop_get_tag(sop) != IROP_TAG_STACKOFF)
        continue;
      if (!sop.is_lval)
        continue;
      if (!sop.is_local)
        continue;
      if (sop.is_param)
        continue;
      /* Spilled vregs (vreg >= 0) carry a defined value not visible as an IR STORE; only raw frontend STACKOFFs (vreg == -1) can be uninit. */
      if (irop_has_vreg(sop) && irop_get_vreg(sop) >= 0)
        continue;
      int off = (int)irop_get_stack_offset(sop);
      if (!crue_offset_present(store_offsets, n_store_offsets, off))
      {
        found_uninit = 1;
        break;
      }
    }

    /* Uninit VAR-vreg read: never-address-taken local kept in a vreg, read before any write to it. */
    for (int k = 1; k <= 2 && !found_uninit; k++)
    {
      if (k == 1 && !irop_config[q->op].has_src1)
        continue;
      if (k == 2 && !irop_config[q->op].has_src2)
        continue;
      IROperand sop = (k == 1) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      int32_t svr = irop_get_vreg(sop);
      if (svr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
        continue;
      /* Pure address-of (is_local && !is_lval) is not a value read. */
      if (sop.is_local && !sop.is_lval)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(svr);
      if (pos < 0 || pos >= CRUE_MAX_VAR_POS)
        continue;
      if (var_addr_taken[pos >> 3] & (uint8_t)(1u << (pos & 7)))
        continue;
      if (!(var_written[pos >> 3] & (uint8_t)(1u << (pos & 7))))
      {
        found_uninit = 1;
        break;
      }
    }

    /* Observable op before any uninit read would lose the effect — bail. */
    if (!found_uninit)
    {
      switch (q->op)
      {
      case TCCIR_OP_STORE:
      case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_STORE_POSTINC:
      case TCCIR_OP_FUNCCALLVAL:
      case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_FUNCPARAMVAL:
      case TCCIR_OP_FUNCPARAMVOID:
      case TCCIR_OP_CALLSEQ_BEGIN:
      case TCCIR_OP_CALLARG_REG:
      case TCCIR_OP_CALLARG_STACK:
      case TCCIR_OP_CALLSEQ_END:
      case TCCIR_OP_BLOCK_COPY:
      case TCCIR_OP_PREFETCH:
        return 0;
      default:
        break;
      }
    }

    /* Record this instruction's VAR-vreg write (after the read check, program order). */
    if (!found_uninit && irop_config[q->op].has_dest)
    {
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dop);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
        crue_mark_var(var_written, dvr, CRUE_MAX_VAR_POS);
    }

    /* Entry-block terminators. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_RETURNVALUE ||
        q->op == TCCIR_OP_RETURNVOID)
      break;
  }

#undef CRUE_MAX_STORE_OFFSETS
#undef CRUE_MAX_VAR_POS

  if (!found_uninit)
    return 0;

  LOG_IR_GEN("CONST-RETURN-UNINIT-ELIDE: collapsing function to a single "
             "RETURNVALUE constant (entry-block UB read poisons all paths; "
             "every RETURNVALUE returns the same constant)");

  /* NOP everything, then place a single RETURNVALUE-const at index 0. */
  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->compact_instructions[0].op = TCCIR_OP_RETURNVALUE;
  tcc_ir_set_dest(ir, 0, IROP_NONE);
  tcc_ir_set_src1(ir, 0, rv_src);
  tcc_ir_set_src2(ir, 0, IROP_NONE);

  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;

  /* Clear param allocations: the body no longer references any param. */
  for (int p = 0; p < ir->next_parameter; p++)
  {
    IRLiveInterval *iv = &ir->parameters_live_intervals[p];
    iv->allocation.r0 = PREG_NONE;
    iv->allocation.r1 = PREG_NONE;
    iv->allocation.offset = 0;
  }

  return 1;
}

int tcc_ir_opt_const_return_uninit_elide_ex(IROptCtx *ctx) { return tcc_ir_opt_const_return_uninit_elide(ctx->ir); }

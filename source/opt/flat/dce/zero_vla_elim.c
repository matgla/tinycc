/*
 *  TCC IR - Zero-size VLA elimination
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

/* Zero-Size VLA Elimination — drop VLA alloc/save/restore when the VLA is empty. */
int tcc_ir_opt_zero_vla_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
        op == TCCIR_OP_INLINE_ASM)
      return 0;
  }

  int changed = 0;
  int eliminated_any = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_VLA_ALLOC)
      continue;

    IROperand size_op = tcc_ir_op_get_src1(ir, q);
    int size_is_zero = 0;

    if (irop_is_immediate(size_op) && irop_get_imm64_ex(ir, size_op) == 0)
    {
      size_is_zero = 1;
    }
    else if (irop_get_tag(size_op) == IROP_TAG_STACKOFF)
    {
      int32_t slot = irop_get_stack_offset(size_op);
      for (int j = i - 1; j >= 0; j--)
      {
        IRQuadCompact *qj = &ir->compact_instructions[j];
        TccIrOp jop = qj->op;
        if (jop == TCCIR_OP_NOP)
          continue;

        if (jop == TCCIR_OP_STORE)
        {
          IROperand dest = tcc_ir_op_get_dest(ir, qj);
          if (irop_get_tag(dest) == IROP_TAG_STACKOFF &&
              irop_get_stack_offset(dest) == slot)
          {
            IROperand src = tcc_ir_op_get_src1(ir, qj);
            if (irop_is_immediate(src) && irop_get_imm64_ex(ir, src) == 0)
              size_is_zero = 1;
            break;
          }
        }

        /* Calls / indirect stores / block ops could clobber the slot via
         * aliasing — stop the backward scan conservatively. */
        if (jop == TCCIR_OP_FUNCCALLVOID || jop == TCCIR_OP_FUNCCALLVAL ||
            jop == TCCIR_OP_STORE_INDEXED || jop == TCCIR_OP_BLOCK_COPY)
          break;
      }
    }

    if (!size_is_zero)
      continue;

    /* Convert VLA_ALLOC to ASSIGN(#0).  VLA_ALLOC has no dest in its op
     * config — converting to ASSIGN (which has dest, src1) requires the
     * operand pool to provide a dest slot.  We rely on the pool layout to
     * have an entry at operand_base[0]; for VLA_ALLOC that slot is unused.
     *
     * To stay safe, just NOP the VLA_ALLOC.  The codegen will then skip
     * any SP adjustment for it.  Consumers that store its (now-undefined)
     * result are dead-store-eliminated downstream since their dest slots
     * are never read in the zero-size case. */
    q->op = TCCIR_OP_NOP;
    changed = 1;
    eliminated_any = 1;
  }

  /* The SP_SAVE/RESTORE cleanup below is safe to run independently of whether
   * we just eliminated a VLA_ALLOC: after a previous pass call NOPed the
   * VLA_ALLOC, later passes (e.g. dead_lea_store) may have cleaned up the
   * LEAs that read the VLA's address slot, leaving a now-dead lone SP_SAVE
   * we couldn't see on the first call. */
  (void)eliminated_any;

  /* Helper: does any op in the function read the slot at `slot`? */
#define SLOT_USED_BY(_op, _is_read)                                                                                    \
  ({                                                                                                                   \
    IROperand _s1 = tcc_ir_op_get_src1(ir, (_op));                                                                      \
    IROperand _s2 = tcc_ir_op_get_src2(ir, (_op));                                                                      \
    int _used = 0;                                                                                                     \
    if (irop_get_tag(_s1) == IROP_TAG_STACKOFF && irop_get_stack_offset(_s1) == slot)                                  \
      _used = 1;                                                                                                       \
    if (irop_get_tag(_s2) == IROP_TAG_STACKOFF && irop_get_stack_offset(_s2) == slot)                                  \
      _used = 1;                                                                                                       \
    if (!(_is_read))                                                                                                   \
    {                                                                                                                  \
      IROperand _d = tcc_ir_op_get_dest(ir, (_op));                                                                    \
      if (irop_get_tag(_d) == IROP_TAG_STACKOFF && irop_get_stack_offset(_d) == slot)                                  \
        _used = 1;                                                                                                     \
    }                                                                                                                  \
    _used;                                                                                                             \
  })

  /* NOP redundant VLA_SP_SAVE / VLA_SP_RESTORE pairs whose enclosed region
   * no longer contains any SP-changing op.  Also NOP lone VLA_SP_SAVEs whose
   * dest slot is never read anywhere (the captured SP isn't used). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_VLA_SP_SAVE)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_tag(dest) != IROP_TAG_STACKOFF)
      continue;
    int32_t slot = irop_get_stack_offset(dest);

    /* Scan the whole function for uses of this slot. */
    int restore_idx = -1;
    int other_reader = 0;
    int other_writer = 0;
    for (int j = 0; j < n; j++)
    {
      if (j == i)
        continue;
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;

      if (qj->op == TCCIR_OP_VLA_SP_RESTORE)
      {
        IROperand src = tcc_ir_op_get_src1(ir, qj);
        if (irop_get_tag(src) == IROP_TAG_STACKOFF &&
            irop_get_stack_offset(src) == slot)
        {
          if (restore_idx >= 0)
          {
            other_reader = 1;
            break;
          }
          restore_idx = j;
        }
      }
      else if (qj->op == TCCIR_OP_VLA_SP_SAVE)
      {
        IROperand d = tcc_ir_op_get_dest(ir, qj);
        if (irop_get_tag(d) == IROP_TAG_STACKOFF &&
            irop_get_stack_offset(d) == slot)
          other_writer = 1;
      }
      else
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, qj);
        IROperand s2 = tcc_ir_op_get_src2(ir, qj);
        IROperand d = tcc_ir_op_get_dest(ir, qj);
        if ((irop_get_tag(s1) == IROP_TAG_STACKOFF &&
             irop_get_stack_offset(s1) == slot) ||
            (irop_get_tag(s2) == IROP_TAG_STACKOFF &&
             irop_get_stack_offset(s2) == slot))
          other_reader = 1;
        /* MLA's accumulator (4th operand) is a source not surfaced by the
         * src1/src2 helpers — a VLA base read only as an MLA addend would
         * otherwise look unused, so we'd wrongly NOP its capturing SP_SAVE. */
        if (qj->op == TCCIR_OP_MLA)
        {
          IROperand acc = tcc_ir_op_get_accum(ir, qj);
          if (irop_get_tag(acc) == IROP_TAG_STACKOFF &&
              irop_get_stack_offset(acc) == slot)
            other_reader = 1;
        }
        if (irop_get_tag(d) == IROP_TAG_STACKOFF &&
            irop_get_stack_offset(d) == slot)
          other_writer = 1;
      }
    }

    if (other_reader || other_writer)
      continue;

    if (restore_idx < 0)
    {
      /* Lone SP_SAVE with no reader anywhere — pure dead store. */
      ir->compact_instructions[i].op = TCCIR_OP_NOP;
      changed = 1;
      continue;
    }

    /* Paired SAVE/RESTORE — require no SP-changing op between them. */
    int sp_changed = 0;
    for (int j = i + 1; j < restore_idx; j++)
    {
      TccIrOp jop = ir->compact_instructions[j].op;
      if (jop == TCCIR_OP_VLA_ALLOC)
      {
        sp_changed = 1;
        break;
      }
    }
    if (sp_changed)
      continue;

    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[restore_idx].op = TCCIR_OP_NOP;
    changed = 1;
  }

#undef SLOT_USED_BY

  return changed;
}

int tcc_ir_opt_zero_vla_elim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_zero_vla_elim(ctx->ir);
}

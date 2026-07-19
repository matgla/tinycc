/*
 *  TCC IR - ADD/SUB constant reassociation (flat, pre-SSA)
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

/* Address-taken local VAR (appears as a LEA src1): mutable via an aliasing store, so its def-value may not hold at a later use. */
static int ir_reassoc_var_addr_taken(TCCIRState *ir, int32_t vreg)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_VAR)
    return 0;
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LEA)
      continue;
    if (irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vreg)
      return 1;
  }
  return 0;
}

/* Collapse constant ADD/SUB chains — ADD(ADD(base,c1),c2) -> ADD(base,c1+c2) — so downstream CMP identity folding sees two "base + N" values as identical. */
int tcc_ir_opt_add_reassoc(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2 || n > 4000)
    return 0;

  uint8_t *is_merge = ir_opt_build_merge_bitmap(ir, n);
  int dc_stride = 0;
  uint8_t *dc = ir_opt_build_def_count(ir, n, &dc_stride);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    if (!irop_is_immediate(src2))
      continue;

    /* Bail on real memory derefs only; register-promoted locals/llocals carry is_lval as a tag but read from a register. */
    if (src1.is_lval && !src1.is_const && !src1.is_local && !src1.is_llocal)
      continue;

    /* Direct case: src1 is a symref-by-value (symref-prop folded the vreg into a sym operand) — combine the immediate into the addend. */
    if (src1.is_sym && !src1.is_lval)
    {
      IRPoolSymref *sref = irop_get_symref_ex(ir, src1);
      if (!sref || !sref->sym)
        continue;
      int64_t c2_d = irop_get_imm64_ex(ir, src2);
      int64_t eff_c2_d = (q->op == TCCIR_OP_SUB) ? -c2_d : c2_d;
      int64_t new_addend_d = (int64_t)sref->addend + eff_c2_d;
      if (new_addend_d != (int32_t)new_addend_d)
        continue;
      Sym *target_sym = sref->sym;
      uint32_t sref_flags = sref->flags;
      int btype_d = irop_get_btype(src1);
      uint8_t local_d = src1.is_local;
      uint8_t const_d = src1.is_const;
      uint8_t uns_d = src1.is_unsigned;
      uint32_t pool_idx_d = tcc_ir_pool_add_symref(ir, target_sym, (int32_t)new_addend_d, sref_flags);
      IROperand new_src_d = irop_make_symref(-1, pool_idx_d, 0, local_d, const_d, btype_d);
      new_src_d.is_unsigned = uns_d;
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, new_src_d);
      changes++;
      continue;
    }

    int32_t src1_vr = irop_get_vreg(src1);
    if (src1_vr < 0)
      continue;

    int def_idx = tcc_ir_find_defining_instruction(ir, src1_vr, i);
    if (def_idx < 0)
      continue;

    /* def_idx..i must be one straight-line block (bail on any merge or intervening JUMP/RETURN): the linear def lookup can otherwise cross a single-pred block boundary and pick a def that never reaches i. */
    {
      int safe = 1;
      for (int j = def_idx + 1; j <= i; j++)
      {
        if (is_merge[j / 8] & (1 << (j % 8)))
        {
          safe = 0;
          break;
        }
        if (j > 0)
        {
          int prev_op = ir->compact_instructions[j - 1].op;
          if (prev_op == TCCIR_OP_JUMP || prev_op == TCCIR_OP_RETURNVALUE ||
              prev_op == TCCIR_OP_RETURNVOID)
          {
            safe = 0;
            break;
          }
        }
      }
      if (!safe)
        continue;
    }

    IRQuadCompact *def_q = &ir->compact_instructions[def_idx];

    /* def is ADD/SUB-imm (chain case), or an ASSIGN of a symref-by-value (collapse ADD(T,imm) to ASSIGN T2 = &S+addend+imm). */
    int def_is_assign_symref = 0;
    IROperand def_src1;
    int64_t eff_c1 = 0;

    if (def_q->op == TCCIR_OP_ADD || def_q->op == TCCIR_OP_SUB)
    {
      IROperand def_src2 = tcc_ir_op_get_src2(ir, def_q);
      if (!irop_is_immediate(def_src2))
        continue;
      def_src1 = tcc_ir_op_get_src1(ir, def_q);
      int64_t c1 = irop_get_imm64_ex(ir, def_src2);
      eff_c1 = (def_q->op == TCCIR_OP_SUB) ? -c1 : c1;
    }
    else if (def_q->op == TCCIR_OP_ASSIGN)
    {
      def_src1 = tcc_ir_op_get_src1(ir, def_q);
      if (!def_src1.is_sym || def_src1.is_lval)
        continue;
      def_is_assign_symref = 1;
      eff_c1 = 0;
    }
    else
    {
      continue;
    }

    /* Reject forwarding a real memory deref def_src1 (is_lval, non-local/llocal): an intervening STORE/CALL may have changed it. is_const is deliberately excluded — a global symref deref is is_const (address) but its value changes across stores. */
    if (def_src1.is_lval && !def_src1.is_local && !def_src1.is_llocal)
      continue;

    /* def_src1 must not be redefined in [def_idx, i) — incl. def_q itself for self-update chains like V0 = V0 + 200. */
    int32_t inner_vr = irop_get_vreg(def_src1);
    if (inner_vr >= 0 && !DC_IS_SINGLE_DEF(dc, dc_stride, inner_vr))
    {
      int redefined = 0;
      for (int j = def_idx; j < i; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op == TCCIR_OP_NOP)
          continue;
        IROperand jdst = tcc_ir_op_get_dest(ir, jq);
        if (irop_get_vreg(jdst) == inner_vr)
        {
          redefined = 1;
          break;
        }
      }
      if (redefined)
        continue;
    }

    /* The linear def lookup is blind to aliasing stores: bail if a memory-clobbering op sits in the gap and either base is an address-taken VAR (ptr fuzz 85636) or def_src1 is a raw stack-slot load with no vreg (ptr fuzz 409667). */
    {
      int gap_clobbers_memory = 0;
      for (int j = def_idx + 1; j < i && !gap_clobbers_memory; j++)
      {
        switch (ir->compact_instructions[j].op)
        {
        case TCCIR_OP_STORE:
        case TCCIR_OP_STORE_INDEXED:
        case TCCIR_OP_STORE_POSTINC:
        case TCCIR_OP_BLOCK_COPY:
        case TCCIR_OP_FUNCCALLVAL:
        case TCCIR_OP_FUNCCALLVOID:
        case TCCIR_OP_INLINE_ASM:
          gap_clobbers_memory = 1;
          break;
        default:
          break;
        }
      }
      if (gap_clobbers_memory &&
          (ir_reassoc_var_addr_taken(ir, src1_vr) ||
           (inner_vr >= 0 && ir_reassoc_var_addr_taken(ir, inner_vr)) ||
           (inner_vr < 0 && def_src1.is_lval)))
        continue;
    }

    int64_t c2 = irop_get_imm64_ex(ir, src2);
    int64_t eff_c2 = (q->op == TCCIR_OP_SUB) ? -c2 : c2;
    int64_t combined = eff_c1 + eff_c2;

    if (combined != (int32_t)combined)
      continue;

    int btype = irop_get_btype(src2);
    LOG_IR_GEN("OPTIMIZE: ADD reassoc at i=%d: (%lld) + (%lld) = %lld",
               i, (long long)eff_c1, (long long)eff_c2, (long long)combined);

    if (def_is_assign_symref)
    {
      /* Fold T=symref(S,+A); T2=T±imm into T2=symref(S,+A±imm). */
      IRPoolSymref *sref = irop_get_symref_ex(ir, def_src1);
      if (!sref || !sref->sym)
        continue;
      int64_t new_addend = (int64_t)sref->addend + combined;
      if (new_addend != (int32_t)new_addend)
        continue;
      uint32_t pool_idx = tcc_ir_pool_add_symref(ir, sref->sym, (int32_t)new_addend, sref->flags);
      IROperand new_src = irop_make_symref(-1, pool_idx, 0, def_src1.is_local, def_src1.is_const,
                                           irop_get_btype(def_src1));
      new_src.is_unsigned = def_src1.is_unsigned;
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, new_src);
      tcc_ir_set_src2(ir, i, IROP_NONE);
    }
    else if (combined == 0)
    {
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, def_src1);
      tcc_ir_set_src2(ir, i, IROP_NONE);
    }
    else
    {
      q->op = TCCIR_OP_ADD;
      tcc_ir_set_src1(ir, i, def_src1);
      tcc_ir_set_src2(ir, i, irop_make_imm32(-1, (int32_t)combined, btype));
    }
    changes++;
  }

  tcc_free(dc);
  tcc_free(is_merge);
  return changes;
}

int tcc_ir_opt_add_reassoc_ex(IROptCtx *ctx) { return tcc_ir_opt_add_reassoc(ctx->ir); }

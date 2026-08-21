/*
 *  TCC IR - Stack-address simplify + compare fold (flat pass)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Relocated flat pass, Branch A [A*]; see docs/plan_legacy_flat_ir_ssa_retire.md */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

typedef struct StackAddrValue
{
  int off;
  int is_param;
} StackAddrValue;

static int ir_resolve_stack_addr_value_ex(TCCIRState *ir, IROperand op, int at_idx,
                                          StackAddrValue *out, int depth);

static int ir_has_backward_control_flow(TCCIRState *ir)
{
  int n = ir ? ir->next_instruction_index : 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (target >= 0 && target <= i)
        return 1;
    }
    else if (q->op == TCCIR_OP_IJUMP)
    {
      return 1;
    }
  }

  return 0;
}

/* Resolve an operand at `at_idx` to a constant stack-frame offset, if provable. */
static int ir_resolve_stack_addr_value(TCCIRState *ir, IROperand op, int at_idx, int *out_off)
{
  StackAddrValue value;
  if (!ir_resolve_stack_addr_value_ex(ir, op, at_idx, &value, 0))
    return 0;
  *out_off = value.off;
  return 1;
}

/* Presence map of vregs with at least one def; indexed pos*3 + (type-1).
 * sav_def_only records the index of that def while the count is exactly one,
 * and -2 once a second def appears. */
static uint8_t *sav_def_present;
static int *sav_def_only;
static int sav_def_present_maxpos = -1;

static int sav_is_def_op(int op)
{
  return op != TCCIR_OP_STORE && op != TCCIR_OP_STORE_INDEXED &&
         op != TCCIR_OP_STORE_POSTINC && op != TCCIR_OP_FUNCPARAMVAL;
}

static void sav_build_def_map(TCCIRState *ir)
{
  int n = ir ? ir->next_instruction_index : 0;
  sav_def_present = NULL;
  sav_def_present_maxpos = -1;
  int maxpos = -1;
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest || !sav_is_def_op(q->op))
      continue;
    int32_t dvr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (dvr < 0)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (pos > maxpos)
      maxpos = pos;
  }
  if (maxpos < 0)
    return;
  sav_def_present = (uint8_t *)tcc_mallocz((size_t)(maxpos + 1) * 3);
  sav_def_only = (int *)tcc_malloc((size_t)(maxpos + 1) * 3 * sizeof(int));
  for (int k = 0; k < (maxpos + 1) * 3; k++)
    sav_def_only[k] = -1;
  sav_def_present_maxpos = maxpos;
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest || !sav_is_def_op(q->op))
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(d);
    if (dvr < 0)
      continue;
    int type = TCCIR_DECODE_VREG_TYPE(dvr);
    int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (type < 1 || type > 3)
      continue;
    int k = pos * 3 + (type - 1);
    sav_def_present[k] = 1;
    /* A write THROUGH the vreg is a use, but so is `has_dest` with is_lval --
     * it is not a def of the vreg, and counting it as one would let a second
     * def slip past the uniqueness test. */
    sav_def_only[k] = d.is_lval ? -2 : (sav_def_only[k] == -1 ? j : -2);
  }
}

static void sav_free_def_map(void)
{
  tcc_free(sav_def_present);
  tcc_free(sav_def_only);
  sav_def_present = NULL;
  sav_def_only = NULL;
  sav_def_present_maxpos = -1;
}

/* Index of vr's single def in the function, or -1 when there is not exactly one. */
static int sav_vreg_unique_def(int32_t vr)
{
  if (!sav_def_only)
    return -1;
  int type = TCCIR_DECODE_VREG_TYPE(vr);
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (type < 1 || type > 3 || pos > sav_def_present_maxpos)
    return -1;
  int only = sav_def_only[pos * 3 + (type - 1)];
  return only >= 0 ? only : -1;
}

/* Returns 1 if vr definitely has no qualifying def (walk would return 0). */
static int sav_vreg_has_no_def(int32_t vr)
{
  if (!sav_def_present)
    return 0; /* map not built — be safe, let the walk run */
  int type = TCCIR_DECODE_VREG_TYPE(vr);
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (type < 1 || type > 3 || pos > sav_def_present_maxpos)
    return 1; /* outside any recorded def */
  return sav_def_present[pos * 3 + (type - 1)] == 0;
}

static int ir_resolve_stack_addr_value_ex(TCCIRState *ir, IROperand op, int at_idx,
                                          StackAddrValue *out, int depth)
{
  if (!ir || !out || depth > 12)
    return 0;

  /* Direct stack address (Addr[StackLoc[X]], i.e. STACKOFF tag, no vreg, not lval). */
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_get_vreg(op) == -1 && !op.is_lval)
  {
    out->off = (int)irop_get_imm64_ex(ir, op);
    out->is_param = op.is_param;
    return 1;
  }

  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;

  /* Fast reject: a vreg with no def anywhere resolves to 0 — skip the scan. */
  if (sav_vreg_has_no_def(vr))
    return 0;

  /* A vreg with exactly ONE def in the function holds that def's value at
   * every use the def reaches, so a merge in between says nothing -- and the
   * backward scan below, which refuses to cross one, cannot see it.  That is
   * `ur`'s address in __aeabi_dadd: assigned once above a diamond and
   * dereferenced below it, which left the slot address-taken and stopped
   * every downstream pass from tracking the slot at all.  A use the single def
   * does NOT reach is reading an uninitialized vreg, undefined either way, and
   * the pass has already refused functions with backward control flow, so
   * there is no loop-carried case to get wrong.  Only a DIRECT address is
   * accepted here; anything needing arithmetic still goes through the scan. */
  {
    int only = sav_vreg_unique_def(vr);
    if (only >= 0 && only != at_idx)
    {
      IRQuadCompact *dq = &ir->compact_instructions[only];
      if (dq->op == TCCIR_OP_ASSIGN || dq->op == TCCIR_OP_LEA)
      {
        IROperand src = tcc_ir_op_get_src1(ir, dq);
        int direct = (irop_get_tag(src) == IROP_TAG_STACKOFF && irop_get_vreg(src) == -1 &&
                      (dq->op == TCCIR_OP_LEA ? !src.is_llocal : !src.is_lval));
        if (direct)
        {
          out->off = (int)irop_get_stack_offset(src);
          out->is_param = src.is_param;
          return 1;
        }
      }
    }
  }

  /* A vreg read at a merge point has an edge-dependent value — bail. */
  if (at_idx >= 0 && at_idx < ir->next_instruction_index &&
      ir->compact_instructions[at_idx].is_jump_target)
    return 0;

  for (int j = at_idx - 1; j >= 0; j--)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];

    /* Real def of vr? STORE-style / FUNCPARAMVAL dests are uses, not defs. */
    int is_def_of_vr = 0;
    if (q->op != TCCIR_OP_NOP && irop_config[q->op].has_dest && sav_is_def_op(q->op))
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (irop_get_vreg(dest) == vr)
        is_def_of_vr = 1;
    }

    /* Never cross a merge point (NOPed jump targets included). */
    if (!is_def_of_vr)
    {
      if (q->is_jump_target)
        return 0;
      continue;
    }

    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand src = tcc_ir_op_get_src1(ir, q);
      return ir_resolve_stack_addr_value_ex(ir, src, j, out, depth + 1);
    }
    if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      StackAddrValue base;
      int64_t c;
      if (!ir_resolve_stack_addr_value_ex(ir, s1, j, &base, depth + 1))
        return 0;
      if (!irop_is_immediate(s2))
        return 0;
      c = irop_get_imm64_ex(ir, s2);
      if (q->op == TCCIR_OP_SUB)
        c = -c;
      c += base.off;
      if (c != (int32_t)c)
        return 0;
      out->off = (int)c;
      out->is_param = base.is_param;
      return 1;
    }
    /* Some other op writes vr — give up. */
    return 0;
  }
  return 0;
}

/* Canonicalize stack-address derefs and fold stack-address arithmetic. */
int tcc_ir_opt_stack_addr_simplify(TCCIRState *ir)
{
  int n = ir ? ir->next_instruction_index : 0;
  int changes = 0;

  if (ir_has_backward_control_flow(ir))
    return 0;

  sav_build_def_map(ir);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (dest.is_lval &&
          !(irop_get_tag(dest) == IROP_TAG_STACKOFF && dest.is_local && !dest.is_llocal))
      {
        StackAddrValue addr;
        if (ir_resolve_stack_addr_value_ex(ir, dest, i, &addr, 0))
        {
          IROperand direct = irop_make_stackoff(-1, addr.off, 1, 0, addr.is_param, irop_get_btype(dest));
          direct.is_unsigned = dest.is_unsigned;
          tcc_ir_set_dest(ir, i, direct);
          changes++;
        }
      }
      continue;
    }

    if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && irop_config[q->op].has_src1)
    {
      IROperand src = tcc_ir_op_get_src1(ir, q);
      if (src.is_lval &&
          !(irop_get_tag(src) == IROP_TAG_STACKOFF && src.is_local && !src.is_llocal))
      {
        StackAddrValue addr;
        if (ir_resolve_stack_addr_value_ex(ir, src, i, &addr, 0))
        {
          IROperand direct = irop_make_stackoff(-1, addr.off, 1, 0, addr.is_param, irop_get_btype(src));
          direct.is_unsigned = src.is_unsigned;
          tcc_ir_set_src1(ir, i, direct);
          changes++;
        }
      }
    }

    if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && irop_config[q->op].has_dest)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      StackAddrValue a1, a2;
      int folded = 0;
      int64_t result = 0;

      if (q->op == TCCIR_OP_SUB &&
          ir_resolve_stack_addr_value_ex(ir, src1, i, &a1, 0) &&
          ir_resolve_stack_addr_value_ex(ir, src2, i, &a2, 0) &&
          a1.is_param == a2.is_param)
      {
        result = (int64_t)a1.off - (int64_t)a2.off;
        folded = 1;
      }

      if (folded && result == (int32_t)result)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)result, irop_get_btype(dest)));
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
    }
  }

  sav_free_def_map();
  return changes;
}

/* Fold CMP whose operands resolve to the same stack-frame offset. */
int tcc_ir_opt_cmp_stack_addr_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

  /* IJUMP safety: address-taken labels aren't is_jump_target — bail. */
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;
  }

  sav_build_def_map(ir);

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;

    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);

    int off1, off2;
    if (!ir_resolve_stack_addr_value(ir, s1, i, &off1))
      continue;
    if (!ir_resolve_stack_addr_value(ir, s2, i, &off2))
      continue;
    if (off1 != off2)
      continue; /* could fold to !equal too, but be conservative */

    IRQuadCompact *next = &ir->compact_instructions[i + 1];
    if (next->op == TCCIR_OP_JUMPIF)
    {
      IROperand cond = tcc_ir_op_get_src1(ir, next);
      int tok = (int)irop_get_imm64_ex(ir, cond);
      int result = evaluate_compare_condition(0, 0, tok); /* equal-equal */
      if (result < 0)
        continue;
      IROperand jmp_dest = tcc_ir_op_get_dest(ir, next);
      LOG_IR_GEN("OPTIMIZE: CMP stack-addr fold at %d (off=%d, %s)",
                 i, off1, result ? "taken" : "not taken");
      if (result)
      {
        q->op = TCCIR_OP_NOP;
        next->op = TCCIR_OP_JUMP;
        tcc_ir_set_dest(ir, i + 1, jmp_dest);
      }
      else
      {
        q->op = TCCIR_OP_NOP;
        next->op = TCCIR_OP_NOP;
      }
      changes++;
    }
    else if (next->op == TCCIR_OP_SELECT)
    {
      IROperand select_cond = ir->iroperand_pool[next->operand_base + 3];
      int tok = (int)irop_get_imm64_ex(ir, select_cond);
      int result = evaluate_compare_condition(0, 0, tok);
      if (result < 0)
        continue;
      IROperand then_val = tcc_ir_op_get_src1(ir, next);
      IROperand else_val = tcc_ir_op_get_src2(ir, next);
      IROperand chosen = result ? then_val : else_val;
      q->op = TCCIR_OP_NOP;
      next->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i + 1, chosen);
      tcc_ir_set_src2(ir, i + 1, IROP_NONE);
      changes++;
    }
  }
  sav_free_def_map();
  return changes;
}


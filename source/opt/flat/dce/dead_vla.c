/*
 *  TCC IR - Dead-VLA elimination: dead VLA structs, alloca-load forwarding, dead alloca vregs
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* NOPing the alloc strands the outer SAVE/RESTORE pair; zero_vla_elim drops it. */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"

static int op_is_address_propagator(TccIrOp op)
{
  switch (op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LEA:
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
    return 1;
  default:
    return 0;
  }
}

static int operand_reads_slot(IROperand op, int32_t slot)
{
  if (!op.is_lval)
    return 0;
  if (irop_get_tag(op) != IROP_TAG_STACKOFF)
    return 0;
  if (!op.is_local)
    return 0;
  if (irop_get_vreg(op) != -1)
    return 0;
  return irop_get_stack_offset(op) == slot;
}

static int operand_is_temp(IROperand op, int *out_pos)
{
  if (op.is_lval)
    return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  *out_pos = TCCIR_DECODE_VREG_POSITION(vr);
  return 1;
}

static int operand_is_temp_lval(IROperand op, int *out_pos)
{
  if (!op.is_lval)
    return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  *out_pos = TCCIR_DECODE_VREG_POSITION(vr);
  return 1;
}

/* Caller owns tainted[max_tmp+1] and kill_idx (capacity >= n). */
static int analyze_dead_vla(TCCIRState *ir, int vla_idx, int max_tmp,
                            uint8_t *tainted, int *kill_idx, int *kill_count,
                            int *out_save_idx)
{
  int n = ir->next_instruction_index;
  *kill_count = 0;
  *out_save_idx = -1;
  memset(tainted, 0, max_tmp + 1);

  /* The frontend always emits VLA_ALLOC / inner VLA_SP_SAVE contiguously. */
  int save_idx = -1;
  for (int j = vla_idx + 1; j < n; j++)
  {
    TccIrOp op = ir->compact_instructions[j].op;
    if (op == TCCIR_OP_NOP)
      continue;
    if (op == TCCIR_OP_VLA_SP_SAVE)
      save_idx = j;
    break;
  }
  if (save_idx < 0)
    return 0;

  IROperand save_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[save_idx]);
  if (irop_get_tag(save_dest) != IROP_TAG_STACKOFF || !save_dest.is_local ||
      irop_get_vreg(save_dest) != -1)
    return 0;
  int32_t slot = irop_get_stack_offset(save_dest);

  /* A second writer to the slot would break single-source taint reasoning. */
  for (int j = 0; j < n; j++)
  {
    if (j == save_idx)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_lval && irop_get_tag(d) == IROP_TAG_STACKOFF && d.is_local &&
        irop_get_vreg(d) == -1 && irop_get_stack_offset(d) == slot)
      return 0;
    if (q->op == TCCIR_OP_VLA_SP_SAVE && irop_get_tag(d) == IROP_TAG_STACKOFF &&
        d.is_local && irop_get_vreg(d) == -1 && irop_get_stack_offset(d) == slot)
      return 0;
  }

  for (int j = save_idx + 1; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* `slot` is the VLA-base capture, not the outer save: a RESTORE reading it can't happen. */
    if (q->op == TCCIR_OP_VLA_SP_RESTORE)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (operand_reads_slot(s1, slot))
        return 0;
      continue;
    }

    int has_d = irop_config[q->op].has_dest;
    int has_s1 = irop_config[q->op].has_src1;
    int has_s2 = irop_config[q->op].has_src2;
    /* MLA's accumulator is a real source the src1/src2 helpers don't surface. */
    int has_accum = (q->op == TCCIR_OP_MLA);
    IROperand d = {0}, s1 = {0}, s2 = {0}, accum = {0};
    if (has_d) d = tcc_ir_op_get_dest(ir, q);
    if (has_s1) s1 = tcc_ir_op_get_src1(ir, q);
    if (has_s2) s2 = tcc_ir_op_get_src2(ir, q);
    if (has_accum) accum = tcc_ir_op_get_accum(ir, q);

    int reads_slot = 0;
    int reads_tainted = 0;
    int tpos;
    if (has_s1)
    {
      if (operand_reads_slot(s1, slot))
        reads_slot = 1;
      else if (operand_is_temp(s1, &tpos) && tpos <= max_tmp && tainted[tpos])
        reads_tainted = 1;
    }
    if (has_s2)
    {
      if (operand_reads_slot(s2, slot))
        reads_slot = 1;
      else if (operand_is_temp(s2, &tpos) && tpos <= max_tmp && tainted[tpos])
        reads_tainted = 1;
    }
    if (has_accum)
    {
      if (operand_reads_slot(accum, slot))
        reads_slot = 1;
      else if (operand_is_temp(accum, &tpos) && tpos <= max_tmp && tainted[tpos])
        reads_tainted = 1;
    }

    /* Tainted deref-dest = kill candidate; tainted src1 = the pointer escapes.
     * The indexed/postinc forms address through a plain (non-lval) dest, so
     * they need the same treatment -- see the dest-is-a-use comment in
     * sweep_orphan_tmp_defs. */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC)
    {
      int dpos;
      int dest_is_tainted = has_d &&
                            (q->op == TCCIR_OP_STORE ? operand_is_temp_lval(d, &dpos)
                                                     : operand_is_temp(d, &dpos)) &&
                            dpos <= max_tmp && tainted[dpos];
      if (has_s1)
      {
        if (operand_reads_slot(s1, slot))
          return 0;
        if (operand_is_temp(s1, &tpos) && tpos <= max_tmp && tainted[tpos])
          return 0;
      }
      if (dest_is_tainted)
      {
        kill_idx[(*kill_count)++] = j;
        continue;
      }
      continue;
    }

    /* Any read through a tainted deref observes bytes we'd be eliminating. */
    int deref_pos;
    if (has_s1 && operand_is_temp_lval(s1, &deref_pos) &&
        deref_pos <= max_tmp && tainted[deref_pos])
      return 0;
    if (has_s2 && operand_is_temp_lval(s2, &deref_pos) &&
        deref_pos <= max_tmp && tainted[deref_pos])
      return 0;

    if (!reads_slot && !reads_tainted)
      continue;

    /* Without a tame propagator and taintable TEMP dest the address escapes untracked. */
    if (!op_is_address_propagator(q->op))
      return 0;
    if (!has_d)
      return 0;
    int dpos;
    if (!operand_is_temp(d, &dpos))
      return 0;
    if (dpos > max_tmp)
      return 0;

    tainted[dpos] = 1;
    kill_idx[(*kill_count)++] = j;
  }

  *out_save_idx = save_idx;
  return 1;
}

/* Whitelist only: dropping e.g. a LOAD would lose a volatile-memory read. */
static int op_is_side_effect_free_tmp_def(TccIrOp op)
{
  switch (op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LEA:
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  case TCCIR_OP_MUL:
  case TCCIR_OP_ROR:
  case TCCIR_OP_ZEXT:
    return 1;
  default:
    return 0;
  }
}

/* Drains the offset-computation chain once its tail consumer has been NOPed. */
static int sweep_orphan_tmp_defs(TCCIRState *ir, int max_tmp)
{
  int n = ir->next_instruction_index;
  int total = 0;

  int *use_count = tcc_mallocz(sizeof(int) * (max_tmp + 1));
  int changed = 1;
  while (changed)
  {
    changed = 0;
    memset(use_count, 0, sizeof(int) * (max_tmp + 1));

    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        int32_t vr = irop_get_vreg(s);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (p <= max_tmp)
            use_count[p]++;
        }
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        int32_t vr = irop_get_vreg(s);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (p <= max_tmp)
            use_count[p]++;
        }
      }
      /* MLA accumulator is a use the src1/src2 helpers above don't cover. */
      if (q->op == TCCIR_OP_MLA)
      {
        IROperand a = tcc_ir_op_get_accum(ir, q);
        int32_t vr = irop_get_vreg(a);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (p <= max_tmp)
            use_count[p]++;
        }
      }
      /* Every store's dest names the address it writes through -- a use of
       * that TEMP, not a def.  STORE spells it as an lval (deref) operand;
       * STORE_INDEXED and STORE_POSTINC carry the base in a plain dest with
       * the displacement in the instruction, so `is_lval` is false there and
       * the operand still has to be counted.  Missing them let this sweep NOP
       * the LEA feeding an indexed store: `T4 <- Addr[StackLoc[-32]]` looked
       * unused because its only consumer was `T4 <- T6 STORE_INDEXED #20`,
       * and the emitted code then stored through whatever register the
       * allocator had given the now-undefined T4 (GNU make's eval_makefile
       * wrote ebuf.floc through three uninitialised callee-saved registers).
       * This sweep runs over the whole function, not just the dead VLA's own
       * chain, so any indexed store in the function was exposed. */
      if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
           q->op == TCCIR_OP_STORE_POSTINC) &&
          irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (d.is_lval || q->op != TCCIR_OP_STORE)
        {
          int32_t vr = irop_get_vreg(d);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
          {
            int p = TCCIR_DECODE_VREG_POSITION(vr);
            if (p <= max_tmp)
              use_count[p]++;
          }
        }
      }
    }

    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (!op_is_side_effect_free_tmp_def(q->op))
        continue;
      if (!irop_config[q->op].has_dest)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (d.is_lval)
        continue;
      int32_t vr = irop_get_vreg(d);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (p > max_tmp)
        continue;
      if (use_count[p] != 0)
        continue;
      q->op = TCCIR_OP_NOP;
      changed = 1;
      total++;
    }
  }

  tcc_free(use_count);
  return total;
}

int tcc_ir_opt_dead_vla_struct_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Static chains and nested funcs leak a VLA address with no visible FUNCCALL. */
  if (ir->captured_count > 0 || ir->has_static_chain ||
      ir->nb_nested_funcs > 0)
    return 0;

  /* Bail on opcodes whose memory effects we don't model. */
  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
        op == TCCIR_OP_NL_SETJMP || op == TCCIR_OP_NL_LONGJMP ||
        op == TCCIR_OP_INLINE_ASM)
      return 0;
  }

  int max_tmp = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos > max_tmp)
      max_tmp = pos;
  }

  uint8_t *tainted = tcc_malloc(max_tmp + 1);
  int *kill_idx = tcc_malloc(sizeof(int) * n);

  int total_changes = 0;
  int any_dead_vla = 0;
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_VLA_ALLOC)
      continue;
    int save_idx = -1;
    int kill_count = 0;
    if (!analyze_dead_vla(ir, i, max_tmp, tainted, kill_idx, &kill_count,
                          &save_idx))
      continue;

    LOG_IR_GEN("DEAD-VLA-STRUCT: NOP VLA_ALLOC@%d + SP_SAVE@%d + %d "
               "dependent ops (slot=%d)",
               i, save_idx,
               kill_count,
               irop_get_stack_offset(tcc_ir_op_get_dest(
                   ir, &ir->compact_instructions[save_idx])));
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[save_idx].op = TCCIR_OP_NOP;
    for (int k = 0; k < kill_count; k++)
      ir->compact_instructions[kill_idx[k]].op = TCCIR_OP_NOP;
    total_changes += 2 + kill_count;
    any_dead_vla = 1;
  }

  tcc_free(kill_idx);
  tcc_free(tainted);

  if (any_dead_vla)
    total_changes += sweep_orphan_tmp_defs(ir, max_tmp);

  /* Every VLA gone: the parser's force_frame_pointer is now spurious, drop the prologue. */
  if (any_dead_vla)
  {
    int has_vla_or_apply = 0;
    for (int i = 0; i < n; i++)
    {
      int op = ir->compact_instructions[i].op;
      if (op == TCCIR_OP_VLA_ALLOC || op == TCCIR_OP_BUILTIN_APPLY_ARGS ||
          op == TCCIR_OP_BUILTIN_APPLY || op == TCCIR_OP_SET_CHAIN)
      {
        has_vla_or_apply = 1;
        break;
      }
    }
    if (!has_vla_or_apply && tcc_state)
    {
      tcc_state->force_frame_pointer = 0;
      tcc_state->need_frame_pointer = 0;
    }
  }

  return total_changes;
}

int tcc_ir_opt_dead_vla_struct_elim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dead_vla_struct_elim(ctx->ir);
}

/* SP_SAVE-to-slot + LOAD-back becomes a REG-dest SP_SAVE, i.e. `mov dest, sp`. */
int tcc_ir_opt_alloca_load_fwd(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *save = &ir->compact_instructions[i];
    if (save->op != TCCIR_OP_VLA_SP_SAVE)
      continue;

    IROperand save_dest = tcc_ir_op_get_dest(ir, save);
    if (irop_get_tag(save_dest) != IROP_TAG_STACKOFF || !save_dest.is_local ||
        irop_get_vreg(save_dest) != -1)
      continue;
    int32_t slot = irop_get_stack_offset(save_dest);

    int j = i + 1;
    while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;
    if (j >= n)
      continue;

    IRQuadCompact *ld = &ir->compact_instructions[j];
    if (ld->op != TCCIR_OP_LOAD)
      continue;
    if (ld->is_jump_target)
      continue;

    IROperand ld_src = tcc_ir_op_get_src1(ir, ld);
    IROperand ld_dest = tcc_ir_op_get_dest(ir, ld);

    if (irop_get_tag(ld_src) != IROP_TAG_STACKOFF || !ld_src.is_local ||
        irop_get_vreg(ld_src) != -1 || irop_get_stack_offset(ld_src) != slot)
      continue;
    if (ld_src.is_llocal)
      continue;
    /* SP is 32-bit; pairs and sub-word loads would need extension we can't express. */
    if (irop_needs_pair(ld_dest))
      continue;
    if (ld_dest.btype != IROP_BTYPE_INT32 && ld_dest.btype != 0)
      continue;

    /* A deref/spill dest would still be written back to memory. */
    if (irop_get_tag(ld_dest) != IROP_TAG_VREG || ld_dest.is_lval)
      continue;
    int32_t ld_dest_vr = irop_get_vreg(ld_dest);
    if (ld_dest_vr < 0)
      continue;

    /* Any other reader/writer of the slot needs its value to stay in memory. */
    int slot_is_isolated = 1;
    for (int k = 0; k < n && slot_is_isolated; k++)
    {
      if (k == i || k == j)
        continue;
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op == TCCIR_OP_NOP)
        continue;

      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (irop_get_tag(d) == IROP_TAG_STACKOFF && d.is_local &&
            irop_get_vreg(d) == -1 && irop_get_stack_offset(d) == slot)
        {
          slot_is_isolated = 0;
          break;
        }
      }
      if (irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (operand_reads_slot(s, slot))
        {
          slot_is_isolated = 0;
          break;
        }
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        if (operand_reads_slot(s, slot))
        {
          slot_is_isolated = 0;
          break;
        }
      }
    }
    if (!slot_is_isolated)
      continue;

    IROperand new_dest = irop_make_vreg(ld_dest_vr, IROP_BTYPE_INT32);
    tcc_ir_set_dest(ir, i, new_dest);
    ld->op = TCCIR_OP_NOP;

    LOG_IR_GEN("ALLOCA-FWD: VLA_SP_SAVE@%d slot=%d redirected to vreg=%d "
               "(LOAD@%d folded)",
               i, slot, ld_dest_vr, j);
    changes++;
  }

  return changes;
}

int tcc_ir_opt_alloca_load_fwd_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_alloca_load_fwd(ctx->ir);
}

/* VREG-dest counterpart of dead_vla_struct_elim, whose slot analysis misses the
 * SP_SAVE once alloca_load_fwd has retargeted it to a vreg. */
int tcc_ir_opt_dead_alloca_vreg_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  if (ir->captured_count > 0 || ir->has_static_chain || ir->nb_nested_funcs > 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
        op == TCCIR_OP_NL_SETJMP || op == TCCIR_OP_NL_LONGJMP ||
        op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_SET_CHAIN ||
        op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
  }

  int max_tmp = 0, max_var = 0;
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
      if (vr < 0)
        continue;
      int t = TCCIR_DECODE_VREG_TYPE(vr);
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (t == TCCIR_VREG_TYPE_TEMP && p > max_tmp) max_tmp = p;
      else if (t == TCCIR_VREG_TYPE_VAR && p > max_var) max_var = p;
    }
  }

  uint8_t *tainted_tmp = tcc_malloc((max_tmp + 1));
  uint8_t *tainted_var = (max_var > 0) ? tcc_malloc((max_var + 1)) : NULL;
  int *kill_idx = tcc_malloc(sizeof(int) * n);

  int total_changes = 0;
  int any_dead = 0;

  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_VLA_ALLOC)
      continue;

    int save_idx = -1;
    for (int j = i + 1; j < n; j++)
    {
      TccIrOp op = ir->compact_instructions[j].op;
      if (op == TCCIR_OP_NOP)
        continue;
      if (op == TCCIR_OP_VLA_SP_SAVE)
        save_idx = j;
      break;
    }
    if (save_idx < 0)
      continue;

    IROperand save_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[save_idx]);
    int32_t seed_vr = irop_get_vreg(save_dest);
    if (seed_vr < 0)
      continue; /* slot-dest case → handled by dead_vla_struct_elim */
    int seed_type = TCCIR_DECODE_VREG_TYPE(seed_vr);
    int seed_pos = TCCIR_DECODE_VREG_POSITION(seed_vr);

    memset(tainted_tmp, 0, max_tmp + 1);
    if (tainted_var)
      memset(tainted_var, 0, max_var + 1);

    if (seed_type == TCCIR_VREG_TYPE_TEMP && seed_pos <= max_tmp)
      tainted_tmp[seed_pos] = 1;
    else if (seed_type == TCCIR_VREG_TYPE_VAR && tainted_var && seed_pos <= max_var)
      tainted_var[seed_pos] = 1;
    else
      continue;

    int kill_count = 0;
    int bail = 0;

    for (int j = save_idx + 1; j < n && !bail; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;

      if (q->op == TCCIR_OP_VLA_SP_RESTORE)
        continue;
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID ||
          q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
      {
        bail = 1;
        break;
      }

      int has_d = irop_config[q->op].has_dest;
      int has_s1 = irop_config[q->op].has_src1;
      int has_s2 = irop_config[q->op].has_src2;
      /* MLA's accumulator is a real source the src1/src2 helpers miss. */
      int has_accum = (q->op == TCCIR_OP_MLA);
      IROperand d = {0}, s1 = {0}, s2 = {0}, accum = {0};
      if (has_d) d = tcc_ir_op_get_dest(ir, q);
      if (has_s1) s1 = tcc_ir_op_get_src1(ir, q);
      if (has_s2) s2 = tcc_ir_op_get_src2(ir, q);
      if (has_accum) accum = tcc_ir_op_get_accum(ir, q);

      /* is_lval on a TEMP src derefs the alloca ptr; on a VAR src it is a slot fetch. */
#define CLASSIFY(_op, _val_out, _deref_out)                                      \
  do                                                                             \
  {                                                                              \
    int32_t _vr = irop_get_vreg(_op);                                            \
    if (_vr >= 0)                                                                \
    {                                                                            \
      int _vt = TCCIR_DECODE_VREG_TYPE(_vr);                                     \
      int _vp = TCCIR_DECODE_VREG_POSITION(_vr);                                 \
      if (_vt == TCCIR_VREG_TYPE_TEMP && _vp <= max_tmp && tainted_tmp[_vp])     \
      {                                                                          \
        if ((_op).is_lval) _deref_out = 1;                                       \
        else _val_out = 1;                                                       \
      }                                                                          \
      else if (_vt == TCCIR_VREG_TYPE_VAR && tainted_var && _vp <= max_var &&    \
               tainted_var[_vp])                                                 \
      {                                                                          \
        _val_out = 1;                                                            \
      }                                                                          \
    }                                                                            \
  } while (0)

      int s1_val = 0, s1_deref = 0, s2_val = 0, s2_deref = 0;
      int acc_val = 0, acc_deref = 0;
      if (has_s1) CLASSIFY(s1, s1_val, s1_deref);
      if (has_s2) CLASSIFY(s2, s2_val, s2_deref);
      if (has_accum) CLASSIFY(accum, acc_val, acc_deref);

      if (s1_deref || s2_deref || acc_deref)
      {
        bail = 1;
        break;
      }

      /* Tainted dest = kill candidate; tainted src1 into an untainted dest = escape. */
      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
          q->op == TCCIR_OP_STORE_POSTINC)
      {
        int dest_is_tainted_addr = 0;
        if (has_d)
        {
          int32_t _vr = irop_get_vreg(d);
          if (_vr >= 0)
          {
            int _vt = TCCIR_DECODE_VREG_TYPE(_vr);
            int _vp = TCCIR_DECODE_VREG_POSITION(_vr);
            if (_vt == TCCIR_VREG_TYPE_TEMP && _vp <= max_tmp && tainted_tmp[_vp])
              dest_is_tainted_addr = 1;
          }
        }
        if (s1_val && !dest_is_tainted_addr)
        {
          bail = 1;
          break;
        }
        if (dest_is_tainted_addr)
          kill_idx[kill_count++] = j;
        continue;
      }

      /* No tainted input: a tainted VAR dest is overwritten, so it loses taint. */
      if (!s1_val && !s2_val && !acc_val)
      {
        if (has_d)
        {
          int32_t _vr = irop_get_vreg(d);
          if (_vr >= 0)
          {
            int _vt = TCCIR_DECODE_VREG_TYPE(_vr);
            int _vp = TCCIR_DECODE_VREG_POSITION(_vr);
            if (_vt == TCCIR_VREG_TYPE_VAR && tainted_var && _vp <= max_var &&
                tainted_var[_vp])
              tainted_var[_vp] = 0;
          }
        }
        continue;
      }

      /* LOAD propagates: the frontend emits it for plain VAR fetches. */
      int is_prop = (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA ||
                     q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ADD ||
                     q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_AND ||
                     q->op == TCCIR_OP_OR || q->op == TCCIR_OP_XOR);
      if (!is_prop || !has_d)
      {
        bail = 1;
        break;
      }
      int32_t d_vr = irop_get_vreg(d);
      if (d_vr < 0)
      {
        bail = 1;
        break;
      }
      int d_vt = TCCIR_DECODE_VREG_TYPE(d_vr);
      int d_vp = TCCIR_DECODE_VREG_POSITION(d_vr);
      if (d_vt == TCCIR_VREG_TYPE_TEMP && d_vp <= max_tmp)
      {
        tainted_tmp[d_vp] = 1;
        kill_idx[kill_count++] = j;
      }
      else if (d_vt == TCCIR_VREG_TYPE_VAR && tainted_var && d_vp <= max_var)
      {
        tainted_var[d_vp] = 1;
        kill_idx[kill_count++] = j;
      }
      else
      {
        bail = 1;
        break;
      }

#undef CLASSIFY
    }

    if (bail)
      continue;

    LOG_IR_GEN("DEAD-ALLOCA-VREG: NOP VLA_ALLOC@%d + VLA_SP_SAVE@%d + %d "
               "dependent ops",
               i, save_idx, kill_count);
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[save_idx].op = TCCIR_OP_NOP;
    for (int k = 0; k < kill_count; k++)
      ir->compact_instructions[kill_idx[k]].op = TCCIR_OP_NOP;
    total_changes += 2 + kill_count;
    any_dead = 1;
  }

  tcc_free(kill_idx);
  if (tainted_var)
    tcc_free(tainted_var);
  tcc_free(tainted_tmp);

  if (any_dead)
    total_changes += sweep_orphan_tmp_defs(ir, max_tmp);

  if (any_dead)
  {
    int has_vla_or_apply = 0;
    for (int i = 0; i < n; i++)
    {
      int op = ir->compact_instructions[i].op;
      if (op == TCCIR_OP_VLA_ALLOC || op == TCCIR_OP_BUILTIN_APPLY_ARGS ||
          op == TCCIR_OP_BUILTIN_APPLY || op == TCCIR_OP_SET_CHAIN)
      {
        has_vla_or_apply = 1;
        break;
      }
    }
    if (!has_vla_or_apply && tcc_state)
    {
      tcc_state->force_frame_pointer = 0;
      tcc_state->need_frame_pointer = 0;
    }
  }

  return total_changes;
}

int tcc_ir_opt_dead_alloca_vreg_elim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dead_alloca_vreg_elim(ctx->ir);
}

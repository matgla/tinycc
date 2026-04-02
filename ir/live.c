/*
 *  TCC IR - Liveness Analysis Implementation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

#define IR_LIVE_INTERVAL_INIT_SIZE 64

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/* Check if a FP IR instruction remaining in the IR will be lowered to a
 * soft-float library call (BL) by the backend.  This is needed so the
 * register allocator treats these instructions as call-sites and avoids
 * placing live values in caller-saved registers across them.
 *
 * When no hardware FPU flag is set for an operation, all remaining
 * instances of that IR opcode are guaranteed to be lowered to library
 * calls (non-complex instances were already converted to FUNCCALLVAL/
 * FUNCCALLVOID by ir_put_soft_call_fpu_if_needed; complex instances
 * bypass that conversion but are still calls in the backend).
 */
static int ir_op_is_implicit_call(TccIrOp op)
{
  const FloatingPointConfig *fpu = architecture_config.fpu;
  if (!fpu)
    return 0;
  switch (op)
  {
  case TCCIR_OP_FADD:
    return !(fpu->has_fadd && fpu->has_dadd);
  case TCCIR_OP_FSUB:
    return !(fpu->has_fsub && fpu->has_dsub);
  case TCCIR_OP_FMUL:
    return !(fpu->has_fmul && fpu->has_dmul);
  case TCCIR_OP_FDIV:
    return !(fpu->has_fdiv && fpu->has_ddiv);
  case TCCIR_OP_FNEG:
    return !(fpu->has_fneg && fpu->has_dneg);
  case TCCIR_OP_FCMP:
    return !(fpu->has_fcmp && fpu->has_dcmp);
  case TCCIR_OP_CVT_FTOF:
    return !(fpu->has_ftof && fpu->has_dtof);
  case TCCIR_OP_CVT_ITOF:
    return !(fpu->has_itof && fpu->has_itod);
  case TCCIR_OP_CVT_FTOI:
    return !(fpu->has_ftoi && fpu->has_dtoi);
  default:
    return 0;
  }
}

/* Check if there's a call instruction in range using prefix sum array */
static int live_has_call_in_range_prefix(const int *call_prefix, int start, int end, int instruction_count)
{
  if (!call_prefix)
    return 0;
  if (instruction_count <= 0)
    return 0;
  if (start < -1)
    start = -1;
  if (end > instruction_count)
    end = instruction_count;
  /* We want calls with indices i in [start+1, end-1]. */
  if (end <= start + 1)
    return 0;
  if (start + 1 >= instruction_count)
    return 0;
  return (call_prefix[end] - call_prefix[start + 1]) != 0;
}

/* Extend live intervals for vregs used as function parameters.
 * When a vreg is passed to FUNCPARAMVAL, it must stay live until the
 * corresponding FUNCCALL instruction. */
static void live_extend_param_intervals(TCCIRState *ir)
{
  if (!ir)
    return;

  const int n = ir->next_instruction_index;
  const int max_call_id = ir->next_call_id;

  /* Fast path: use call_id -> call_idx mapping when call_id is available. */
  int *call_idx_by_id = NULL;
  if (max_call_id > 0)
  {
    call_idx_by_id = (int *)tcc_malloc(sizeof(int) * max_call_id);
    for (int i = 0; i < max_call_id; ++i)
      call_idx_by_id[i] = -1;

    for (int call_idx = 0; call_idx < n; ++call_idx)
    {
      const IRQuadCompact *callq = &ir->compact_instructions[call_idx];

      if (callq->op != TCCIR_OP_FUNCCALLVOID && callq->op != TCCIR_OP_FUNCCALLVAL)
        continue;
      const int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, callq)));
      if (call_id >= 0 && call_id < max_call_id)
        call_idx_by_id[call_id] = call_idx;
    }

    for (int j = 0; j < n; ++j)
    {
      const IRQuadCompact *p = &ir->compact_instructions[j];
      if (p->op != TCCIR_OP_FUNCPARAMVAL)
        continue;

      const int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, p)));
      if (call_id < 0 || call_id >= max_call_id)
        continue;
      const int call_idx = call_idx_by_id[call_id];
      if (call_idx < 0)
        continue;

      IROperand src1 = tcc_ir_op_get_src1(ir, p);
      int src1_vreg = irop_get_vreg(src1);
      if (tcc_ir_vreg_is_valid(ir, src1_vreg))
      {
        IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, src1_vreg);
        if (interval && interval->end < (uint32_t)call_idx)
          interval->end = (uint32_t)call_idx;
        if (interval && interval->start == INTERVAL_NOT_STARTED)
          interval->start = 0;
      }
    }

    tcc_free(call_idx_by_id);
    return;
  }

  /* Slow path: scan backwards for each call */
  for (int call_idx = 0; call_idx < ir->next_instruction_index; ++call_idx)
  {
    const IRQuadCompact *callq = &ir->compact_instructions[call_idx];
    if (callq->op != TCCIR_OP_FUNCCALLVOID && callq->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    const int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, callq)));
    for (int j = call_idx - 1; j >= 0; --j)
    {
      const IRQuadCompact *p = &ir->compact_instructions[j];
      if (p->op != TCCIR_OP_FUNCPARAMVAL)
        continue;

      const int param_call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, p)));
      if (param_call_id != call_id)
        continue;

      IROperand src1 = tcc_ir_op_get_src1(ir, p);
      int src1_vreg = irop_get_vreg(src1);
      if (tcc_ir_vreg_is_valid(ir, src1_vreg))
      {
        IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, src1_vreg);
        if (interval && interval->end < (uint32_t)call_idx)
          interval->end = (uint32_t)call_idx;
        if (interval && interval->start == INTERVAL_NOT_STARTED)
          interval->start = 0;
      }
    }
  }
}

/* Extend intervals for variables live at backward jump targets (loop variables) */
static void live_extend_intervals_for_backward_jumps(TCCIRState *ir)
{
  if (!ir)
    return;

  const int n = ir->next_instruction_index;
  if (n <= 0)
    return;

  int *extend_to = (int *)tcc_malloc(sizeof(int) * n);
  for (int i = 0; i < n; ++i)
    extend_to[i] = -1;

  /* Collect the maximum jump index for each backward-jump target. */
  for (int i = 0; i < n; ++i)
  {
    const IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      const int target = tcc_ir_op_get_dest(ir, q).u.imm32;
      if (target >= 0 && target < n && target < i)
      {
        if (extend_to[target] < i)
          extend_to[target] = i;
      }
    }
    else if (q->op == TCCIR_OP_SWITCH_TABLE)
    {
      /* SWITCH_TABLE can jump backward to any of its case targets */
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, src2);
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++)
        {
          int target = table->targets[j];
          if (target >= 0 && target < n && target < i)
          {
            if (extend_to[target] < i)
              extend_to[target] = i;
          }
        }
        int dtarget = table->default_target;
        if (dtarget >= 0 && dtarget < n && dtarget < i)
        {
          if (extend_to[dtarget] < i)
            extend_to[dtarget] = i;
        }
      }
    }
    else if (q->op == TCCIR_OP_IJUMP)
    {
      /* IJUMP (computed goto) can target any label in the function.
       * Since targets are determined at runtime, conservatively treat it
       * as a backward edge to instruction 0. */
      if (i > 0)
      {
        if (extend_to[0] < i)
          extend_to[0] = i;
      }
    }
  }

  int target_count = 0;
  for (int t = 0; t < n; ++t)
    if (extend_to[t] >= 0)
      ++target_count;
  if (target_count == 0)
  {
    tcc_free(extend_to);
    return;
  }

  int *targets = (int *)tcc_malloc(sizeof(int) * target_count);
  int *is_ijmp_target = (int *)tcc_malloc(sizeof(int) * target_count);
  int out = 0;
  for (int t = 0; t < n; ++t)
    if (extend_to[t] >= 0)
    {
      targets[out] = t;
      is_ijmp_target[out] = 0;
      out++;
    }

  /* Mark targets that originate from IJMP (computed goto).  IJMP targets
   * are conservatively set to instruction 0, so check if any IJMP exists. */
  {
    int has_ijmp = 0;
    for (int i = 0; i < n && !has_ijmp; ++i)
    {
      if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
        has_ijmp = 1;
    }
    if (has_ijmp)
    {
      for (int ti = 0; ti < target_count; ++ti)
      {
        if (targets[ti] == 0)
          is_ijmp_target[ti] = 1;
      }
    }
  }

  const int local_count = ir->next_local_variable;
  const int temp_count = ir->next_temporary_variable;
  const int param_count = ir->next_parameter;
  const int interval_count = local_count + temp_count + param_count;

  int *start_head = (int *)tcc_malloc(sizeof(int) * n);
  for (int i = 0; i < n; ++i)
    start_head[i] = -1;
  int *start_next = (int *)tcc_malloc(sizeof(int) * interval_count);
  IRLiveInterval **start_interval = (IRLiveInterval **)tcc_malloc(sizeof(IRLiveInterval *) * interval_count);

  int node_idx = 0;
  for (int v = 0; v < local_count; ++v)
  {
    IRLiveInterval *interval = &ir->variables_live_intervals[v];
    if (interval->start == INTERVAL_NOT_STARTED)
      continue;
    int s = (int)interval->start;
    if (s < 0)
      s = 0;
    if (s >= n)
      continue;
    start_interval[node_idx] = interval;
    start_next[node_idx] = start_head[s];
    start_head[s] = node_idx++;
  }
  for (int v = 0; v < temp_count; ++v)
  {
    IRLiveInterval *interval = &ir->temporary_variables_live_intervals[v];
    if (interval->start == INTERVAL_NOT_STARTED)
      continue;
    int s = (int)interval->start;
    if (s < 0)
      s = 0;
    if (s >= n)
      continue;
    start_interval[node_idx] = interval;
    start_next[node_idx] = start_head[s];
    start_head[s] = node_idx++;
  }
  for (int v = 0; v < param_count; ++v)
  {
    IRLiveInterval *interval = &ir->parameters_live_intervals[v];
    if (interval->start == INTERVAL_NOT_STARTED)
      continue;
    int s = (int)interval->start;
    if (s < 0)
      s = 0;
    if (s >= n)
      continue;
    start_interval[node_idx] = interval;
    start_next[node_idx] = start_head[s];
    start_head[s] = node_idx++;
  }

  IRLiveInterval **active = (IRLiveInterval **)tcc_malloc(sizeof(IRLiveInterval *) * node_idx);
  int active_count = 0;
  int scan_pos = 0;

  for (int ti = 0; ti < target_count; ++ti)
  {
    const int target = targets[ti];
    const int jump_end = extend_to[target];
    if (jump_end < 0)
      continue;

    const int ijmp = is_ijmp_target[ti];

    /* For IJMP (computed goto) targets, scan the entire loop body [target, jump_end]
     * because IJMP can target any label and variables defined inside the loop body
     * may be live across the backward edge.  For regular backward jumps, only scan
     * up to the target — variables live at the loop header are sufficient. */
    const int scan_limit = ijmp ? jump_end : target;
    for (; scan_pos <= scan_limit && scan_pos < n; ++scan_pos)
    {
      for (int node = start_head[scan_pos]; node != -1; node = start_next[node])
      {
        active[active_count++] = start_interval[node];
      }
    }

    /* Compact active set.  For IJMP targets, keep intervals that overlap
     * [target, jump_end] — the entire loop body — because the runtime target
     * is unknown.  For regular backward jumps, keep only intervals live at
     * the specific target (the original, tighter filter). */
    int w = 0;
    for (int i = 0; i < active_count; ++i)
    {
      IRLiveInterval *interval = active[i];
      if (!interval)
        continue;
      if (interval->start == INTERVAL_NOT_STARTED)
        continue;
      if (ijmp)
      {
        /* Broad filter: overlaps [target, jump_end] */
        if ((int)interval->start > jump_end)
          continue;
        if ((int)interval->end < target)
          continue;
      }
      else
      {
        /* Original tight filter: live at target */
        if ((int)interval->start > target)
          continue;
        if ((int)interval->end < target)
          continue;
      }
      active[w++] = interval;
    }
    active_count = w;

    /* Extend all matching intervals to cover through the jump source. */
    for (int i = 0; i < active_count; ++i)
    {
      IRLiveInterval *interval = active[i];
      if ((int)interval->end < jump_end)
        interval->end = (uint32_t)jump_end;
    }
  }

  tcc_free(active);
  tcc_free(is_ijmp_target);
  tcc_free(start_interval);
  tcc_free(start_next);
  tcc_free(start_head);

  /* Second pass: extend starts for variables live at backward jump sources.
   * When a variable is defined inside a loop but used after the loop exits
   * (or in subsequent iterations), its value must survive through the
   * back-edge.  We extend the start of such intervals to the loop target
   * so they're considered live throughout the loop body.
   *
   * Example: variable V defined at 16, used at 21.  Back-edge 17->6.
   * V is live at 17 (the jump source) but starts at 16 > 6 (the target).
   * Without this fix, a temporary at instruction 9 could reuse V's register
   * since the allocator thinks V isn't live yet at 9. */

  /* Collect all backward edges as (source, target) pairs. */
  int back_edge_count = 0;
  for (int i = 0; i < n; ++i)
  {
    const IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      const int target = tcc_ir_op_get_dest(ir, q).u.imm32;
      if (target >= 0 && target < n && target < i)
        back_edge_count++;
    }
    else if (q->op == TCCIR_OP_SWITCH_TABLE)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, src2);
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++)
        {
          if (table->targets[j] >= 0 && table->targets[j] < n && table->targets[j] < i)
            back_edge_count++;
        }
        if (table->default_target >= 0 && table->default_target < n && table->default_target < i)
          back_edge_count++;
      }
    }
    else if (q->op == TCCIR_OP_IJUMP)
    {
      if (i > 0)
        back_edge_count++;
    }
  }

  if (back_edge_count > 0)
  {
    int *be_src = (int *)tcc_malloc(sizeof(int) * back_edge_count);
    int *be_tgt = (int *)tcc_malloc(sizeof(int) * back_edge_count);
    int bei = 0;
    for (int i = 0; i < n; ++i)
    {
      const IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        const int target = tcc_ir_op_get_dest(ir, q).u.imm32;
        if (target >= 0 && target < n && target < i)
        {
          be_src[bei] = i;
          be_tgt[bei] = target;
          bei++;
        }
      }
      else if (q->op == TCCIR_OP_SWITCH_TABLE)
      {
        IROperand src2 = tcc_ir_op_get_src2(ir, q);
        int table_id = (int)irop_get_imm64_ex(ir, src2);
        if (table_id >= 0 && table_id < ir->num_switch_tables)
        {
          TCCIRSwitchTable *table = &ir->switch_tables[table_id];
          for (int j = 0; j < table->num_entries; j++)
          {
            int target = table->targets[j];
            if (target >= 0 && target < n && target < i)
            {
              be_src[bei] = i;
              be_tgt[bei] = target;
              bei++;
            }
          }
          int dtarget = table->default_target;
          if (dtarget >= 0 && dtarget < n && dtarget < i)
          {
            be_src[bei] = i;
            be_tgt[bei] = dtarget;
            bei++;
          }
        }
      }
      else if (q->op == TCCIR_OP_IJUMP)
      {
        if (i > 0)
        {
          be_src[bei] = i;
          be_tgt[bei] = 0;
          bei++;
        }
      }
    }

    /* Iterate until stable — extending one interval's start may make it
     * live at another back-edge source, requiring further extension
     * (e.g. nested loops). */
    int changed = 1;
    while (changed)
    {
      changed = 0;
      for (int b = 0; b < back_edge_count; ++b)
      {
        const int J = be_src[b]; /* jump source */
        const int T = be_tgt[b]; /* jump target */

        for (int v = 0; v < local_count; ++v)
        {
          IRLiveInterval *iv = &ir->variables_live_intervals[v];
          if (iv->start == INTERVAL_NOT_STARTED)
            continue;
          if ((int)iv->start <= J && (int)iv->end >= J && (int)iv->start > T)
          {
            iv->start = (uint32_t)T;
            changed = 1;
          }
        }
        for (int v = 0; v < temp_count; ++v)
        {
          IRLiveInterval *iv = &ir->temporary_variables_live_intervals[v];
          if (iv->start == INTERVAL_NOT_STARTED)
            continue;
          if ((int)iv->start <= J && (int)iv->end >= J && (int)iv->start > T)
          {
            iv->start = (uint32_t)T;
            changed = 1;
          }
        }
        for (int v = 0; v < param_count; ++v)
        {
          IRLiveInterval *iv = &ir->parameters_live_intervals[v];
          if (iv->start == INTERVAL_NOT_STARTED)
            continue;
          if ((int)iv->start <= J && (int)iv->end >= J && (int)iv->start > T)
          {
            iv->start = (uint32_t)T;
            changed = 1;
          }
        }
      }
    }

    tcc_free(be_src);
    tcc_free(be_tgt);
  }

  tcc_free(targets);
  tcc_free(extend_to);
}

/* ============================================================================
 * Live Interval Computation
 * ============================================================================ */

void tcc_ir_live_intervals_compute(TCCIRState *ir)
{
  /* Reset only start/end positions, preserve other flags like is_lvalue, addrtaken, etc. */
  for (int i = 0; i < ir->next_local_variable; ++i)
  {
    ir->variables_live_intervals[i].start = INTERVAL_NOT_STARTED;
    ir->variables_live_intervals[i].end = 0;
  }
  for (int i = 0; i < ir->next_temporary_variable; ++i)
  {
    ir->temporary_variables_live_intervals[i].start = INTERVAL_NOT_STARTED;
    ir->temporary_variables_live_intervals[i].end = 0;
  }
  for (int i = 0; i < ir->next_parameter; ++i)
  {
    ir->parameters_live_intervals[i].start = INTERVAL_NOT_STARTED;
    ir->parameters_live_intervals[i].end = 0;
  }

  /* Single forward pass over IR to find def/use ranges */
  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Skip NOP instructions */
    if (q->op == TCCIR_OP_NOP)
      continue;

    const IROperand src1 = tcc_ir_op_get_src1(ir, q);
    /* Process source operands (uses) */
    if (irop_config[q->op].has_src1 == 1 && tcc_ir_vreg_is_valid(ir, src1.vr))
    {
      IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, src1.vr);
      if (interval->start == INTERVAL_NOT_STARTED)
      {
        /* Use before def - this is a parameter or input */
        interval->start = 0;
      }
      interval->end = i;
    }

    const IROperand src2 = tcc_ir_op_get_src2(ir, q);
    if (irop_config[q->op].has_src2 == 1 && tcc_ir_vreg_is_valid(ir, src2.vr))
    {
      IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, src2.vr);
      if (interval->start == INTERVAL_NOT_STARTED)
      {
        /* Use before def - this is a parameter or input */
        interval->start = 0;
      }
      interval->end = i;
    }

    /* Process destination operand (definition or use) */
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vreg = irop_get_vreg(dest);
    if (irop_config[q->op].has_dest == 1 && tcc_ir_vreg_is_valid(ir, dest_vreg))
    {
      IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, dest_vreg);
      /* For STORE-like instructions the dest slot holds the target
       * address (base pointer) which is READ, not written.  Treat it
       * as a USE so that parameters / earlier definitions keep their
       * original start and backward-jump extension sees them alive. */
      int dest_is_use = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC);
      if (interval->start == INTERVAL_NOT_STARTED)
      {
        interval->start = dest_is_use ? 0 : i;
      }
      interval->end = i;
    }

    /* MLA has a hidden 4th operand (accumulator) at operand_base+3.
     * The standard src1/src2 scan above doesn't see it, so we must
     * extend liveness for the accumulator vreg explicitly. */
    if (q->op == TCCIR_OP_MLA)
    {
      const IROperand accum = tcc_ir_op_get_accum(ir, q);
      if (tcc_ir_vreg_is_valid(ir, irop_get_vreg(accum)))
      {
        IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, irop_get_vreg(accum));
        if (interval->start == INTERVAL_NOT_STARTED)
        {
          interval->start = 0;
        }
        interval->end = i;
      }
    }
  }

  /* Handle backward jumps - extend intervals for loop variables */
  live_extend_intervals_for_backward_jumps(ir);

  /* Extend intervals for vregs used as function parameters */
  live_extend_param_intervals(ir);
}

/* ============================================================================
 * Full Liveness Analysis
 * ============================================================================ */

void tcc_ir_live_analysis(TCCIRState *ir)
{
  int start, end;
  int crosses_call;
  int addrtaken;
  int reg_type;
  IRLiveInterval *interval;
  tcc_ls_clear_live_intervals(&ir->ls);

  /* Set types based on operand btypes */
  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_config[q->op].has_dest && tcc_ir_vreg_is_valid(ir, irop_get_vreg(dest)))
    {
      int btype = irop_get_btype(dest);
      if (btype == IROP_BTYPE_FLOAT32 || btype == IROP_BTYPE_FLOAT64)
        tcc_ir_vreg_type_set_fp(ir, irop_get_vreg(dest), 1, btype == IROP_BTYPE_FLOAT64);
      else if (btype == IROP_BTYPE_INT64)
        tcc_ir_vreg_type_set_64bit(ir, irop_get_vreg(dest));
      /* Restore complex flag from IROperand (cleared by tcc_ls_clear_live_intervals) */
      if (dest.is_complex)
        tcc_ir_vreg_type_set_complex(ir, irop_get_vreg(dest));
    }
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_config[q->op].has_src1 && tcc_ir_vreg_is_valid(ir, irop_get_vreg(src1)))
    {
      int btype = irop_get_btype(src1);
      if (btype == IROP_BTYPE_FLOAT32 || btype == IROP_BTYPE_FLOAT64)
        tcc_ir_vreg_type_set_fp(ir, irop_get_vreg(src1), 1, btype == IROP_BTYPE_FLOAT64);
      else if (btype == IROP_BTYPE_INT64)
        tcc_ir_vreg_type_set_64bit(ir, irop_get_vreg(src1));
      if (src1.is_complex)
        tcc_ir_vreg_type_set_complex(ir, irop_get_vreg(src1));
    }
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    if (irop_config[q->op].has_src2 && tcc_ir_vreg_is_valid(ir, irop_get_vreg(src2)))
    {
      int btype = irop_get_btype(src2);
      if (btype == IROP_BTYPE_FLOAT32 || btype == IROP_BTYPE_FLOAT64)
        tcc_ir_vreg_type_set_fp(ir, irop_get_vreg(src2), 1, btype == IROP_BTYPE_FLOAT64);
      else if (btype == IROP_BTYPE_INT64)
        tcc_ir_vreg_type_set_64bit(ir, irop_get_vreg(src2));
      if (src2.is_complex)
        tcc_ir_vreg_type_set_complex(ir, irop_get_vreg(src2));
    }
  }

  const int instruction_count = ir->next_instruction_index;
  int *call_prefix = NULL;
  if (instruction_count > 0)
  {
    call_prefix = (int *)tcc_malloc(sizeof(int) * (instruction_count + 1));
    call_prefix[0] = 0;
    for (int i = 0; i < instruction_count; ++i)
    {
      const TccIrOp op = ir->compact_instructions[i].op;
      const int is_call = (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_BUILTIN_APPLY ||
                           ir_op_is_implicit_call(op))
                              ? 1
                              : 0;
      call_prefix[i + 1] = call_prefix[i] + is_call;
    }
  }

  /* Compute live intervals from the IR after optimizations */
  tcc_ir_live_intervals_compute(ir);

  /* Compute per-vreg use counts for spill cost heuristic */
  {
    const int lc = ir->next_local_variable;
    const int tc = ir->next_temporary_variable;
    const int pc = ir->next_parameter;
    const int total = lc + tc + pc;
    uint16_t *uc = (uint16_t *)tcc_mallocz(sizeof(uint16_t) * (total > 0 ? total : 1));
    for (int i = 0; i < instruction_count; ++i)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP) continue;
      const IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (irop_config[q->op].has_src1 && tcc_ir_vreg_is_valid(ir, s1.vr))
      {
        int t = TCCIR_DECODE_VREG_TYPE(s1.vr), p = TCCIR_DECODE_VREG_POSITION(s1.vr);
        int idx = (t == TCCIR_VREG_TYPE_VAR && p < lc) ? p :
                  (t == TCCIR_VREG_TYPE_TEMP && p < tc) ? lc + p :
                  (t == TCCIR_VREG_TYPE_PARAM && p < pc) ? lc + tc + p : -1;
        if (idx >= 0 && uc[idx] < 65535) uc[idx]++;
      }
      const IROperand s2 = tcc_ir_op_get_src2(ir, q);
      if (irop_config[q->op].has_src2 && tcc_ir_vreg_is_valid(ir, s2.vr))
      {
        int t = TCCIR_DECODE_VREG_TYPE(s2.vr), p = TCCIR_DECODE_VREG_POSITION(s2.vr);
        int idx = (t == TCCIR_VREG_TYPE_VAR && p < lc) ? p :
                  (t == TCCIR_VREG_TYPE_TEMP && p < tc) ? lc + p :
                  (t == TCCIR_VREG_TYPE_PARAM && p < pc) ? lc + tc + p : -1;
        if (idx >= 0 && uc[idx] < 65535) uc[idx]++;
      }
    }
    /* Build use count array in interval order (vars, temps, params) */
    int ic = 0;
    for (int v = 0; v < lc; v++) { if (!tcc_ir_vreg_is_ignored(ir, (TCCIR_VREG_TYPE_VAR<<28)|v) && ir->variables_live_intervals[v].start != INTERVAL_NOT_STARTED) ic++; }
    for (int v = 0; v < tc; v++) { if (!tcc_ir_vreg_is_ignored(ir, (TCCIR_VREG_TYPE_TEMP<<28)|v) && ir->temporary_variables_live_intervals[v].start != INTERVAL_NOT_STARTED) ic++; }
    ic += pc;
    uint16_t *w = (uint16_t *)tcc_malloc(sizeof(uint16_t) * (ic > 0 ? ic : 1));
    int wi = 0;
    for (int v = 0; v < lc; v++) { if (!tcc_ir_vreg_is_ignored(ir, (TCCIR_VREG_TYPE_VAR<<28)|v) && ir->variables_live_intervals[v].start != INTERVAL_NOT_STARTED) { w[wi++] = uc[v]; } }
    for (int v = 0; v < tc; v++) { if (!tcc_ir_vreg_is_ignored(ir, (TCCIR_VREG_TYPE_TEMP<<28)|v) && ir->temporary_variables_live_intervals[v].start != INTERVAL_NOT_STARTED) { w[wi++] = uc[lc+v]; } }
    for (int v = 0; v < pc; v++) { w[wi++] = uc[lc+tc+v]; }
    tcc_free(uc);
    tcc_ls_set_use_counts(w, wi);
  }

  /* Now populate the linear scan allocator with the computed intervals */
  for (int vreg = 0; vreg < ir->next_local_variable; ++vreg)
  {
    const int encoded_vreg = (TCCIR_VREG_TYPE_VAR << 28) | vreg;
    if (tcc_ir_vreg_is_ignored(ir, encoded_vreg))
    {
      continue;
    }
    interval = &ir->variables_live_intervals[vreg];
    if (interval->start != INTERVAL_NOT_STARTED)
    {
      start = interval->start;
      end = interval->end;

      /* Check if this is the static chain vreg (for nested functions) */
      int is_static_chain = (ir->has_static_chain && encoded_vreg == ir->static_chain_vreg);

      /* For static chain vreg, extend to end of function */
      if (is_static_chain)
      {
        end = ir->next_instruction_index;
        crosses_call = 1; /* Chain vreg crosses all calls */
      }
      else
      {
        crosses_call = live_has_call_in_range_prefix(call_prefix, start, end, instruction_count);
      }

      addrtaken = interval->addrtaken;
      reg_type = tcc_ir_vreg_type_get(ir, encoded_vreg);
      if (end < ir->next_instruction_index && (ir->compact_instructions[end].op == TCCIR_OP_FUNCCALLVAL ||
                                               ir->compact_instructions[end].op == TCCIR_OP_FUNCCALLVOID))
      {
        crosses_call = 1;
      }

      /* Precolor static chain vreg to R10 */
      int precolored = -1;
      if (is_static_chain)
      {
        precolored = 10; /* R10 is the static chain register */
      }

      tcc_ls_add_live_interval(&ir->ls, encoded_vreg, start, end, crosses_call, addrtaken, reg_type,
                               interval->is_lvalue, precolored);
    }
  }
  for (int vreg = 0; vreg < ir->next_temporary_variable; ++vreg)
  {
    const int vreg_encoded = (TCCIR_VREG_TYPE_TEMP << 28) | vreg;
    if (tcc_ir_vreg_is_ignored(ir, vreg_encoded))
    {
      continue;
    }
    interval = &ir->temporary_variables_live_intervals[vreg];
    if (interval->start != INTERVAL_NOT_STARTED)
    {
      start = interval->start;
      end = interval->end;
      crosses_call = live_has_call_in_range_prefix(call_prefix, start, end, instruction_count);
      addrtaken = interval->addrtaken;
      reg_type = tcc_ir_vreg_type_get(ir, vreg_encoded);
      if (end < ir->next_instruction_index && (ir->compact_instructions[end].op == TCCIR_OP_FUNCCALLVAL ||
                                               ir->compact_instructions[end].op == TCCIR_OP_FUNCCALLVOID))
      {
        crosses_call = 1;
      }
      tcc_ls_add_live_interval(&ir->ls, vreg_encoded, start, end, crosses_call, addrtaken, reg_type,
                               interval->is_lvalue, -1);
    }
  }

  for (int vreg = 0; vreg < ir->next_parameter; ++vreg)
  {
    const int vreg_encoded = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
    interval = &ir->parameters_live_intervals[vreg];
    start = 0;
    end = interval->end;
    if (end == 0)
      end = 1;
    crosses_call = (call_prefix && end > 0) ? (call_prefix[end] != 0) : 0;
    addrtaken = interval->addrtaken;
    reg_type = tcc_ir_vreg_type_get(ir, vreg_encoded);
    /* Only precolor parameters that actually arrive in a register.
     * Stack-passed parameters (incoming_reg0 < 0) must NOT be precolored,
     * even if their vreg index < 4 — e.g. when AAPCS 8-byte alignment
     * skips a register, the parameter indices no longer match register
     * numbers and a stack-passed struct could get a false precoloring. */
    int precolored = -1;
    if (vreg < 4 && !crosses_call && interval->incoming_reg0 >= 0)
      precolored = interval->incoming_reg0;
    tcc_ls_add_live_interval(&ir->ls, vreg_encoded, start, end, crosses_call, addrtaken, reg_type, interval->is_lvalue,
                             precolored);
  }

  if (call_prefix)
    tcc_free(call_prefix);
}

void tcc_ir_live_intervals_patch(TCCIRState *ir)
{
  for (int i = 0; i < ir->ls.next_interval_index; ++i)
  {
    LSLiveInterval *interval = &ir->ls.intervals[i];
    tcc_ir_stack_reg_assign(ir, interval->vreg, interval->stack_location, interval->r0, interval->r1);
    /* Also copy crosses_call to IRLiveInterval for fast lookup later */
    IRLiveInterval *ir_interval = tcc_ir_vreg_live_interval(ir, interval->vreg);
    if (ir_interval)
      ir_interval->crosses_call = interval->crosses_call;
  }
}

/* ============================================================================
 * Interval Management
 * ============================================================================ */

void tcc_ir_live_intervals_clear(TCCIRState *ir)
{
  if (!ir)
    return;

  ir->variables_live_intervals_size = IR_LIVE_INTERVAL_INIT_SIZE;
  ir->temporary_variables_live_intervals_size = IR_LIVE_INTERVAL_INIT_SIZE;
  ir->parameters_live_intervals_size = IR_LIVE_INTERVAL_INIT_SIZE;

  /* Reset interval starts */
  for (int i = 0; i < ir->variables_live_intervals_size; ++i)
  {
    ir->variables_live_intervals[i].start = INTERVAL_NOT_STARTED;
    ir->variables_live_intervals[i].incoming_reg0 = -1;
    ir->variables_live_intervals[i].incoming_reg1 = -1;
  }
  for (int i = 0; i < ir->temporary_variables_live_intervals_size; ++i)
  {
    ir->temporary_variables_live_intervals[i].start = INTERVAL_NOT_STARTED;
    ir->temporary_variables_live_intervals[i].incoming_reg0 = -1;
    ir->temporary_variables_live_intervals[i].incoming_reg1 = -1;
  }
  for (int i = 0; i < ir->parameters_live_intervals_size; ++i)
  {
    ir->parameters_live_intervals[i].start = INTERVAL_NOT_STARTED;
    ir->parameters_live_intervals[i].incoming_reg0 = -1;
    ir->parameters_live_intervals[i].incoming_reg1 = -1;
  }
}

void tcc_ir_live_intervals_init(TCCIRState *ir)
{
  /* Handled by tcc_ir_alloc in core.c */
  (void)ir;
}

void tcc_ir_live_params_extend(TCCIRState *ir)
{
  /* Now handled within live_intervals_compute */
  live_extend_param_intervals(ir);
}

void tcc_ir_live_jumps_extend(TCCIRState *ir)
{
  /* Now handled within live_intervals_compute */
  live_extend_intervals_for_backward_jumps(ir);
}

void tcc_ir_live_interval_extend(IRLiveInterval *interval, int start, int end)
{
  if (!interval)
    return;
  if (interval->start == INTERVAL_NOT_STARTED || interval->start > (uint32_t)start)
    interval->start = start;
  if (interval->end < (uint32_t)end)
    interval->end = end;
}

int tcc_ir_live_has_call_in_range(TCCIRState *ir, int start, int end)
{
  const int instruction_count = ir->next_instruction_index;
  int *call_prefix = NULL;
  int result = 0;

  if (instruction_count > 0)
  {
    call_prefix = (int *)tcc_malloc(sizeof(int) * (instruction_count + 1));
    call_prefix[0] = 0;
    for (int i = 0; i < instruction_count; ++i)
    {
      const TccIrOp op = ir->compact_instructions[i].op;
      const int is_call = (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_BUILTIN_APPLY ||
                           ir_op_is_implicit_call(op))
                              ? 1
                              : 0;
      call_prefix[i + 1] = call_prefix[i] + is_call;
    }
    result = live_has_call_in_range_prefix(call_prefix, start, end, instruction_count);
    tcc_free(call_prefix);
  }
  return result;
}

void tcc_ir_live_call_record(TCCIRState *ir, int instr_idx)
{
  /* Call tracking is now handled by prefix sum computation in liveness_analysis */
  (void)ir;
  (void)instr_idx;
}

void tcc_ir_live_params_avoid_spill(TCCIRState *ir)
{
  /* Legacy - parameter spilling decisions are now handled by the allocator */
  (void)ir;
}

void tcc_ir_live_return_mark(TCCIRState *ir)
{
  /* Legacy - return value handling is done during codegen */
  (void)ir;
}

/* ============================================================================
 * Legacy API Wrappers
 * ============================================================================ */

/* Legacy name for tcc_ir_live_analysis */
void tcc_ir_liveness_analysis(TCCIRState *ir)
{
  tcc_ir_live_analysis(ir);
}

/* Legacy name for tcc_ir_live_intervals_patch */
void tcc_ir_patch_live_intervals_registers(TCCIRState *ir)
{
  tcc_ir_live_intervals_patch(ir);
}

int tcc_ir_move_coalescing(TCCIRState *ir)
{
  LSLiveIntervalState *ls = &ir->ls;
  if (!ls->live_regs_by_instruction || ls->live_regs_by_instruction_size <= 0)
    return 0;

  int coalesced = 0;
  const int n = ir->next_instruction_index;
  const int tbl_size = ls->live_regs_by_instruction_size;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN)
      continue;

    const IROperand src1 = tcc_ir_op_get_src1(ir, q);
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!irop_config[q->op].has_src1 || !tcc_ir_vreg_is_valid(ir, src1.vr))
      continue;
    int32_t dv = irop_get_vreg(dest);
    if (!irop_config[q->op].has_dest || !tcc_ir_vreg_is_valid(ir, dv))
      continue;

    LSLiveInterval *src_iv = NULL, *dst_iv = NULL;
    for (int j = 0; j < ls->next_interval_index; ++j)
    {
      if (ls->intervals[j].vreg == (uint32_t)src1.vr) src_iv = &ls->intervals[j];
      if (ls->intervals[j].vreg == (uint32_t)dv) dst_iv = &ls->intervals[j];
      if (src_iv && dst_iv) break;
    }
    if (!src_iv || !dst_iv) continue;
    if (src_iv->r0 < 0 || dst_iv->r0 < 0) continue;
    if (src_iv->stack_location != 0 || dst_iv->stack_location != 0) continue;
    if (src_iv->r0 == dst_iv->r0) continue;
    if (src_iv->end != (uint32_t)i) continue;

    int src_reg = src_iv->r0;
    int conflict = 0;
    for (int k = i + 1; k <= (int)dst_iv->end && k < tbl_size; ++k)
    {
      if (ls->live_regs_by_instruction[k] & (1u << src_reg))
      {
        conflict = 1;
        break;
      }
    }
    if (conflict) continue;

    int old_reg = dst_iv->r0;
    dst_iv->r0 = src_reg;

    for (int k = (int)dst_iv->start; k <= (int)dst_iv->end && k < tbl_size; ++k)
    {
      ls->live_regs_by_instruction[k] &= ~(1u << old_reg);
      ls->live_regs_by_instruction[k] |= (1u << src_reg);
    }
    coalesced++;
  }

  if (coalesced > 0)
    tcc_ls_recompute_dirty_registers(ls);

  return coalesced;
}

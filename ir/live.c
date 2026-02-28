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
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    const int target = tcc_ir_op_get_dest(ir, q).u.imm32;
    if (target < 0 || target >= n)
      continue;
    if (target >= i)
      continue;
    if (extend_to[target] < i)
      extend_to[target] = i;
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
  int out = 0;
  for (int t = 0; t < n; ++t)
    if (extend_to[t] >= 0)
      targets[out++] = t;

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

    /* Advance scan position and add intervals that start in [scan_pos, target]. */
    for (; scan_pos <= target && scan_pos < n; ++scan_pos)
    {
      for (int node = start_head[scan_pos]; node != -1; node = start_next[node])
      {
        active[active_count++] = start_interval[node];
      }
    }

    /* Compact active set to intervals that are live at 'target'. */
    int w = 0;
    for (int i = 0; i < active_count; ++i)
    {
      IRLiveInterval *interval = active[i];
      if (!interval)
        continue;
      if (interval->start == INTERVAL_NOT_STARTED)
        continue;
      if ((int)interval->start > target)
        continue;
      if ((int)interval->end < target)
        continue;
      active[w++] = interval;
    }
    active_count = w;

    /* Extend all intervals live at the jump target. */
    for (int i = 0; i < active_count; ++i)
    {
      IRLiveInterval *interval = active[i];
      if ((int)interval->end < jump_end)
        interval->end = (uint32_t)jump_end;
    }
  }

  tcc_free(active);
  tcc_free(start_interval);
  tcc_free(start_next);
  tcc_free(start_head);
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
      int dest_is_use = (q->op == TCCIR_OP_STORE ||
                         q->op == TCCIR_OP_STORE_INDEXED ||
                         q->op == TCCIR_OP_STORE_POSTINC);
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
      const int is_call = (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL) ? 1 : 0;
      call_prefix[i + 1] = call_prefix[i] + is_call;
    }
  }

  /* Compute live intervals from the IR after optimizations */
  tcc_ir_live_intervals_compute(ir);

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
    int precolored = (vreg < 4 && !crosses_call) ? vreg : -1;
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
      const int is_call = (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL) ? 1 : 0;
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

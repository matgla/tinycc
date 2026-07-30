/*
 *  TCC IR - Natural-loop detection + IR instruction insertion primitive
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "licm.h"
#include "opt.h"
#include "opt_utils.h"
#include "cfg.h"
#include "core.h"
#include "pool.h"
#include "vreg.h"
#include <string.h>

/* Pattern-based: a backward jump is a back-edge, its target the header, its source the latch; catches plain while/for, not general control flow. */

int tcc_ir_estimate_hoist_budget(TCCIRState *ir, int loop_start, int loop_end, int num_params)
{
  int total_regs = tcc_state->registers_for_allocator;
  if (total_regs <= 0)
    total_regs = 11;

  int n = ir->next_instruction_index;
  if (loop_start < 0) loop_start = 0;
  if (loop_end >= n) loop_end = n - 1;

  /* Max distinct vregs in any WINDOW_SIZE-instruction window approximates peak liveness. */
  #define WINDOW_SIZE 8
  #define BUDGET_MAX_VREGS 512
  int16_t window_buf[WINDOW_SIZE][3];
  int window_head = 0;
  int window_fill = 0;
  int max_pressure = 0;

  uint16_t refcount[BUDGET_MAX_VREGS];
  for (int i = 0; i < BUDGET_MAX_VREGS; i++) refcount[i] = 0;
  int current_distinct = 0;

  for (int i = loop_start; i <= loop_end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (window_fill == WINDOW_SIZE)
    {
      for (int j = 0; j < 3; j++)
      {
        int16_t pos = window_buf[window_head][j];
        if (pos >= 0 && --refcount[pos] == 0) current_distinct--;
      }
      window_head = (window_head + 1) % WINDOW_SIZE;
      window_fill--;
    }

    int slot = (window_head + window_fill) % WINDOW_SIZE;
    IROperand ops[3];
    ops[0] = tcc_ir_op_get_dest(ir, q);
    ops[1] = tcc_ir_op_get_src1(ir, q);
    ops[2] = tcc_ir_op_get_src2(ir, q);
    const int has[3] = {irop_config[q->op].has_dest, irop_config[q->op].has_src1, irop_config[q->op].has_src2};
    for (int j = 0; j < 3; j++)
    {
      int16_t pos = -1;
      if (has[j])
      {
        int32_t vr = irop_get_vreg(ops[j]);
        if (tcc_ir_vreg_is_valid(ir, vr))
        {
          int p = TCCIR_DECODE_VREG_POSITION(vr);
          if (p >= 0 && p < BUDGET_MAX_VREGS)
          {
            pos = (int16_t)p;
            if (refcount[p]++ == 0) current_distinct++;
          }
        }
      }
      window_buf[slot][j] = pos;
    }
    window_fill++;

    if (current_distinct > max_pressure)
      max_pressure = current_distinct;
  }
  #undef WINDOW_SIZE
  #undef BUDGET_MAX_VREGS

  if (max_pressure < 3) max_pressure = 3;
  int budget = total_regs - num_params - max_pressure;
  if (budget < 1) budget = 1;
  return budget;
}

IRLoops *tcc_ir_detect_loops(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return NULL;

  IRLoops *loops = tcc_mallocz(sizeof(IRLoops));
  if (!loops)
    return NULL;

  loops->capacity = LICM_MAX_LOOPS;
  loops->loops = tcc_mallocz(sizeof(IRLoop) * loops->capacity);
  if (!loops->loops)
  {
    tcc_free(loops);
    return NULL;
  }

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);

      /* target >= 0: an unresolved dest decodes negative and would index before compact_instructions. */
      if (target >= 0 && target < i)
      {
        if (loops->num_loops >= loops->capacity)
        {
          LOG_LICM("Warning: too many loops, skipping rest");
          break;
        }

        IRLoop *loop = &loops->loops[loops->num_loops];
        loop->header_idx = target;
        loop->start_idx = target;
        loop->end_idx = i;

        /* Preheader = nearest non-jump predecessor that falls through into the header. */
        int preheader = target - 1;
        while (preheader >= 0)
        {
          IRQuadCompact *ph = &ir->compact_instructions[preheader];
          if (ph->op != TCCIR_OP_JUMP && ph->op != TCCIR_OP_JUMPIF)
          {
            break;
          }
          preheader--;
        }
        loop->preheader_idx = preheader;
        loop->depth = 1;

        int body_size = i - target + 1;
        loop->body_instrs_capacity = body_size;
        loop->body_instrs = tcc_mallocz(sizeof(int) * body_size);

        if (loop->body_instrs)
        {
          for (int j = target; j <= i; j++)
          {
            loop->body_instrs[loop->num_body_instrs++] = j;
          }

          /* Forward jumps out of [target, i] can still land in the body; extend the range. */
          int max_idx = i;
          for (int j = target; j <= max_idx; j++)
          {
            IRQuadCompact *jq = &ir->compact_instructions[j];
            if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
            {
              IROperand jdest = tcc_ir_op_get_dest(ir, jq);
              int jtarget = (int)irop_get_imm64_ex(ir, jdest);
              if (jtarget > max_idx && jtarget < ir->next_instruction_index)
              {
                /* No reachability check; a distance cap stands in for a path back to the header. */
                if (jtarget < target + 50)
                {
                  max_idx = jtarget;
                }
              }
            }
          }

          if (max_idx > i)
          {
            int new_body_size = max_idx - target + 1;
            tcc_free(loop->body_instrs);
            loop->body_instrs = tcc_mallocz(sizeof(int) * new_body_size);
            loop->body_instrs_capacity = new_body_size;
            loop->num_body_instrs = 0;
            for (int j = target; j <= max_idx; j++)
            {
              loop->body_instrs[loop->num_body_instrs++] = j;
            }
          }

          loops->num_loops++;
        }
      }
    }
  }

  /* Same header + smaller end = switch-break back-edge artifact, not a real loop; drop it. */
  for (int i = 0; i < loops->num_loops; i++)
  {
    IRLoop *li = &loops->loops[i];
    for (int j = 0; j < loops->num_loops; j++)
    {
      if (i == j)
        continue;
      IRLoop *lj = &loops->loops[j];
      if (li->header_idx == lj->header_idx && li->end_idx < lj->end_idx)
      {
        tcc_free(li->body_instrs);
        li->body_instrs = NULL;
        li->num_body_instrs = 0;
        li->header_idx = -1; /* sentinel: removed */
        break;
      }
    }
  }
  {
    int dst = 0;
    for (int src = 0; src < loops->num_loops; src++)
    {
      if (loops->loops[src].header_idx >= 0)
      {
        if (dst != src)
          loops->loops[dst] = loops->loops[src];
        dst++;
      }
    }
    loops->num_loops = dst;
  }

  /* depth = 1 + number of loops properly containing this one. */
  for (int i = 0; i < loops->num_loops; i++)
  {
    int depth = 1;
    for (int j = 0; j < loops->num_loops; j++)
    {
      if (i == j)
        continue;
      if (loops->loops[j].start_idx <= loops->loops[i].start_idx &&
          loops->loops[j].end_idx >= loops->loops[i].end_idx &&
          (loops->loops[j].start_idx < loops->loops[i].start_idx ||
           loops->loops[j].end_idx > loops->loops[i].end_idx))
      {
        depth++;
      }
    }
    loops->loops[i].depth = depth;
  }

  if (loops->num_loops > 0)
  {
    LOG_LICM("Detected %d loop(s) (after filtering)", loops->num_loops);
    for (int i = 0; i < loops->num_loops; i++)
    {
      LOG_LICM("Loop %d: header=%d, start=%d, end=%d, preheader=%d, body_instrs=%d, depth=%d", i,
             loops->loops[i].header_idx, loops->loops[i].start_idx, loops->loops[i].end_idx,
             loops->loops[i].preheader_idx, loops->loops[i].num_body_instrs, loops->loops[i].depth);
    }
  }

  return loops;
}

void tcc_ir_free_loops(IRLoops *loops)
{
  if (!loops)
    return;

  if (loops->loops)
  {
    for (int i = 0; i < loops->num_loops; i++)
    {
      if (loops->loops[i].body_instrs)
        tcc_free(loops->loops[i].body_instrs);
    }
    tcc_free(loops->loops);
  }

  tcc_free(loops);
}

int tcc_ir_is_in_loop(IRLoop *loop, int instr_idx)
{
  if (!loop)
    return 0;

  for (int i = 0; i < loop->num_body_instrs; i++)
  {
    if (loop->body_instrs[i] == instr_idx)
      return 1;
  }
  return 0;
}
int tcc_ir_insert_instruction_before(TCCIRState *ir, int before_idx, IRQuadCompact *new_q)
{
  if (ir->next_instruction_index + 1 >= ir->compact_instructions_size)
  {
    int new_size = ir->compact_instructions_size << 1;
    ir->compact_instructions = (IRQuadCompact *)tcc_realloc(ir->compact_instructions, sizeof(IRQuadCompact) * new_size);
    if (!ir->compact_instructions)
      tcc_error("compiler_error: failed to resize compact_instructions");
    ir->compact_instructions_size = new_size;
  }

  for (int i = ir->next_instruction_index; i > before_idx; i--)
  {
    ir->compact_instructions[i] = ir->compact_instructions[i - 1];
  }

  ir->compact_instructions[before_idx] = *new_q;
  ir->next_instruction_index++;

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);
      if (target >= before_idx)
      {
        IROperand new_dest = irop_make_imm32(-1, target + 1, IROP_BTYPE_INT32);
        tcc_ir_op_set_dest(ir, q, new_dest);
      }
    }
  }

  /* SWITCH_TABLE targets live in a side table, not in operands, and desync without this. */
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    if (table->default_target >= before_idx)
      table->default_target += 1;
    if (table->targets)
    {
      for (int j = 0; j < table->num_entries; j++)
      {
        if (table->targets[j] >= before_idx)
          table->targets[j] += 1;
      }
    }
  }

  return before_idx;
}

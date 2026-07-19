/*
 *  TCC IR - Block-boundary and NOP-skip scans
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_utils.h"

uint8_t *ir_opt_build_merge_bitmap(TCCIRState *ir, int n)
{
  uint8_t *is_merge = tcc_mallocz((n + 7) / 8);
  int *pred_count = tcc_mallocz(n * sizeof(int));

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)dest.u.imm32;
      if (target >= 0 && target < n)
      {
        pred_count[target]++;
        if (i > target)
          is_merge[target / 8] |= (1 << (target % 8));
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
          if (target >= 0 && target < n)
            pred_count[target]++;
        }
        if (table->default_target >= 0 && table->default_target < n)
          pred_count[table->default_target]++;
      }
    }
    /* NOP falls through: count that edge or a merge behind DCE-left NOP padding is missed */
    if (i + 1 < n && q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_RETURNVALUE &&
        q->op != TCCIR_OP_RETURNVOID && q->op != TCCIR_OP_SWITCH_TABLE)
    {
      pred_count[i + 1]++;
    }
  }

  for (int i = 0; i < n; i++)
  {
    if (pred_count[i] > 1)
      is_merge[i / 8] |= (1 << (i % 8));
  }

  tcc_free(pred_count);
  return is_merge;
}

void ir_opt_mark_block_starts(TCCIRState *ir, int *block_start_seen, int gen, int n)
{
  block_start_seen[0] = gen;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      const int tgt = (int)irop_get_imm64_ex(ir, dest);
      if (tgt >= 0 && tgt < n)
        block_start_seen[tgt] = gen;
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
          int tgt = table->targets[j];
          if (tgt >= 0 && tgt < n)
            block_start_seen[tgt] = gen;
        }
        if (table->default_target >= 0 && table->default_target < n)
          block_start_seen[table->default_target] = gen;
      }
    }
  }
}

uint8_t *ir_opt_build_block_starts_bitmap(TCCIRState *ir, int n)
{
  uint8_t *bs = tcc_mallocz((n + 7) / 8);
  bs[0] |= 1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      const int tgt = (int)irop_get_imm64_ex(ir, dest);
      if (tgt >= 0 && tgt < n)
        bs[tgt / 8] |= (1 << (tgt % 8));
      if (i + 1 < n)
        bs[(i + 1) / 8] |= (1 << ((i + 1) % 8));
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
          int tgt = table->targets[j];
          if (tgt >= 0 && tgt < n)
            bs[tgt / 8] |= (1 << (tgt % 8));
        }
        if (table->default_target >= 0 && table->default_target < n)
          bs[table->default_target / 8] |= (1 << (table->default_target % 8));
      }
    }
  }
  return bs;
}

int ir_opt_next_non_nop(TCCIRState *ir, int start)
{
  int n = ir->next_instruction_index;
  for (int i = start; i < n; ++i)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
      return i;
  }
  return -1;
}

int ir_skip_nops_forward(TCCIRState *ir, int start, int n)
{
  if (start < 0) return n; /* unresolved target: report not-found */
  for (int j = start; j < n; j++)
    if (ir->compact_instructions[j].op != TCCIR_OP_NOP)
      return j;
  return n;
}

int ir_has_other_jump_to_fast(TCCIRState *ir, const int *jt_cnt,
                              int target, int exclude_idx)
{
  int n = ir->next_instruction_index;
  if (target < 0 || target >= n) return 0;
  int total = jt_cnt[target];
  if (total == 0) return 0;
  if (exclude_idx >= 0 && exclude_idx < n) {
    IRQuadCompact *q = &ir->compact_instructions[exclude_idx];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if ((int)irop_get_imm64_ex(ir, d) == target) total--;
    }
  }
  return total > 0;
}

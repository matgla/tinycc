/*
 *  TCC IR - NOP compaction (jump-target renumbering)
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

/* Compact NOPs — remove NOP instructions and renumber all jump targets. */
int tcc_ir_opt_compact_nops(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  IRQuadCompact *instr = ir->compact_instructions;

  /* Quick check: any NOPs at all? */
  int has_nops = 0;
  for (int i = 0; i < n; i++)
  {
    if (instr[i].op == TCCIR_OP_NOP)
    {
      has_nops = 1;
      break;
    }
  }
  if (!has_nops)
    return 0;

  /* Build old_to_new mapping and compact in one forward pass */
  int *old_to_new = tcc_malloc(n * sizeof(int));
  int write_pos = 0;

  for (int i = 0; i < n; i++)
  {
    if (instr[i].op == TCCIR_OP_NOP)
    {
      old_to_new[i] = -1;
    }
    else
    {
      old_to_new[i] = write_pos;
      if (write_pos != i)
        instr[write_pos] = instr[i];
      write_pos++;
    }
  }

  int removed = n - write_pos;
  if (removed == 0)
  {
    tcc_free(old_to_new);
    return 0;
  }

  /* Fix jump targets in JUMP / JUMPIF instructions.
   * Targets can be in [0, n] — target == n means "epilogue" (one past the
   * last instruction), set by tcc_ir_backpatch_to_here for return jumps. */
  for (int i = 0; i < write_pos; i++)
  {
    IRQuadCompact *q = &instr[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int old_target = (int)irop_get_imm64_ex(ir, dest);
      if (old_target < 0)
        continue;

      int new_target;
      if (old_target >= n)
      {
        /* Epilogue target (past-end): remap to new past-end */
        new_target = write_pos + (old_target - n);
      }
      else
      {
        new_target = old_to_new[old_target];
        if (new_target < 0)
        {
          /* Target was a NOP — find the next non-NOP instruction after it.
           * This shouldn't normally happen (DCE + jump threading should have
           * fixed dangling targets), but handle it defensively. */
          for (int j = old_target + 1; j < n; j++)
          {
            if (old_to_new[j] >= 0)
            {
              new_target = old_to_new[j];
              break;
            }
          }
          if (new_target < 0)
            new_target = write_pos; /* fall through to epilogue */
        }
      }
      if (new_target != old_target)
      {
        IROperand new_dest = irop_make_imm32(-1, new_target, IROP_BTYPE_INT32);
        tcc_ir_op_set_dest(ir, q, new_dest);
      }
    }
  }

  /* Fix switch table targets — same epilogue-aware remapping as jumps */
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    for (int j = 0; j < table->num_entries; j++)
    {
      int old_t = table->targets[j];
      if (old_t < 0)
        continue;
      if (old_t >= n)
      {
        table->targets[j] = write_pos + (old_t - n);
      }
      else
      {
        int new_t = old_to_new[old_t];
        if (new_t < 0)
        {
          for (int k = old_t + 1; k < n; k++)
          {
            if (old_to_new[k] >= 0)
            {
              new_t = old_to_new[k];
              break;
            }
          }
          if (new_t < 0)
            new_t = write_pos;
        }
        table->targets[j] = new_t;
      }
    }
    {
      int old_dt = table->default_target;
      if (old_dt >= 0)
      {
        if (old_dt >= n)
        {
          table->default_target = write_pos + (old_dt - n);
        }
        else
        {
          int new_dt = old_to_new[old_dt];
          if (new_dt < 0)
          {
            for (int k = old_dt + 1; k < n; k++)
            {
              if (old_to_new[k] >= 0)
              {
                new_dt = old_to_new[k];
                break;
              }
            }
            if (new_dt < 0)
              new_dt = write_pos;
          }
          table->default_target = new_dt;
        }
      }
    }
  }

  /* Re-derive is_jump_target flags from scratch */
  for (int i = 0; i < write_pos; i++)
    instr[i].is_jump_target = 0;

  for (int i = 0; i < write_pos; i++)
  {
    IRQuadCompact *q = &instr[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);
      if (target >= 0 && target < write_pos)
        instr[target].is_jump_target = 1;
    }
  }
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    for (int j = 0; j < table->num_entries; j++)
    {
      if (table->targets[j] >= 0 && table->targets[j] < write_pos)
        instr[table->targets[j]].is_jump_target = 1;
    }
    if (table->default_target >= 0 && table->default_target < write_pos)
      instr[table->default_target].is_jump_target = 1;
  }

  ir->next_instruction_index = write_pos;

  tcc_free(old_to_new);

  return removed;
}

int tcc_ir_opt_compact_nops_ex(IROptCtx *ctx)
{
  int removed = tcc_ir_opt_compact_nops(ctx->ir);
  if (removed > 0)
    tcc_ir_opt_ctx_invalidate(ctx);
  return removed;
}

/*
 *  TCC IR - Loop IR mutation primitives
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "licm.h"
#include "opt.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"


/* Insert an instr at pos, shifting later instrs and fixing jump targets; returns pos. */
int insert_instr_at(TCCIRState *ir, int pos, TccIrOp op, IROperand dest, IROperand src1, IROperand src2)
{
  int n = ir->next_instruction_index;

  if (n + 1 >= ir->compact_instructions_size)
  {
    int new_size = ir->compact_instructions_size << 1;
    ir->compact_instructions = (IRQuadCompact *)tcc_realloc(ir->compact_instructions, new_size * sizeof(IRQuadCompact));
    ir->compact_instructions_size = new_size;
  }

  if (ir->iroperand_pool_count + 3 > ir->iroperand_pool_capacity)
  {
    tcc_ir_pool_ensure(ir, 3);
    if (ir->iroperand_pool_count + 3 > ir->iroperand_pool_capacity)
    {
      if (TCC_LOG_IV_SR)
        fprintf(stderr, "[IV_SR] ERROR: iroperand_pool_capacity limit reached\n");
      return -1;
    }
  }

  for (int i = n; i > pos; i--)
  {
    ir->compact_instructions[i] = ir->compact_instructions[i - 1];
  }
  ir->next_instruction_index++;

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (i == pos)
      continue; /* skip the newly inserted slot */
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int64_t jdest_imm = tcc_ir_op_dest_imm(ir, q);
      int target = (int)jdest_imm;
      if (target >= pos)
      {
        IROperand new_dest = irop_make_imm32(-1, target + 1, IROP_BTYPE_INT32);
        tcc_ir_op_set_dest(ir, q, new_dest);
      }
    }
  }
  for (int ti = 0; ti < ir->num_switch_tables; ti++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[ti];
    if (table->default_target >= pos)
      table->default_target++;
    for (int tj = 0; tj < table->num_entries; tj++)
    {
      if (table->targets[tj] >= pos)
        table->targets[tj]++;
    }
  }
  tcc_ir_frame_scope_insert(ir, pos);

  IRQuadCompact *new_q = &ir->compact_instructions[pos];
  new_q->op = op;
  /* Fresh orig_index (bump max) so side tables sized max_orig_index+1 stay covered. */
  new_q->orig_index = ++ir->max_orig_index;
  new_q->is_jump_target = 0;
  new_q->no_unroll = 0;
  new_q->line_num = 0;
  new_q->operand_base = tcc_ir_pool_add(ir, dest);
  tcc_ir_pool_add(ir, src1);
  tcc_ir_pool_add(ir, src2);

  return pos;
}

/* Write an instr into a NOP slot at pos; slot MUST already be NOP. */
void write_instr_at_nop(TCCIRState *ir, int pos, TccIrOp op, IROperand dest, IROperand src1, IROperand src2)
{
  IRQuadCompact *q = &ir->compact_instructions[pos];
  q->op = op;
  /* Fresh orig_index, same reason as insert_instr_at above — and one more: the
   * slot's PREVIOUS occupant's index is still sitting here, so a synthesized
   * instruction would inherit that instruction's side-table annotations.  A
   * rotated loop's new tail CMP landed on a slot whose old occupant was an
   * `orr rd,rn,rm lsl #8`, and came out as `cmp r2,#100 lsl #8` — 25600 — so
   * the loop never exited (pr71083).  Callers that MOVE an instruction (see
   * loop_rotate's body_origs/latch_origs) restore the real index afterwards. */
  q->orig_index = ++ir->max_orig_index;
  q->is_jump_target = 0;
  /* Only write operand slots the instr uses, so pool layout matches codegen. */
  int base = ir->iroperand_pool_count;
  if (irop_config[op].has_dest)
    tcc_ir_pool_add(ir, dest);
  if (irop_config[op].has_src1)
    tcc_ir_pool_add(ir, src1);
  if (irop_config[op].has_src2)
    tcc_ir_pool_add(ir, src2);
  q->operand_base = base;
}

/* Grow a side table keyed by orig_index to hold index `idx`. */
static void *orig_table_grow(void *tab, int *len, int idx, size_t elem)
{
  if (idx < *len)
    return tab;
  int nlen = idx + 1 > 2 * *len ? idx + 1 : 2 * *len;
  tab = tcc_realloc(tab, (size_t)nlen * elem);
  memset((char *)tab + (size_t)*len * elem, 0, (size_t)(nlen - *len) * elem);
  *len = nlen;
  return tab;
}

/* Give instruction orig_index `to` the codegen annotations of `from`: the
 * side tables the backend reads just before codegen (barrel_shifts,
 * shift64_dead_half, zero_half64, bfi_params) are keyed by orig_index, and a
 * clone stamped with a fresh index by write_instr_at_nop has none -- an
 * unrolled `add rd, rn, rm lsl #4` came back as a plain add. */
void tcc_ir_copy_orig_annotations(TCCIRState *ir, int from, int to)
{
  if (from < 0 || to < 0 || from == to)
    return;
  if (ir->barrel_shifts && from < ir->barrel_shifts_len && ir->barrel_shifts[from])
  {
    ir->barrel_shifts = orig_table_grow(ir->barrel_shifts, &ir->barrel_shifts_len, to, 1);
    ir->barrel_shifts[to] = ir->barrel_shifts[from];
  }
  if (ir->shift64_dead_half && from < ir->shift64_dead_half_len && ir->shift64_dead_half[from])
  {
    ir->shift64_dead_half = orig_table_grow(ir->shift64_dead_half, &ir->shift64_dead_half_len, to, 1);
    ir->shift64_dead_half[to] = ir->shift64_dead_half[from];
  }
  if (ir->zero_half64 && from < ir->zero_half64_len && ir->zero_half64[from])
  {
    ir->zero_half64 = orig_table_grow(ir->zero_half64, &ir->zero_half64_len, to, 1);
    ir->zero_half64[to] = ir->zero_half64[from];
  }
  if (ir->bfi_params && from < ir->bfi_params_len && ir->bfi_params[from])
  {
    ir->bfi_params = orig_table_grow(ir->bfi_params, &ir->bfi_params_len, to, sizeof(uint16_t));
    ir->bfi_params[to] = ir->bfi_params[from];
  }
}

/* Write a SELECT into a NOP slot; 4 pool entries: dest, then, else, cond at base+3. */
void write_select_at_nop(TCCIRState *ir, int pos, IROperand dest, IROperand then_val,
                                IROperand else_val, int cond_tok)
{
  IRQuadCompact *q = &ir->compact_instructions[pos];
  q->op = TCCIR_OP_SELECT;
  q->orig_index = ++ir->max_orig_index;
  q->is_jump_target = 0;
  IROperand cond_op = irop_make_imm32(-1, cond_tok, IROP_BTYPE_INT32);
  int base = tcc_ir_pool_add(ir, dest);
  tcc_ir_pool_add(ir, then_val);
  tcc_ir_pool_add(ir, else_val);
  tcc_ir_pool_add(ir, cond_op);
  q->operand_base = base;
}

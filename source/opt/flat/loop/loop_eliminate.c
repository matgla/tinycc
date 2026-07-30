/*
 *  TCC IR - Loop elimination
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


int try_eliminate_loop_symbolic(TCCIRState *ir, IRLoop *loop)
{
  LOG_LOOP_OPT("try_eliminate_loop_symbolic: header=%d start=%d end=%d preheader=%d", loop->header_idx,
               loop->start_idx, loop->end_idx, loop->preheader_idx);

  InductionVar ivs[MAX_IV];
  int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1);
  if (num_ivs < 1)
    return 0;

  /* Find a counter IV (step=1) whose exit condition has a symbolic limit. */
  int cmp_idx = -1, jmpif_idx = -1, cond = -1, exit_target = -1;
  IROperand limit_op = {0};
  InductionVar *counter_iv = NULL;
  for (int k = 0; k < num_ivs; k++)
  {
    int ci, ji, c, et;
    IROperand lop;
    if (find_loop_exit_condition_op(ir, loop, ivs[k].vreg, &ci, &ji, &lop, &c, &et))
    {
      if (irop_is_immediate(lop))
        continue; /* constant limit — try_eliminate_loop handles this. */
      if (ivs[k].step != 1 || ivs[k].init_val != 0)
        continue; /* restrict to the common case */
      counter_iv = &ivs[k];
      cmp_idx = ci; jmpif_idx = ji; limit_op = lop; cond = c; exit_target = et;
      break;
    }
  }
  if (!counter_iv)
  {
    LOG_LOOP_OPT("try_eliminate_loop_symbolic: no symbolic-limit counter IV");
    return 0;
  }

  /* Condition must be one we know how to invert into a step direction. */
  if (cond != TOK_GE && cond != TOK_LT && cond != TOK_GT && cond != TOK_LE)
    return 0;

  /* Verify the loop body is ONLY IV updates / copy-throughs / NOP / JUMP / exit CMP+JUMPIF. */
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_JUMP)
      continue;
    if (i == cmp_idx || i == jmpif_idx)
      continue;
    int is_iv_def = 0;
    for (int k = 0; k < num_ivs; k++)
    {
      if (i == ivs[k].def_idx) { is_iv_def = 1; break; }
    }
    if (is_iv_def)
      continue;
    if (q->op == TCCIR_OP_ASSIGN)
    {
      int is_iv_copy = 0;
      for (int k = 0; k < num_ivs; k++)
      {
        if (i == ivs[k].def_idx - 1)
        {
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          if (irop_get_vreg(src1) == ivs[k].vreg) { is_iv_copy = 1; break; }
        }
      }
      if (is_iv_copy)
        continue;
    }
    LOG_LOOP_OPT("try_eliminate_loop_symbolic: BLOCKED by instr [%d] op=%d", i, q->op);
    return 0;
  }

  /* Compute slots needed; bail if we won't fit in the loop region. */
  int slots_avail = loop->end_idx - loop->start_idx + 1;
  int slots_needed = 0;
  int per_iv_writes[MAX_IV] = {0};
  for (int k = 0; k < num_ivs; k++)
  {
    int used_after = 0;
    for (int j = exit_target; j < ir->next_instruction_index && !used_after; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == ivs[k].vreg)
        used_after = 1;
      if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == ivs[k].vreg)
        used_after = 1;
    }
    if (!used_after)
      continue;
    if (&ivs[k] == counter_iv)
    {
      per_iv_writes[k] = 1; /* ASSIGN V_iv = limit */
      slots_needed += 1;
    }
    else
    {
      /* Accumulator: MUL + optional ADD */
      per_iv_writes[k] = (ivs[k].init_val == 0) ? 1 : 2;
      slots_needed += per_iv_writes[k];
    }
  }
  if (slots_needed > slots_avail)
  {
    LOG_LOOP_OPT("try_eliminate_loop_symbolic: needs %d slots, only %d avail", slots_needed, slots_avail);
    return 0;
  }

  LOG_IR_GEN("[LOOP-ELIM-SYM] Eliminating loop header=%d num_ivs=%d slots_needed=%d", loop->header_idx, num_ivs,
             slots_needed);

  /* Look for the pre-loop entry guard CMP iv,limit / JUMPIF in the preheader. */
  int guard_cmp = -1, guard_jmpif = -1;
  for (int g = counter_iv->init_idx + 1; g < loop->start_idx; g++)
  {
    IRQuadCompact *gq = &ir->compact_instructions[g];
    if (gq->op == TCCIR_OP_CMP)
    {
      IROperand gsrc1 = tcc_ir_op_get_src1(ir, gq);
      if (irop_get_vreg(gsrc1) == counter_iv->vreg && g + 1 < loop->start_idx)
      {
        IRQuadCompact *gjq = &ir->compact_instructions[g + 1];
        if (gjq->op == TCCIR_OP_JUMPIF)
        {
          guard_cmp = g;
          guard_jmpif = g + 1;
          break;
        }
      }
    }
  }

  /* Count used-after accumulators (non-counter) and detect the simple case. */
  int num_acc_used = 0;
  int single_acc_idx = -1;
  int counter_used_after = 0;
  for (int k = 0; k < num_ivs; k++)
  {
    if (per_iv_writes[k] == 0)
      continue;
    if (&ivs[k] == counter_iv)
      counter_used_after = 1;
    else
    {
      num_acc_used++;
      single_acc_idx = k;
    }
  }

  int use_select_path =
      (guard_cmp >= 0 && guard_jmpif >= 0 && num_acc_used == 1 && !counter_used_after &&
       ivs[single_acc_idx].init_val == 0);

  /* Fallback writes unconditional final IVs — wrong for a symbolic limit's zero-trip case; only the SELECT path guards it, so bail before NOPing the body. */
  if (!use_select_path)
  {
    LOG_LOOP_OPT("try_eliminate_loop_symbolic: bail — fallback can't guard the zero-trip case for a symbolic limit");
    return 0;
  }

  /* NOP the loop body in both paths. */
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
    ir->compact_instructions[i].op = TCCIR_OP_NOP;

  if (use_select_path)
  {
    InductionVar *acc_iv = &ivs[single_acc_idx];
    IROperand step_imm = irop_make_imm32(-1, acc_iv->step, IROP_BTYPE_INT32);
    IROperand zero_imm = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
    IROperand acc_dest = irop_make_vreg(acc_iv->vreg, IROP_BTYPE_INT32);

    /* Layout CMP->MUL->SELECT; CMP before MUL frees the limit reg so V_acc can reuse it. */
    ir->compact_instructions[counter_iv->init_idx].op = TCCIR_OP_NOP;
    write_instr_at_nop(ir, counter_iv->init_idx, TCCIR_OP_CMP, (IROperand){0}, limit_op, zero_imm);

    {
      IRQuadCompact *gq = &ir->compact_instructions[guard_cmp];
      gq->op = TCCIR_OP_MUL;
      /* MUL needs a dest; CMP only set src1/src2, so re-add a dest slot. */
      int new_base = tcc_ir_pool_add(ir, acc_dest);
      tcc_ir_pool_add(ir, limit_op);
      tcc_ir_pool_add(ir, step_imm);
      gq->operand_base = new_base;
    }

    if (acc_iv->init_idx >= 0 && acc_iv->init_idx != counter_iv->init_idx &&
        acc_iv->init_idx != guard_cmp)
      ir->compact_instructions[acc_iv->init_idx].op = TCCIR_OP_NOP;

    ir->compact_instructions[guard_jmpif].op = TCCIR_OP_NOP;
    write_select_at_nop(ir, guard_jmpif, acc_dest, acc_dest, zero_imm, TOK_GT);

    return 1;
  }

  /* Fallback: ASSIGN-based closed form for cases the SELECT path doesn't cover. */
  int write_pos = loop->start_idx;
  for (int k = 0; k < num_ivs; k++)
  {
    if (per_iv_writes[k] == 0)
      continue;
    InductionVar *iv = &ivs[k];
    IROperand acc_dest = irop_make_vreg(iv->vreg, IROP_BTYPE_INT32);
    if (iv == counter_iv)
    {
      write_instr_at_nop(ir, write_pos++, TCCIR_OP_ASSIGN, acc_dest, limit_op, (IROperand){0});
    }
    else
    {
      IROperand step_imm = irop_make_imm32(-1, iv->step, IROP_BTYPE_INT32);
      if (iv->init_val == 0)
      {
        write_instr_at_nop(ir, write_pos++, TCCIR_OP_MUL, acc_dest, limit_op, step_imm);
      }
      else
      {
        write_instr_at_nop(ir, write_pos++, TCCIR_OP_MUL, acc_dest, limit_op, step_imm);
        IROperand init_imm = irop_make_imm32(-1, iv->init_val, IROP_BTYPE_INT32);
        IROperand acc_src = irop_make_vreg(iv->vreg, IROP_BTYPE_INT32);
        write_instr_at_nop(ir, write_pos++, TCCIR_OP_ADD, acc_dest, acc_src, init_imm);
      }
    }
  }

  return 1;
}

/* Eliminate a loop whose body is only IV updates by computing final IV values. */
int try_eliminate_loop(TCCIRState *ir, IRLoop *loop)
{
  LOG_LOOP_OPT("try_eliminate_loop: header=%d start=%d end=%d preheader=%d", loop->header_idx, loop->start_idx,
               loop->end_idx, loop->preheader_idx);
  InductionVar ivs[MAX_IV];
  int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1 /* allow copy-through */);
  if (num_ivs < 1)
  {
    LOG_LOOP_OPT("try_eliminate_loop: no IVs found, giving up");
    return 0;
  }

  /* Find the primary IV — the one referenced in the loop exit condition. */
  int cmp_idx, jmpif_idx, limit, cond, exit_target;
  InductionVar *primary_iv = NULL;
  for (int k = 0; k < num_ivs; k++)
  {
    if (find_loop_exit_condition(ir, loop, ivs[k].vreg, &cmp_idx, &jmpif_idx, &limit, &cond, &exit_target))
    {
      primary_iv = &ivs[k];
      LOG_LOOP_OPT("try_eliminate_loop: primary IV=VAR%d (init=%d, step=%d)",
                   TCCIR_DECODE_VREG_POSITION(primary_iv->vreg), primary_iv->init_val, primary_iv->step);
      break;
    }
  }
  if (!primary_iv)
  {
    LOG_LOOP_OPT("try_eliminate_loop: no primary IV (exit condition not found for any IV)");
    return 0;
  }

  int trip_count = compute_trip_count(primary_iv->init_val, limit, primary_iv->step, cond);
  if (trip_count <= 0)
  {
    LOG_LOOP_OPT("try_eliminate_loop: trip_count=%d (invalid), giving up", trip_count);
    return 0;
  }
  LOG_LOOP_OPT("try_eliminate_loop: trip_count=%d limit=%d", trip_count, limit);

  /* Verify the loop body contains ONLY IV updates. */
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_JUMP)
      continue;
    if (i == cmp_idx || i == jmpif_idx)
      continue;

    int is_iv_def = 0;
    for (int k = 0; k < num_ivs; k++)
    {
      if (i == ivs[k].def_idx)
      {
        is_iv_def = 1;
        break;
      }
    }
    if (is_iv_def)
      continue;

    /* Copy-through temp: ASSIGN of an IV vreg immediately before that IV's def. */
    if (q->op == TCCIR_OP_ASSIGN)
    {
      int is_iv_copy = 0;
      for (int k = 0; k < num_ivs; k++)
      {
        if (i == ivs[k].def_idx - 1)
        {
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          if (irop_get_vreg(src1) == ivs[k].vreg)
          {
            is_iv_copy = 1;
            break;
          }
        }
      }
      if (is_iv_copy)
        continue;
    }

    /* Any other instruction — loop has side effects, can't eliminate */
    LOG_LOOP_OPT("try_eliminate_loop: BLOCKED by instr [%d] op=%d (not IV/NOP/JUMP/CMP)", i, q->op);
    return 0;
  }

  /* Also verify no backward jumps escape the loop */
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, jd);
      if (target < loop->start_idx)
        return 0;
    }
  }

  LOG_IR_GEN("[LOOP-ELIM] Eliminating loop header=%d trip_count=%d num_ivs=%d", loop->header_idx, trip_count, num_ivs);

  for (int i = loop->start_idx; i <= loop->end_idx; i++)
    ir->compact_instructions[i].op = TCCIR_OP_NOP;

  /* Write final value assignments for all IVs used after the loop */
  int write_pos = loop->start_idx;
  for (int k = 0; k < num_ivs; k++)
  {
    int iv_final = ivs[k].init_val + trip_count * ivs[k].step;

    int used_after = 0;
    for (int j = exit_target; j < ir->next_instruction_index; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == ivs[k].vreg)
      {
        used_after = 1;
        break;
      }
      if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == ivs[k].vreg)
      {
        used_after = 1;
        break;
      }
    }

    if (used_after && write_pos <= loop->end_idx)
    {
      IROperand dest = irop_make_vreg(ivs[k].vreg, IROP_BTYPE_INT32);
      IROperand val = irop_make_imm32(-1, iv_final, IROP_BTYPE_INT32);
      write_instr_at_nop(ir, write_pos++, TCCIR_OP_ASSIGN, dest, val, (IROperand){0});
    }

    /* NOP the initialization too */
    if (ivs[k].init_idx >= 0)
    {
      ir->compact_instructions[ivs[k].init_idx].op = TCCIR_OP_NOP;

      /* Bottom-tested loops: NOP the dead pre-loop guard (trip_count>0) so it doesn't read the NOP'd IV init. */
      for (int g = ivs[k].init_idx + 1; g < loop->start_idx; g++)
      {
        IRQuadCompact *gq = &ir->compact_instructions[g];
        if (gq->op == TCCIR_OP_CMP)
        {
          IROperand gsrc1 = tcc_ir_op_get_src1(ir, gq);
          if (irop_get_vreg(gsrc1) == ivs[k].vreg && g + 1 < loop->start_idx)
          {
            IRQuadCompact *gjq = &ir->compact_instructions[g + 1];
            if (gjq->op == TCCIR_OP_JUMPIF)
            {
              LOG_LOOP_OPT("NOP'ing pre-loop guard CMP@%d + JUMPIF@%d", g, g + 1);
              gq->op = TCCIR_OP_NOP;
              gjq->op = TCCIR_OP_NOP;
            }
          }
        }
      }
    }
  }

  /* If fall-through doesn't reach exit_target, restore the exit edge with a JUMP (seed 198468 else-arm wrong-code). */
  int need_exit_jump = 0;
  {
    int n2 = ir->next_instruction_index;
    int ft = loop->end_idx + 1;
    while (ft < n2 && ir->compact_instructions[ft].op == TCCIR_OP_NOP)
      ft++;
    int et = exit_target;
    while (et < n2 && ir->compact_instructions[et].op == TCCIR_OP_NOP)
      et++;
    if (ft != et)
      need_exit_jump = 1;
  }
  if (need_exit_jump)
  {
    for (int i = write_pos; i <= loop->end_idx; i++)
    {
      if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      {
        IROperand exit_dest = irop_make_imm32(-1, exit_target, IROP_BTYPE_INT32);
        write_instr_at_nop(ir, i, TCCIR_OP_JUMP, exit_dest, (IROperand){0}, (IROperand){0});
        if (exit_target >= 0 && exit_target < ir->next_instruction_index)
          ir->compact_instructions[exit_target].is_jump_target = 1;
        break;
      }
    }
  }

  return 1;
}

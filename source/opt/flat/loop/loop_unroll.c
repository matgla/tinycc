/*
 *  TCC IR - Loop unrolling
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


/* loops/loop_idx let sibling loop records be patched when the IR grows; NULL/0 if unused. */
int try_unroll_loop_ex(TCCIRState *ir, IRLoop *loop, IRLoops *loops, int loop_idx)
{
  LOG_LOOP_OPT("try_unroll_loop: header=%d start=%d end=%d preheader=%d", loop->header_idx, loop->start_idx,
               loop->end_idx, loop->preheader_idx);

  if (loop->end_idx >= 0 && loop->end_idx < ir->next_instruction_index &&
      ir->compact_instructions[loop->end_idx].no_unroll)
  {
    LOG_LOOP_OPT("try_unroll_loop: back-edge marked no_unroll (rerolled), skipping");
    return 0;
  }

  InductionVar ivs[MAX_IV];
  int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1 /* allow copy-through */);
  if (num_ivs < 1)
  {
    LOG_LOOP_OPT("try_unroll_loop: no IVs found, giving up");
    return 0;
  }

  /* Primary IV = the one in the exit condition; accumulators match the pattern but are body instrs. */
  int cmp_idx, jmpif_idx, limit, cond, exit_target;
  InductionVar *iv = NULL;
  for (int k = 0; k < num_ivs; k++)
  {
    if (find_loop_exit_condition(ir, loop, ivs[k].vreg, &cmp_idx, &jmpif_idx, &limit, &cond, &exit_target))
    {
      iv = &ivs[k];
      break;
    }
  }
  if (!iv)
  {
    LOG_LOOP_OPT("try_unroll_loop: no primary IV (exit condition not found)");
    return 0;
  }

  int trip_count = compute_trip_count(iv->init_val, limit, iv->step, cond);
  if (trip_count <= 0 || trip_count > UNROLL_MAX_TRIP_COUNT)
  {
    LOG_LOOP_OPT("try_unroll_loop: trip_count=%d (invalid or > %d), giving up", trip_count, UNROLL_MAX_TRIP_COUNT);
    return 0;
  }

  int ret = 0;
  size_t _usz = UNROLL_MAX_BODY_INSNS * (2 * sizeof(int) + 3 * sizeof(IROperand));
  char *_ubuf = (char *)tcc_mallocz(_usz);
  char *_up = _ubuf;
  int *body_indices = (int *)_up; _up += UNROLL_MAX_BODY_INSNS * sizeof(int);
  int *body_ops = (int *)_up; _up += UNROLL_MAX_BODY_INSNS * sizeof(int);
  IROperand *body_dests = (IROperand *)_up; _up += UNROLL_MAX_BODY_INSNS * sizeof(IROperand);
  IROperand *body_src1s = (IROperand *)_up; _up += UNROLL_MAX_BODY_INSNS * sizeof(IROperand);
  IROperand *body_src2s = (IROperand *)_up;

  int body_count = collect_body_instructions(ir, loop, iv->vreg, cmp_idx, jmpif_idx, iv->def_idx, body_indices,
                                             UNROLL_MAX_BODY_INSNS);
  if (body_count <= 0 || body_count > UNROLL_MAX_BODY_INSNS)
  {
    LOG_LOOP_OPT("try_unroll_loop: body_count=%d (invalid or > %d), giving up", body_count, UNROLL_MAX_BODY_INSNS);
    goto unroll_cleanup;
  }

  int total_insns = trip_count * body_count;
  if (total_insns > UNROLL_MAX_TOTAL_INSNS)
  {
    LOG_LOOP_OPT("try_unroll_loop: total_insns=%d > %d, giving up", total_insns, UNROLL_MAX_TOTAL_INSNS);
    goto unroll_cleanup;
  }
  for (int b = 0; b < body_count; b++)
  {
    IRQuadCompact *bq = &ir->compact_instructions[body_indices[b]];
    int op = bq->op;
    body_ops[b] = op;
    body_dests[b] = (IROperand){0};
    body_src1s[b] = (IROperand){0};
    body_src2s[b] = (IROperand){0};
    if (irop_config[op].has_dest)
      body_dests[b] = ir->iroperand_pool[bq->operand_base];
    if (irop_config[op].has_src1)
      body_src1s[b] = ir->iroperand_pool[bq->operand_base + irop_config[op].has_dest];
    if (irop_config[op].has_src2)
      body_src2s[b] = ir->iroperand_pool[bq->operand_base + irop_config[op].has_dest + irop_config[op].has_src1];
  }

  /* Use only [start_idx..end_idx]; the extended body_instrs can include post-loop instrs. */
  int loop_end = loop->end_idx;

  /* Reserve an explicit exit JUMP when exit_target is not the physical successor of the region. */
  int need_exit_jump = 0;
  {
    int n2 = ir->next_instruction_index;
    int ft = loop_end + 1;
    while (ft < n2 && ir->compact_instructions[ft].op == TCCIR_OP_NOP)
      ft++;
    int et = exit_target;
    while (et < n2 && ir->compact_instructions[et].op == TCCIR_OP_NOP)
      et++;
    if (ft != et)
      need_exit_jump = 1;
  }

  /* Grow the region with NOPs when too small; indices in [start_idx..loop_end] are stable, exit_target must be shifted manually. */
  int avail_slots = loop_end - loop->start_idx + 1;
  int needed_slots = total_insns + 1 + need_exit_jump; /* +1 IV final, +1 exit JUMP */
  /* Only grow the IR for single-loop functions; multi-loop cross-book-keeping leaves regalloc state stale. */
  if (needed_slots > avail_slots && (!loops || loops->num_loops != 1))
    goto unroll_cleanup;
  if (needed_slots > avail_slots)
  {
    int extra = needed_slots - avail_slots;
    int insert_pos = loop_end + 1;
    int orig_end = loop->end_idx;
    IROperand none_op = (IROperand){0};
    for (int k = 0; k < extra; k++)
    {
      if (insert_instr_at(ir, insert_pos, TCCIR_OP_NOP, none_op, none_op, none_op) < 0)
        goto unroll_cleanup;
    }
    loop_end += extra;
    if (exit_target > orig_end)
      exit_target += extra;
    loop->end_idx = loop_end;
    /* Shift sibling loop records past the insertion point, else a later unroll corrupts them. */
    if (loops)
    {
      for (int li = 0; li < loops->num_loops; li++)
      {
        if (li == loop_idx)
          continue;
        IRLoop *other = &loops->loops[li];
        if (other->start_idx < 0)
          continue;
        if (other->start_idx > orig_end)
          other->start_idx += extra;
        if (other->end_idx > orig_end)
          other->end_idx += extra;
        if (other->header_idx > orig_end)
          other->header_idx += extra;
        if (other->preheader_idx > orig_end)
          other->preheader_idx += extra;
        for (int b = 0; b < other->num_body_instrs; b++)
        {
          if (other->body_instrs[b] > orig_end)
            other->body_instrs[b] += extra;
        }
      }
    }
  }

  for (int i = loop->start_idx; i <= loop_end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP && i != loop->end_idx)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, jd);
      if (target < i && target < loop->start_idx)
        goto unroll_cleanup; /* backward jump escaping the loop: nested or malformed */
    }
  }

  LOG_IR_GEN("[UNROLL] Unrolling loop header=%d trip_count=%d body_count=%d", loop->header_idx, trip_count, body_count);

  for (int i = loop->start_idx; i <= loop_end; i++)
    ir->compact_instructions[i].op = TCCIR_OP_NOP;

  if (iv->init_idx >= 0)
    ir->compact_instructions[iv->init_idx].op = TCCIR_OP_NOP;

  /* Rotated loops have a dead pre-loop guard CMP+JUMPIF; NOP it since the IV init is now gone. */
  if (iv->init_idx >= 0)
  {
    for (int g = iv->init_idx + 1; g < loop->start_idx; g++)
    {
      IRQuadCompact *gq = &ir->compact_instructions[g];
      if (gq->op == TCCIR_OP_CMP)
      {
        IROperand gsrc1 = tcc_ir_op_get_src1(ir, gq);
        if (irop_get_vreg(gsrc1) == iv->vreg && g + 1 < loop->start_idx)
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

  /* Rename body-local TEMPs per iteration; otherwise reused TEMP positions make global use-count passes bail. */
#define UNROLL_MAX_RENAME 16
  int rename_old_pos[UNROLL_MAX_RENAME];
  int rename_count = 0;
  for (int b = 0; b < body_count && rename_count < UNROLL_MAX_RENAME; b++)
  {
    int op = body_ops[b];
    if (!irop_config[op].has_dest)
      continue;
    /* These dests are uses (store addresses / param values), not body defs. */
    if (op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_STORE_POSTINC ||
        op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_FUNCPARAMVOID)
      continue;
    int32_t vr = irop_get_vreg(body_dests[b]);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    int seen = 0;
    for (int r = 0; r < rename_count; r++)
      if (rename_old_pos[r] == pos) { seen = 1; break; }
    if (seen)
      continue;
    rename_old_pos[rename_count++] = pos;
  }
  /* Reject any TEMP referenced outside the loop body (value escapes); mark it -1. */
  if (rename_count > 0)
  {
    int n_all = ir->next_instruction_index;
    for (int i = 0; i < n_all; i++)
    {
      if (i >= loop->start_idx && i <= loop_end)
        continue;
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      for (int slot = 0; slot < 3; slot++)
      {
        IROperand op;
        if (slot == 0)
        {
          if (!irop_config[q->op].has_dest) continue;
          op = tcc_ir_op_get_dest(ir, q);
        }
        else if (slot == 1)
        {
          if (!irop_config[q->op].has_src1) continue;
          op = tcc_ir_op_get_src1(ir, q);
        }
        else
        {
          if (!irop_config[q->op].has_src2) continue;
          op = tcc_ir_op_get_src2(ir, q);
        }
        int32_t vr = irop_get_vreg(op);
        if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
          continue;
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        for (int r = 0; r < rename_count; r++)
          if (rename_old_pos[r] == pos) rename_old_pos[r] = -1;
      }
    }
  }

  int write_pos = loop->start_idx;

  for (int k = 0; k < trip_count; k++)
  {
    int rename_new_vreg[UNROLL_MAX_RENAME];
    for (int r = 0; r < rename_count; r++)
      rename_new_vreg[r] = (rename_old_pos[r] < 0) ? -1 : tcc_ir_vreg_alloc_temp(ir);

    for (int b = 0; b < body_count; b++)
    {
      int saved_op = body_ops[b];
      IROperand dest = body_dests[b];
      IROperand src1 = body_src1s[b];
      IROperand src2 = body_src2s[b];

      int iv_val = iv->init_val + k * iv->step;
      IROperand iv_const = irop_make_imm32(-1, iv_val, IROP_BTYPE_INT32);

      if (irop_get_vreg(src1) == iv->vreg)
        src1 = iv_const;
      if (irop_get_vreg(src2) == iv->vreg)
        src2 = iv_const;

      for (int r = 0; r < rename_count; r++)
      {
        if (rename_old_pos[r] < 0)
          continue;
        int32_t old_pos = rename_old_pos[r];
        int32_t new_vr = rename_new_vreg[r];
        if (irop_config[saved_op].has_dest)
        {
          int32_t vr = irop_get_vreg(dest);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
              TCCIR_DECODE_VREG_POSITION(vr) == old_pos)
            irop_set_vreg(&dest, new_vr);
        }
        {
          int32_t vr = irop_get_vreg(src1);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
              TCCIR_DECODE_VREG_POSITION(vr) == old_pos)
            irop_set_vreg(&src1, new_vr);
        }
        {
          int32_t vr = irop_get_vreg(src2);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
              TCCIR_DECODE_VREG_POSITION(vr) == old_pos)
            irop_set_vreg(&src2, new_vr);
        }
      }

      while (write_pos <= loop_end && ir->compact_instructions[write_pos].op != TCCIR_OP_NOP)
        write_pos++;

      if (write_pos > loop_end)
        goto unroll_cleanup; /* should not happen: avail_slots check prevents it */

      write_instr_at_nop(ir, write_pos, saved_op, dest, src1, src2);
      write_pos++;
    }
  }
#undef UNROLL_MAX_RENAME

  /* If the IV is used after the loop, materialize its final value. */
  {
    int iv_used_after = 0;
    for (int i = exit_target; i < ir->next_instruction_index; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s1) == iv->vreg)
        {
          iv_used_after = 1;
          break;
        }
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, q);
        if (irop_get_vreg(s2) == iv->vreg)
        {
          iv_used_after = 1;
          break;
        }
      }
    }

    if (iv_used_after)
    {
      int iv_final = iv->init_val + trip_count * iv->step;
      IROperand iv_dest = irop_make_vreg(iv->vreg, IROP_BTYPE_INT32);
      IROperand iv_val_op = irop_make_imm32(-1, iv_final, IROP_BTYPE_INT32);

      for (int i = write_pos; i <= loop_end; i++)
      {
        if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
        {
          write_instr_at_nop(ir, i, TCCIR_OP_ASSIGN, iv_dest, iv_val_op, (IROperand){0});
          write_pos = i + 1;
          break;
        }
      }
    }
  }

  /* Emit the exit branch when fall-through misses exit_target; after the IV-final assignment so it's still computed. */
  if (need_exit_jump)
  {
    for (int i = write_pos; i <= loop_end; i++)
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

  ret = 1;

unroll_cleanup:
  tcc_free(_ubuf);
  return ret;
}

/*
 *  TCC IR - Infinite Loop Simplification
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

int tcc_ir_opt_infinite_loop_simplify(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 3)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    tcc_ir_free_loops(loops);
    return 0;
  }

  int changes = 0;

  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];

    int back_edge_idx = -1;
    int is_infinite = 1;
    int has_call = 0;
    int has_volatile = 0;

    for (int bi = 0; bi < loop->num_body_instrs; bi++)
    {
      int idx = loop->body_instrs[bi];
      IRQuadCompact *q = &ir->compact_instructions[idx];
      if (q->op == TCCIR_OP_NOP)
        continue;

      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID ||
          q->op == TCCIR_OP_CALLSEQ_BEGIN || q->op == TCCIR_OP_INLINE_ASM)
      {
        has_call = 1;
        break;
      }
      if (q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
      {
        is_infinite = 0;
        break;
      }
      if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE)
      {
        is_infinite = 0;
        break;
      }

      /* Check for volatile operands */
      for (int k = 0; k <= 2; k++)
      {
        IROperand op;
        if (k == 0 && irop_config[q->op].has_dest)
          op = tcc_ir_op_get_dest(ir, q);
        else if (k == 1 && irop_config[q->op].has_src1)
          op = tcc_ir_op_get_src1(ir, q);
        else if (k == 2 && irop_config[q->op].has_src2)
          op = tcc_ir_op_get_src2(ir, q);
        else
          continue;
        if (op.is_sym)
        {
          Sym *sym = irop_get_sym_ex(ir, op);
          if (sym && (sym->type.t & VT_VOLATILE))
            has_volatile = 1;
        }
      }

      /* The symbol scan above misses a volatile MEMBER of a non-volatile
       * symbol; only the access itself carries that. */
      if (tcc_ir_instr_access_is_volatile(ir, q))
        has_volatile = 1;

      if (q->op == TCCIR_OP_JUMPIF)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int target = (int)dest.u.imm32;
        if (target < loop->start_idx || target > loop->end_idx)
        {
          is_infinite = 0;
          break;
        }
      }
      if (q->op == TCCIR_OP_JUMP)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int target = (int)dest.u.imm32;
        if (target == loop->header_idx)
          back_edge_idx = idx;
        else if (target < loop->start_idx || target > loop->end_idx)
        {
          is_infinite = 0;
          break;
        }
      }
    }

    if (!is_infinite || has_call || has_volatile || back_edge_idx < 0)
      continue;

    /* Analyze stores in the loop body. Check if all are dead or hoistable. */
    int all_stores_dead = 1;

    /* Track which globals get constant stores (for hoisting) */
#define MAX_HOIST 8
    struct { Sym *sym; int64_t addend; IROperand value; int store_idx; } hoist[MAX_HOIST];
    int nhoist = 0;

    for (int bi = 0; bi < loop->num_body_instrs; bi++)
    {
      int idx = loop->body_instrs[bi];
      IRQuadCompact *q = &ir->compact_instructions[idx];

      if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED &&
          q->op != TCCIR_OP_STORE_POSTINC)
        continue;

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dest_vr = irop_get_vreg(dest);

      /* Store to a local or parameter (direct vreg store, not pointer deref) */
      if (q->op == TCCIR_OP_STORE && dest_vr >= 0 && !dest.is_lval && !dest.is_sym)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vr);
        if (interval && interval->addrtaken)
          {
            /* Address taken: check if the feeding LEA is inside the loop. */
            int lea_in_loop = 0;
            for (int j = 0; j < n; j++)
            {
              IRQuadCompact *lq = &ir->compact_instructions[j];
              if (lq->op == TCCIR_OP_LEA || lq->op == TCCIR_OP_ASSIGN)
              {
                if (irop_config[lq->op].has_src1)
                {
                  IROperand s1 = tcc_ir_op_get_src1(ir, lq);
                  if (!s1.is_lval && irop_get_vreg(s1) == dest_vr)
                  {
                    for (int bk = 0; bk < loop->num_body_instrs; bk++)
                    {
                      if (loop->body_instrs[bk] == j)
                      {
                        lea_in_loop = 1;
                        break;
                      }
                    }
                  }
                }
              }
            }
            if (lea_in_loop)
            {
              all_stores_dead = 0;
              break;
            }
          }
        continue;
      }

      if (q->op == TCCIR_OP_STORE && dest.is_sym && dest.is_lval)
      {
        /* Store to global. Check if value is loop-invariant (constant). */
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
        if (!sr || !sr->sym)
        {
          all_stores_dead = 0;
          break;
        }
        if ((sr->sym->type.t & VT_VOLATILE) || tcc_ir_instr_access_is_volatile(ir, q))
        {
          all_stores_dead = 0;
          break;
        }
        if (irop_is_immediate(src1) && !src1.is_sym)
        {
          /* Constant store to non-volatile global → hoistable */
          if (nhoist < MAX_HOIST)
          {
            hoist[nhoist].sym = sr->sym;
            hoist[nhoist].addend = sr->addend;
            hoist[nhoist].value = src1;
            hoist[nhoist].store_idx = idx;
            nhoist++;
          }
          continue;
        }
        /* Non-constant store: allow a signed RMW (++m) where overflow is UB. */
        int32_t val_vr = irop_get_vreg(src1);
        int is_signed_rmw = 0;
        int dbtype = irop_get_btype(dest);
        if (val_vr >= 0 && !src1.is_lval && !src1.is_sym &&
            (dbtype == IROP_BTYPE_INT32 || dbtype == IROP_BTYPE_INT16 ||
             dbtype == IROP_BTYPE_INT8) &&
            !dest.is_unsigned)
        {
          for (int bj = 0; bj < loop->num_body_instrs; bj++)
          {
            int didx = loop->body_instrs[bj];
            IRQuadCompact *dq = &ir->compact_instructions[didx];
            if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_SUB)
              continue;
            if (!irop_config[dq->op].has_dest)
              continue;
            IROperand dd = tcc_ir_op_get_dest(ir, dq);
            if (irop_get_vreg(dd) != val_vr)
              continue;
            IROperand ds1 = tcc_ir_op_get_src1(ir, dq);
            IROperand ds2 = tcc_ir_op_get_src2(ir, dq);
            if (ds1.is_sym && ds1.is_lval && irop_is_immediate(ds2) && !ds2.is_sym)
            {
              IRPoolSymref *dsr = irop_get_symref_ex(ir, ds1);
              if (dsr && dsr->sym == sr->sym && dsr->addend == sr->addend)
                is_signed_rmw = 1;
            }
            break;
          }
        }
        if (!is_signed_rmw)
        {
          all_stores_dead = 0;
          break;
        }
        continue;
      }

      /* STORE_INDEXED, STORE_POSTINC, or unknown STORE pattern */
      all_stores_dead = 0;
      break;
    }

    if (!all_stores_dead)
      continue;

    /* All stores are dead or hoistable. Simplify the loop. */

    /* Step 1: move hoisted constant stores to before the loop, NOP originals. */
    for (int h = 0; h < nhoist; h++)
    {
      /* Use the loop's preheader slot if present; otherwise we can't hoist. */
      int preheader = loop->preheader_idx;
      if (preheader < 0)
      {
        /* Try to find a NOP slot before the header */
        for (int j = loop->header_idx - 1; j >= 0; j--)
        {
          if (ir->compact_instructions[j].op == TCCIR_OP_NOP)
          {
            preheader = j;
            break;
          }
          break;
        }
      }
      if (preheader >= 0 && ir->compact_instructions[preheader].op == TCCIR_OP_NOP)
      {
        ir->compact_instructions[preheader] = ir->compact_instructions[hoist[h].store_idx];
        ir->compact_instructions[preheader].is_jump_target =
          ir->compact_instructions[hoist[h].store_idx].is_jump_target ? 1 : 0;
        int src_base = ir->compact_instructions[hoist[h].store_idx].operand_base;
        int dst_base = ir->compact_instructions[preheader].operand_base;
        int nops_count = (irop_config[TCCIR_OP_STORE].has_dest ? 1 : 0) +
                         (irop_config[TCCIR_OP_STORE].has_src1 ? 1 : 0) +
                         (irop_config[TCCIR_OP_STORE].has_src2 ? 1 : 0);
        for (int k = 0; k < nops_count; k++)
          ir->iroperand_pool[dst_base + k] = ir->iroperand_pool[src_base + k];
      }
      ir->compact_instructions[hoist[h].store_idx].op = TCCIR_OP_NOP;
    }

    /* Step 2: NOP the remaining body instructions except the back-edge jump. */
    for (int bi = 0; bi < loop->num_body_instrs; bi++)
    {
      int idx = loop->body_instrs[bi];
      IRQuadCompact *q = &ir->compact_instructions[idx];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (idx == back_edge_idx)
        continue;
      q->op = TCCIR_OP_NOP;
    }

    /* Convert the back-edge to a self-jump at the loop header */
    ir->compact_instructions[loop->header_idx].op = TCCIR_OP_JUMP;
    ir->compact_instructions[loop->header_idx].is_jump_target = 1;
    IROperand self = irop_make_imm32(-1, loop->header_idx, IROP_BTYPE_INT32);
    tcc_ir_set_dest(ir, loop->header_idx, self);
    tcc_ir_set_src1(ir, loop->header_idx, IROP_NONE);
    tcc_ir_set_src2(ir, loop->header_idx, IROP_NONE);

    /* NOP the old back-edge if it's not the header */
    if (back_edge_idx != loop->header_idx)
      ir->compact_instructions[back_edge_idx].op = TCCIR_OP_NOP;

    changes++;
#undef MAX_HOIST
  }

  tcc_ir_free_loops(loops);
  return changes;
}

int tcc_ir_opt_infinite_loop_simplify_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_infinite_loop_simplify(ctx->ir);
}

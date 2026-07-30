/*
 *  TCC IR - Negation-chain CSE
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "tcc.h"
#include "tccir.h"
#include "tccir_operand.h"
#include "ir.h"
#include "opt.h"
#include "opt_utils.h"
#include "opt_engine.h"
#include "log.h"

#ifndef LOG_NEG_CHAIN
#ifdef TCC_LOG_NEG_CHAIN
#define LOG_NEG_CHAIN(...) fprintf(stderr, "[NEG_CHAIN] " __VA_ARGS__), fprintf(stderr, "\n")
#else
#define LOG_NEG_CHAIN(...) ((void)0)
#endif
#endif

typedef struct
{
  int32_t base_vr;   /* base vreg this TEMP traces to (must be TEMP) */
  uint8_t sign;      /* 0 = +base, 1 = -base */
  uint8_t valid;
} NegCanon;

int tcc_ir_opt_neg_chain_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;

  int max_tmp = -1;
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
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_tmp)
          max_tmp = pos;
      }
    }
  }
  if (max_tmp < 0)
    return 0;

  size_t canon_size = (size_t)(max_tmp + 1) * sizeof(NegCanon);
  size_t first_size = (size_t)(max_tmp + 1) * sizeof(int32_t);
  NegCanon *canon = (NegCanon *)tcc_mallocz(canon_size);
  /* first_pos/first_neg[base_pos]: earliest TEMP vreg with canonical (base, +/-). */
  int32_t *first_pos = (int32_t *)tcc_malloc(first_size);
  int32_t *first_neg = (int32_t *)tcc_malloc(first_size);
  for (int i = 0; i <= max_tmp; i++)
  {
    first_pos[i] = -1;
    first_neg[i] = -1;
  }

  /* Merges clear only touched entries; whole-table clears were quadratic. */
  /* At most one canon and one first entry is recorded per instruction, so n bounds both lists. */
  int *touched_canon = (int *)tcc_malloc((size_t)n * sizeof(int));
  int *touched_first = (int *)tcc_malloc((size_t)n * sizeof(int));
  int n_touched_canon = 0;
  int n_touched_first = 0;

  uint8_t *is_merge = ir_opt_build_merge_bitmap(ir, n);

  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    /* Tracked TEMPs need not be live on every incoming edge: reset at merges. */
    if (is_merge[i / 8] & (1 << (i % 8)))
    {
      for (int k = 0; k < n_touched_canon; k++)
        canon[touched_canon[k]].valid = 0;
      n_touched_canon = 0;
      for (int k = 0; k < n_touched_first; k++)
      {
        int bp = touched_first[k];
        first_pos[bp] = -1;
        first_neg[bp] = -1;
      }
      n_touched_first = 0;
    }

    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (!irop_config[q->op].has_dest)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (dest.is_lval)
      continue;

    int dest_pos = TCCIR_DECODE_VREG_POSITION(dest_vr);

    int32_t base_vr = dest_vr;
    int sign = 0;
    int did_replace = 0;

    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t src_vr = irop_get_vreg(src1);
      /* Width must match: a narrowing/widening ASSIGN is not value-preserving. */
      if (!src1.is_lval && src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_TEMP &&
          irop_get_btype(dest) == irop_get_btype(src1))
      {
        int src_pos = TCCIR_DECODE_VREG_POSITION(src_vr);
        if (src_pos <= max_tmp && canon[src_pos].valid)
        {
          base_vr = canon[src_pos].base_vr;
          sign = canon[src_pos].sign;
        }
        else
        {
          base_vr = src_vr;
          sign = 0;
        }
      }
    }
    else if (q->op == TCCIR_OP_SUB)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int dest_btype = irop_get_btype(dest);
      int src_btype = irop_get_btype(src2);
      /* T_b = #0 SUB T_a; widths must match or a later fold drops the truncation. */
      if (irop_is_immediate(src1) && irop_get_imm64_ex(ir, src1) == 0 &&
          dest_btype == src_btype)
      {
        int32_t src_vr = irop_get_vreg(src2);
        if (!src2.is_lval && src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int src_pos = TCCIR_DECODE_VREG_POSITION(src_vr);
          if (src_pos <= max_tmp && canon[src_pos].valid)
          {
            base_vr = canon[src_pos].base_vr;
            sign = canon[src_pos].sign ? 0 : 1;
          }
          else
          {
            base_vr = src_vr;
            sign = 1;
          }

          int base_pos = TCCIR_DECODE_VREG_POSITION(base_vr);
          int32_t existing = (sign == 1) ? first_neg[base_pos] : first_pos[base_pos];
          if (existing >= 0 && existing != dest_vr)
          {
            IROperand new_src = irop_make_vreg(existing, dest_btype);
            q->op = TCCIR_OP_ASSIGN;
            tcc_ir_set_src1(ir, i, new_src);
            tcc_ir_set_src2(ir, i, IROP_NONE);
            LOG_NEG_CHAIN("@%d: T%d = -T%d folded to T%d = T%d (base=T%d sign=%d)",
                          i, dest_pos, TCCIR_DECODE_VREG_POSITION(src_vr),
                          dest_pos, TCCIR_DECODE_VREG_POSITION(existing),
                          base_pos, sign);
            changes++;
            did_replace = 1;
          }
        }
      }
    }

    canon[dest_pos].base_vr = base_vr;
    canon[dest_pos].sign = (uint8_t)sign;
    canon[dest_pos].valid = 1;
    touched_canon[n_touched_canon++] = dest_pos;

    if (TCCIR_DECODE_VREG_TYPE(base_vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int base_pos = TCCIR_DECODE_VREG_POSITION(base_vr);
      if (base_pos >= 0 && base_pos <= max_tmp)
      {
        if (sign == 0)
        {
          if (first_pos[base_pos] < 0)
          {
            first_pos[base_pos] = dest_vr;
            touched_first[n_touched_first++] = base_pos;
          }
        }
        else
        {
          if (first_neg[base_pos] < 0)
          {
            first_neg[base_pos] = dest_vr;
            touched_first[n_touched_first++] = base_pos;
          }
        }
      }
    }
    (void)did_replace;
  }

  tcc_free(canon);
  tcc_free(first_pos);
  tcc_free(first_neg);
  tcc_free(touched_canon);
  tcc_free(touched_first);
  tcc_free(is_merge);

  return changes;
}

int tcc_ir_opt_neg_chain_cse_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_neg_chain_cse(ctx->ir);
}

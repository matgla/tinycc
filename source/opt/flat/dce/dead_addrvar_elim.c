/*
 *  TCC IR - Dead address-taken VAR elimination
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

static void var_bit_set(uint8_t *bm, int pos) { bm[pos / 8] |= (1 << (pos % 8)); }
static int var_bit_test(const uint8_t *bm, int pos) { return bm[pos / 8] & (1 << (pos % 8)); }

int tcc_ir_opt_dead_addrvar_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Find max VAR and TMP positions */
  int max_var = 0, max_tmp = 0;
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
      if (vr >= 0)
      {
        int type = TCCIR_DECODE_VREG_TYPE(vr);
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (type == TCCIR_VREG_TYPE_VAR && pos > max_var)
          max_var = pos;
        else if (type == TCCIR_VREG_TYPE_TEMP && pos > max_tmp)
          max_tmp = pos;
      }
    }
  }

  if (max_var == 0)
    return 0;

  uint8_t *var_read = tcc_mallocz((max_var + 8) / 8);
  uint8_t *var_has_lea = tcc_mallocz((max_var + 8) / 8);
  int *lea_map = tcc_mallocz(sizeof(int) * (max_tmp + 1));
  int *var_lea = tcc_mallocz(sizeof(int) * (max_var + 1));
  for (int i = 0; i <= max_tmp; i++)
    lea_map[i] = -1;
  for (int i = 0; i <= max_var; i++)
    var_lea[i] = -1;

  /* Pass 1: build LEA map and mark directly-read VARs (LEA src1 / STORE dest are not reads). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_LEA)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      int32_t s_vr = irop_get_vreg(src1);
      if (s_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int var_pos = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          /* LEA with TMP dest: trackable — record in lea_map */
          int tmp_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
          if (tmp_pos <= max_tmp && var_pos <= max_var)
          {
            lea_map[tmp_pos] = var_pos;
            var_bit_set(var_has_lea, var_pos);
          }
        }
        else if (var_pos <= max_var)
        {
          /* LEA with VAR dest: pointer escapes into a VAR, mark as read */
          var_bit_set(var_read, var_pos);
        }
      }
      continue;
    }

    /* LEA propagation: STORE V = T where T is a LEA result */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest_op = tcc_ir_op_get_dest(ir, q);
      IROperand src1_op = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest_op);
      int32_t s_vr = irop_get_vreg(src1_op);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR && s_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int d_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
        int s_tmp = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_pos <= max_var && s_tmp <= max_tmp && lea_map[s_tmp] >= 0)
          var_lea[d_pos] = lea_map[s_tmp];
      }
    }

    /* LEA propagation: ASSIGN T = V where V holds a LEA result */
    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD)
    {
      IROperand dest_op = tcc_ir_op_get_dest(ir, q);
      IROperand src1_op = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest_op);
      int32_t s_vr = irop_get_vreg(src1_op);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP && s_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int d_tmp = TCCIR_DECODE_VREG_POSITION(d_vr);
        int s_pos = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_tmp <= max_tmp && s_pos <= max_var && var_lea[s_pos] >= 0)
          lea_map[d_tmp] = var_lea[s_pos];
      }
    }

    /* Mark VARs read as src1 (all instructions including STORE) */
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_bit_set(var_read, pos);
      }
    }

    /* Mark VARs read as src2 */
    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_bit_set(var_read, pos);
      }
    }
  }

  /* Pass 2: mark VARs whose LEA pointer escapes (used outside a STORE ptr-copy dest). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
        {
          int is_ptr_copy = 0;
          if (q->op == TCCIR_OP_STORE)
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            int32_t d_vr = irop_get_vreg(d);
            if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR)
              is_ptr_copy = 1;
          }
          if (!is_ptr_copy)
          {
            int var_pos = lea_map[tmp_pos];
            if (var_pos <= max_var)
              var_bit_set(var_read, var_pos);
          }
        }
      }
    }

    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
        {
          int var_pos = lea_map[tmp_pos];
          if (var_pos <= max_var)
            var_bit_set(var_read, var_pos);
        }
      }
    }
  }

  /* Pass 2b: mark VARs read when their pointer escapes via FUNCPARAMVAL. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCPARAMVAL)
      continue;
    IROperand param_val = tcc_ir_op_get_src1(ir, q);
    int32_t vr = irop_get_vreg(param_val);
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos <= max_var && var_lea[pos] >= 0)
      {
        int target = var_lea[pos];
        if (target <= max_var)
          var_bit_set(var_read, target);
      }
    }
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
      {
        int target = lea_map[tmp_pos];
        if (target <= max_var)
          var_bit_set(var_read, target);
      }
    }
  }

  /* Pass 3: eliminate dead writes to unread VARs */
  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* ASSIGN to dead VAR -> NOP (only if VAR has LEA, proving all accesses are tracked) */
    if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var && var_bit_test(var_has_lea, pos) && !var_bit_test(var_read, pos))
        {
          q->op = TCCIR_OP_NOP;
          changes++;
        }
      }
    }

    /* LEA from dead VAR -> NOP (TMP dests only; VAR dests handled by regular DSE) */
    if (q->op == TCCIR_OP_LEA)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t vr = irop_get_vreg(src1);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos <= max_var && var_bit_test(var_has_lea, pos) && !var_bit_test(var_read, pos))
          {
            q->op = TCCIR_OP_NOP;
            changes++;
          }
        }
      }
    }

    /* STORE through LEA to dead VAR -> NOP */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
        {
          int var_pos = lea_map[tmp_pos];
          if (var_pos <= max_var && !var_bit_test(var_read, var_pos))
          {
            q->op = TCCIR_OP_NOP;
            changes++;
          }
        }
      }
    }
  }

  /* STORE to dead VAR (non-deref): V = val where V is unread */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos <= max_var && var_bit_test(var_has_lea, pos) && !var_bit_test(var_read, pos))
      {
        q->op = TCCIR_OP_NOP;
        changes++;
      }
    }
  }

  LOG_IR_GEN("=== DEAD ADDRVAR ELIM: eliminated %d dead writes ===", changes);

  tcc_free(var_lea);
  tcc_free(lea_map);
  tcc_free(var_has_lea);
  tcc_free(var_read);
  return changes;
}

int tcc_ir_opt_dead_addrvar_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_dead_addrvar_elim(ctx->ir); }

/*
 *  TCC IR - Def-use chains (pre-SSA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_du.h"

int ir_opt_du_idx(const IROptDU *du, int32_t vreg)
{
  if (vreg < 0)
    return -1;
  int type = TCCIR_DECODE_VREG_TYPE(vreg);
  int pos = TCCIR_DECODE_VREG_POSITION(vreg);
  int idx;
  switch (type)
  {
  case TCCIR_VREG_TYPE_VAR:
    if (du->mode == IR_DU_MODE_TMP_ONLY)
      return -1;
    idx = pos;
    break;
  case TCCIR_VREG_TYPE_TEMP:
    idx = du->max_var + pos;
    break;
  case TCCIR_VREG_TYPE_PARAM:
    if (du->mode == IR_DU_MODE_TMP_ONLY)
      return -1;
    idx = du->max_var + du->max_tmp + pos;
    break;
  default:
    return -1;
  }
  return (idx < du->total) ? idx : -1;
}

void ir_opt_du_build(TCCIRState *ir, IROptDU *du)
{
  ir_opt_du_build_mode(ir, du, IR_DU_MODE_FULL);
}

void ir_opt_du_build_mode(TCCIRState *ir, IROptDU *du, uint8_t mode)
{
  du->mode = mode;
  if (mode == IR_DU_MODE_TMP_ONLY) {
    du->max_var = 0;
    du->max_tmp = ir->next_temporary_variable + 1;
    du->total = du->max_tmp;
  } else {
    du->max_var = ir->next_local_variable + 1;
    du->max_tmp = ir->next_temporary_variable + 1;
    int max_par = ir->next_parameter + 1;
    du->total = du->max_var + du->max_tmp + max_par;
  }

  /* Single allocation holding def[], use[] and def_cnt[]; only du->def is freed. */
  int def_bytes = du->total * (int)sizeof(int);
  int use_bytes = du->total * (int)sizeof(uint8_t);
  int cnt_bytes = du->total * (int)sizeof(uint8_t);
  du->def = tcc_malloc(def_bytes + use_bytes + cnt_bytes);
  du->use = (uint8_t *)((char *)du->def + def_bytes);
  du->def_cnt = (uint8_t *)((char *)du->def + def_bytes + use_bytes);

  for (int k = 0; k < du->total; k++)
    du->def[k] = -1;
  memset(du->use, 0, use_bytes);
  memset(du->def_cnt, 0, cnt_bytes);

  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* STORE-family ops keep the address pointer in `dest`: that is a use, not a def. */
    int dest_is_addr_use = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                            q->op == TCCIR_OP_STORE_POSTINC);
    if (irop_config[q->op].has_dest)
    {
      int idx = ir_opt_du_idx(du, irop_get_vreg(tcc_ir_op_get_dest(ir, q)));
      if (idx >= 0)
      {
        if (dest_is_addr_use)
        {
          if (du->use[idx] < 2)
            du->use[idx]++;
        }
        else
        {
          du->def[idx] = i;
          if (du->def_cnt[idx] < 2)
            du->def_cnt[idx]++;
        }
      }
    }
    if (irop_config[q->op].has_src1)
    {
      int idx = ir_opt_du_idx(du, irop_get_vreg(tcc_ir_op_get_src1(ir, q)));
      if (idx >= 0 && du->use[idx] < 2)
        du->use[idx]++;
    }
    if (irop_config[q->op].has_src2)
    {
      int idx = ir_opt_du_idx(du, irop_get_vreg(tcc_ir_op_get_src2(ir, q)));
      if (idx >= 0 && du->use[idx] < 2)
        du->use[idx]++;
    }
    /* MLA's 4th accumulator operand is a use; missing it makes its result look dead (seed 4274). */
    if (q->op == TCCIR_OP_MLA)
    {
      int idx = ir_opt_du_idx(du, irop_get_vreg(tcc_ir_op_get_accum(ir, q)));
      if (idx >= 0 && du->use[idx] < 2)
        du->use[idx]++;
    }
  }
}

uint8_t *ir_opt_build_def_count(TCCIRState *ir, int n, int *out_stride)
{
  int max_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (vr < 0)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos > max_pos)
      max_pos = pos;
  }
  int stride = max_pos + 1;
  uint8_t *dc = tcc_mallocz(16 * stride);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (vr < 0)
      continue;
    int typ = TCCIR_DECODE_VREG_TYPE(vr);
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (dc[typ * stride + pos] < 2)
      dc[typ * stride + pos]++;
  }
  *out_stride = stride;
  return dc;
}

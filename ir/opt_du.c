/*
 *  TCC IR - Def-Use Table (shared pre-SSA optimization helper)
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
    idx = pos;
    break;
  case TCCIR_VREG_TYPE_TEMP:
    idx = du->max_var + pos;
    break;
  case TCCIR_VREG_TYPE_PARAM:
    idx = du->max_var + du->max_tmp + pos;
    break;
  default:
    return -1;
  }
  return (idx < du->total) ? idx : -1;
}

void ir_opt_du_build(TCCIRState *ir, IROptDU *du)
{
  du->max_var = ir->next_local_variable + 1;
  du->max_tmp = ir->next_temporary_variable + 1;
  int max_par = ir->next_parameter + 1;
  du->total = du->max_var + du->max_tmp + max_par;

  /* Single allocation: int def[] immediately followed by uint8_t use[]. */
  int def_bytes = du->total * (int)sizeof(int);
  int use_bytes = du->total * (int)sizeof(uint8_t);
  du->def = tcc_malloc(def_bytes + use_bytes);
  du->use = (uint8_t *)((char *)du->def + def_bytes);

  for (int k = 0; k < du->total; k++)
    du->def[k] = -1;
  memset(du->use, 0, use_bytes);

  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* STORE-family ops carry the address pointer in their `dest` slot --
     * that is a USE of the pointer vreg, not a definition.  Counting it as
     * a def would shadow the real def from the upstream address-compute
     * (e.g. ADD base, #imm) and prevent disp/indexed fusion from finding
     * it via ir_opt_du_def. */
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
  }
}
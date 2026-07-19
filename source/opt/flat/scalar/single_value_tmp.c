/*
 *  TCC IR - Single-value temp propagation (flat pass)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Relocated flat pass, Branch A [A*]; see docs/plan_legacy_flat_ir_ssa_retire.md */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

/* If all defs of a TEMP are ASSIGN/LOAD of the same immediate, fold uses to it. */
int tcc_ir_opt_single_value_tmp(TCCIRState *ir)
{
#define SVT_MAX_TEMPS 128
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;

  int max_tmp = -1;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(d);
    if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP) {
      int pos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (pos > max_tmp)
        max_tmp = pos;
    }
  }
  if (max_tmp < 0 || max_tmp >= SVT_MAX_TEMPS)
    return 0;

  int count = max_tmp + 1;
  uint8_t state[SVT_MAX_TEMPS];
  int32_t vals[SVT_MAX_TEMPS];
  memset(state, 0, count);

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(d);
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (pos >= count) continue;

    if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) &&
        state[pos] != 2) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (irop_is_immediate(s) && !s.is_lval &&
          irop_get_btype(s) == IROP_BTYPE_INT32) {
        int32_t v = (int32_t)irop_get_imm64_ex(ir, s);
        if (state[pos] == 0) {
          state[pos] = 1;
          vals[pos] = v;
        } else if (vals[pos] != v) {
          state[pos] = 2;
        }
        continue;
      }
    }
    state[pos] = 2;
  }

  int changes = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_RETURNVALUE)
      continue;
    for (int k = 0; k < 2; k++) {
      IROperand op = k == 0 ? tcc_ir_op_get_src1(ir, q)
                            : tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr < 0 || op.is_lval)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos >= count || state[pos] != 1)
        continue;
      IROperand imm = irop_make_imm32(-1, vals[pos], IROP_BTYPE_INT32);
      if (k == 0)
        tcc_ir_set_src1(ir, i, imm);
      else
        tcc_ir_set_src2(ir, i, imm);
      changes++;
    }
  }

  if (changes) {
    /* Let DCE reclaim dead defs; a single-value temp may still have other uses. */
    changes += tcc_ir_opt_dce(ir);
  }

  if (changes) {
    n = ir->next_instruction_index;
    int64_t ret_val = 0;
    int ret_btype = 0;
    int ret_idx = -1;
    int all_same_ret = 1;
    int has_side_effect = 0;
    for (int i = 0; i < n && all_same_ret && !has_side_effect; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      switch (q->op) {
      case TCCIR_OP_RETURNVALUE: {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (!irop_is_immediate(s)) { all_same_ret = 0; break; }
        int64_t v = irop_get_imm64_ex(ir, s);
        if (ret_idx < 0) {
          ret_val = v; ret_btype = irop_get_btype(s); ret_idx = i;
        } else if (v != ret_val) { all_same_ret = 0; }
        break;
      }
      case TCCIR_OP_STORE: case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_FUNCCALLVAL: case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_VLA_ALLOC: case TCCIR_OP_VLA_SP_SAVE:
      case TCCIR_OP_VLA_SP_RESTORE: case TCCIR_OP_TRAP:
      case TCCIR_OP_RETURNVOID:
        has_side_effect = 1; break;
      default: break;
      }
    }
    if (all_same_ret && !has_side_effect && ret_idx >= 0) {
      for (int i = 0; i < n; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_RETURNVALUE)
          continue;
        q->op = TCCIR_OP_NOP;
        changes++;
      }
      if (ret_idx > 0) {
        ir->compact_instructions[0].op = TCCIR_OP_RETURNVALUE;
        IROperand rv = irop_make_imm32(-1, (int32_t)ret_val, ret_btype);
        tcc_ir_set_src1(ir, 0, rv);
        ir->compact_instructions[ret_idx].op = TCCIR_OP_NOP;
        changes++;
      }
      changes += tcc_ir_opt_dce(ir);
    }
  }
  return changes;
#undef SVT_MAX_TEMPS
}

int tcc_ir_opt_single_value_tmp_ex(IROptCtx *ctx) { return tcc_ir_opt_single_value_tmp(ctx->ir); }


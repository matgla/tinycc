/*
 *  TCC IR - SSA stack-address resolution
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include <limits.h>

int ssa_opt_resolve_lea_stackloc(IRSSAOptCtx *ctx, int32_t vr)
{
  return ssa_opt_resolve_lea_stackloc_ex(ctx, vr, NULL);
}

/* out_base_var: -1 for a direct stack slot, else the VAR/PARAM vreg of `&VAR`. */
int ssa_opt_resolve_lea_stackloc_ex(IRSSAOptCtx *ctx, int32_t vr, int32_t *out_base_var)
{
  TCCIRState *ir = ctx->ir;
  int acc = 0;
  if (out_base_var)
    *out_base_var = -1;
  /* Hop cap: pathological chains must bail to INT_MIN, not run unbounded. */
  for (int hop = 0; hop < 64; hop++) {
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return INT_MIN;
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (!vi || vi->def_instr < 0 || vi->def_count > 1)
      return INT_MIN;
    IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];

    if (dq->op == TCCIR_OP_LEA) {
      IROperand src = tcc_ir_op_get_src1(ir, dq);
      if (src.tag == IROP_TAG_STACKOFF || src.is_local) {
        if (out_base_var)
          *out_base_var = irop_get_vreg(src);
        return irop_get_stack_offset(src) + acc;
      }
      return INT_MIN;
    }

    if (dq->op == TCCIR_OP_ASSIGN) {
      IROperand src = tcc_ir_op_get_src1(ir, dq);
      if (src.tag == IROP_TAG_STACKOFF && !src.is_lval) {
        if (out_base_var)
          *out_base_var = irop_get_vreg(src);
        return irop_get_stack_offset(src) + acc;
      }
      int32_t sv = irop_get_vreg(src);
      if (sv >= 0 && !src.is_lval) {
        vr = sv;
        continue;
      }
      return INT_MIN;
    }

    /* STORE with a non-lval dest materialises an address, same as LEA. */
    if (dq->op == TCCIR_OP_STORE) {
      IROperand dest = tcc_ir_op_get_dest(ir, dq);
      if (!dest.is_lval) {
        IROperand src = tcc_ir_op_get_src1(ir, dq);
        if (src.tag == IROP_TAG_STACKOFF && !src.is_lval) {
          if (out_base_var)
            *out_base_var = irop_get_vreg(src);
          return irop_get_stack_offset(src) + acc;
        }
        int32_t sv = irop_get_vreg(src);
        if (sv >= 0 && !src.is_lval) {
          vr = sv;
          continue;
        }
      }
      return INT_MIN;
    }

    if (dq->op == TCCIR_OP_ADD || dq->op == TCCIR_OP_SUB) {
      IROperand src1 = tcc_ir_op_get_src1(ir, dq);
      IROperand src2 = tcc_ir_op_get_src2(ir, dq);
      if (!src1.is_lval && irop_is_immediate(src2)) {
        int32_t s1vr = irop_get_vreg(src1);
        if (s1vr >= 0) {
          int delta = irop_get_imm32(src2);
          acc += (dq->op == TCCIR_OP_ADD) ? delta : -delta;
          vr = s1vr;
          continue;
        }
      }
      return INT_MIN;
    }

    return INT_MIN;
  }
  return INT_MIN;
}

/* Resolve `vr` backward to canonical (base_vr, offset); contract in ssa_opt.h. */
int ssa_opt_resolve_temp_to_base_off(IRSSAOptCtx *ctx, int32_t vr,
                                      int32_t *out_base, int32_t *out_off)
{
  *out_off = 0;
  for (int hop = 0; hop < 8; hop++) {
    if (vr < 0)
      return 0;
    int type = TCCIR_DECODE_VREG_TYPE(vr);
    if (type == TCCIR_VREG_TYPE_VAR || type == TCCIR_VREG_TYPE_PARAM) {
      *out_base = vr;
      return 1;
    }
    if (type != TCCIR_VREG_TYPE_TEMP)
      return 0;

    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (!vi || vi->def_count > 1 || vi->def_instr < 0)
      return 0;
    IRQuadCompact *dq = &ctx->ir->compact_instructions[vi->def_instr];

    if (dq->op == TCCIR_OP_ASSIGN) {
      IROperand src = tcc_ir_op_get_src1(ctx->ir, dq);
      int32_t sv = irop_get_vreg(src);
      if (sv < 0)
        return 0;
      int svt = TCCIR_DECODE_VREG_TYPE(sv);
      if (svt == TCCIR_VREG_TYPE_TEMP && !src.is_lval && !src.is_local &&
          !src.is_llocal && src.tag == IROP_TAG_VREG) {
        vr = sv;
        continue;
      }
      /* A VAR/PARAM read appears in either encoding: register form or slot form. */
      if (svt == TCCIR_VREG_TYPE_VAR || svt == TCCIR_VREG_TYPE_PARAM) {
        int reg_form = (src.tag == IROP_TAG_VREG && !src.is_lval &&
                        !src.is_local && !src.is_llocal);
        int slot_form = (src.tag == IROP_TAG_STACKOFF && src.is_lval &&
                          src.is_local && !src.is_llocal);
        if (reg_form || slot_form) {
          *out_base = sv;
          return 1;
        }
      }
      return 0;
    }

    if (dq->op == TCCIR_OP_ADD) {
      IROperand src1 = tcc_ir_op_get_src1(ctx->ir, dq);
      IROperand src2 = tcc_ir_op_get_src2(ctx->ir, dq);
      if (!irop_is_immediate(src2) || src2.is_lval)
        return 0;
      if (src1.is_lval)
        return 0;
      int32_t s1vr = irop_get_vreg(src1);
      if (s1vr < 0)
        return 0;
      *out_off += irop_get_imm32(src2);
      vr = s1vr;
      continue;
    }

    /* Any other defining op: the TEMP itself is the canonical root. */
    *out_base = vr;
    return 1;
  }
  return 0;
}

int ssa_opt_indirect_stack_offset(IRSSAOptCtx *ctx, const IRQuadCompact *q, int side)
{
  return ssa_opt_indirect_stack_offset_ex(ctx, q, side, NULL);
}

int ssa_opt_indirect_stack_offset_ex(IRSSAOptCtx *ctx, const IRQuadCompact *q, int side,
                                     int32_t *out_base_var)
{
  TCCIRState *ir = ctx->ir;
  IROperand base;
  int has_index = 0;
  int require_lval = 0;
  IROperand idx = IROP_NONE, scale = IROP_NONE;

  if (out_base_var)
    *out_base_var = -1;

  if (side == SSA_OPT_INDIRECT_DEST) {
    base = tcc_ir_op_get_dest(ir, q);
    if (q->op == TCCIR_OP_STORE_INDEXED) {
      has_index = 1;
      idx = tcc_ir_op_get_src2(ir, q);
      scale = tcc_ir_op_get_scale(ir, q);
    } else if (q->op == TCCIR_OP_STORE) {
      require_lval = 1; /* plain *T = val: T must be deref'd */
    } else {
      return INT_MIN;
    }
  } else {
    base = tcc_ir_op_get_src1(ir, q);
    if (q->op == TCCIR_OP_LOAD_INDEXED) {
      has_index = 1;
      idx = tcc_ir_op_get_src2(ir, q);
      scale = tcc_ir_op_get_scale(ir, q);
    } else if (q->op == TCCIR_OP_LOAD) {
      require_lval = 1;
    } else {
      return INT_MIN;
    }
  }

  if (base.tag != IROP_TAG_VREG || base.is_local)
    return INT_MIN;
  if (require_lval && !base.is_lval)
    return INT_MIN;
  int32_t bvr = irop_get_vreg(base);
  if (bvr < 0 || TCCIR_DECODE_VREG_TYPE(bvr) != TCCIR_VREG_TYPE_TEMP)
    return INT_MIN;
  int base_off = ssa_opt_resolve_lea_stackloc_ex(ctx, bvr, out_base_var);
  if (base_off == INT_MIN) {
    if (out_base_var)
      *out_base_var = -1;
    return INT_MIN;
  }
  if (!has_index)
    return base_off;
  if (!irop_is_immediate(idx) || !irop_is_immediate(scale))
    return INT_MIN;
  if (irop_get_imm32(scale) != 0)
    return INT_MIN;
  return base_off + irop_get_imm32(idx);
}

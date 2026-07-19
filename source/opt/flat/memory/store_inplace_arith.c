/*
 *  TCC IR - Transform Primitives (shared pre-SSA optimization helpers)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_xform.h"
#include "opt_engine.h"



/* In-place arithmetic fold:
 *   T <- V OP src ; V <- T [STORE]  =>  V <- V OP src ; NOP
 * where T is a single-use TEMP and OP a simple arith op. Eliminates one
 * mov at codegen.
 * Safety: V must be register-promotable (not addrtaken, not lvalue).
 * The OP reads V as a source operand before writing V as dest, so the
 * old-value-of-V semantic for the arithmetic is preserved on ARM. */
int tcc_ir_opt_store_inplace_arith(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i + 1 < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* Restrict to simple, no-side-effect arith ops with a single dest. */
    switch (q->op) {
      case TCCIR_OP_ADD:
      case TCCIR_OP_SUB:
      case TCCIR_OP_AND:
      case TCCIR_OP_OR:
      case TCCIR_OP_XOR:
      case TCCIR_OP_SHL:
      case TCCIR_OP_SAR:
      case TCCIR_OP_SHR:
        break;
      default:
        continue;
    }

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t t_vr = irop_get_vreg(dest);
    if (t_vr < 0 || !tcc_ir_vreg_is_valid(ir, t_vr)) continue;
    if (TCCIR_DECODE_VREG_TYPE(t_vr) != TCCIR_VREG_TYPE_TEMP) continue;
    if (dest.is_lval) continue;

    /* Find next non-NOP; require it to be the STORE consuming T. */
    int j = i + 1;
    while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP) j++;
    if (j >= n) continue;
    /* Must stay within the same basic block. */
    if (!ir_xform_same_block(ir, i, j)) continue;

    IRQuadCompact *sq = &ir->compact_instructions[j];
    if (sq->op != TCCIR_OP_STORE) continue;

    IROperand store_dest = tcc_ir_op_get_dest(ir, sq);
    IROperand store_src = tcc_ir_op_get_src1(ir, sq);

    /* STORE must read T and write a register-promoted vreg V. */
    if (store_src.is_lval) continue;
    if (irop_get_vreg(store_src) != t_vr) continue;

    int32_t v_vr = irop_get_vreg(store_dest);
    if (v_vr < 0 || !tcc_ir_vreg_is_valid(ir, v_vr)) continue;
    if (store_dest.is_lval) continue;

    /* V must be a register-promoted scalar (PARAM or VAR), not addrtaken,
     * not lvalue, not 64-bit (no easy single-reg in-place form). */
    int v_type = TCCIR_DECODE_VREG_TYPE(v_vr);
    if (v_type != TCCIR_VREG_TYPE_PARAM && v_type != TCCIR_VREG_TYPE_VAR)
      continue;
    IRLiveInterval *vli = tcc_ir_vreg_live_interval(ir, v_vr);
    if (!vli || vli->addrtaken || vli->is_lvalue) continue;
    if (vli->is_llong || vli->is_double || vli->is_complex) continue;

    /* T must have exactly one use (the STORE): scan for any other read of
     * T's vreg. Deref-via-T (is_lval=1) still reads T as an address, and
     * STORE-class dest reads T as a base — both count. */
    int extra_uses = 0;
    for (int k = 0; k < n && !extra_uses; k++) {
      if (k == j) continue;
      IRQuadCompact *kq = &ir->compact_instructions[k];
      if (kq->op == TCCIR_OP_NOP) continue;
      int is_store_op = (kq->op == TCCIR_OP_STORE || kq->op == TCCIR_OP_STORE_INDEXED ||
                         kq->op == TCCIR_OP_STORE_POSTINC);
      if (irop_config[kq->op].has_dest) {
        IROperand kd = tcc_ir_op_get_dest(ir, kq);
        /* STORE-class dest is an address/base read, so it counts as a use. */
        if (irop_has_vreg(kd) && irop_get_vreg(kd) == t_vr &&
            (is_store_op || kd.is_lval)) { extra_uses = 1; break; }
      }
      if (irop_config[kq->op].has_src1) {
        IROperand s1 = tcc_ir_op_get_src1(ir, kq);
        if (irop_has_vreg(s1) && irop_get_vreg(s1) == t_vr) { extra_uses = 1; break; }
      }
      if (irop_config[kq->op].has_src2) {
        IROperand s2 = tcc_ir_op_get_src2(ir, kq);
        if (irop_has_vreg(s2) && irop_get_vreg(s2) == t_vr) { extra_uses = 1; break; }
      }
    }
    if (extra_uses) continue;

    /* Btype match: V and T must have the same width.  Restrict to INT32
     * (sub-word carries narrowing; INT64 needs a register pair). */
    int t_btype = irop_get_btype(dest);
    int v_btype = irop_get_btype(store_dest);
    if (t_btype != v_btype) continue;
    if (t_btype != IROP_BTYPE_INT32) continue;

    IROperand new_dest = store_dest;
    new_dest.is_lval = 0;
    tcc_ir_set_dest(ir, i, new_dest);
    ir_xform_nop(ir, j);
    changes++;
  }

  return changes;
}

int tcc_ir_opt_store_inplace_arith_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_store_inplace_arith(ctx->ir);
}
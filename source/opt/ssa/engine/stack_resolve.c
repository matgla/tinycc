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

/* ssa:stack_deref_fold -- a load or store through a TEMP pointer whose value is
 * a constant frame address becomes a direct StackLoc access.
 *
 * The Zig C backend names every sub-object through a pointer temporary
 * (`t3 = &t2->major; (*t3) = 5;`), assigned in each arm of a switch, so before
 * SSA the pointer is a multi-definition VAR and stack_addr_simplify cannot
 * follow it; it also stays away from any function with a loop.  After renaming
 * each arm's pointer is a single-definition TEMP, and resolving it here turns
 * `add rX, sp, #off; str rY, [rX]` into `str rY, [sp, #off]`; DCE then drops
 * the address arithmetic.  Every hop must be the value's only definition, phi
 * included: the rewrite replaces the use outright. */
static int sdf_resolve(IRSSAOptCtx *ctx, int32_t vr, int *out_off, int32_t *out_slot_vr)
{
  TCCIRState *ir = ctx->ir;
  int acc = 0;
  for (int hop = 0; hop < 16; hop++) {
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (!vi || vi->def_instr < 0 || ssa_opt_def_total(vi) != 1)
      return 0;
    IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
    IROperand src = tcc_ir_op_get_src1(ir, dq);
    int direct = irop_get_tag(src) == IROP_TAG_STACKOFF && irop_get_vreg(src) < 0 && src.is_local &&
                 !src.is_lval && !src.is_llocal && !src.is_param;
    int copy = irop_get_tag(src) == IROP_TAG_VREG && !src.is_lval && !src.is_local && !src.is_llocal;
    if (dq->op == TCCIR_OP_ASSIGN || dq->op == TCCIR_OP_LEA) {
      if (direct) {
        *out_off = irop_get_stack_offset(src) + acc;
        *out_slot_vr = irop_get_vreg(src);
        return 1;
      }
      if (dq->op == TCCIR_OP_ASSIGN && copy) {
        vr = irop_get_vreg(src);
        continue;
      }
      return 0;
    }
    if (dq->op == TCCIR_OP_ADD || dq->op == TCCIR_OP_SUB) {
      IROperand s2 = tcc_ir_op_get_src2(ir, dq);
      if (irop_get_tag(s2) != IROP_TAG_IMM32 || s2.is_lval)
        return 0;
      int d = irop_get_imm32(s2);
      acc += dq->op == TCCIR_OP_ADD ? d : -d;
      if (direct) {
        *out_off = irop_get_stack_offset(src) + acc;
        *out_slot_vr = irop_get_vreg(src);
        return 1;
      }
      if (copy) {
        vr = irop_get_vreg(src);
        continue;
      }
      return 0;
    }
    return 0;
  }
  return 0;
}

int ssa_opt_stack_deref_fold(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int changes = 0;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int op = q->op;
    if (op != TCCIR_OP_STORE && op != TCCIR_OP_LOAD && op != TCCIR_OP_STORE_INDEXED && op != TCCIR_OP_LOAD_INDEXED)
      continue;
    int is_store = op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_INDEXED;
    int indexed = op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_LOAD_INDEXED;
    IROperand base = is_store ? tcc_ir_op_get_dest(ir, q) : tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(base) != IROP_TAG_VREG || base.is_local || base.is_llocal || base.is_lval == indexed)
      continue;
    int32_t bvr = irop_get_vreg(base), slot_vr;
    int off;
    if (!sdf_resolve(ctx, bvr, &off, &slot_vr))
      continue;

    /* The access's width and sign: the deref operand for a plain access, the
     * value (store) or result (load) for an indexed one, whose marks were
     * moved onto the base when it was fused. */
    IROperand acc = base;
    if (indexed) {
      IROperand idx = tcc_ir_op_get_src2(ir, q);
      IROperand scale = tcc_ir_op_get_scale(ir, q);
      if (irop_get_tag(idx) != IROP_TAG_IMM32 || idx.is_lval ||
          (!irop_is_none(scale) && (irop_get_tag(scale) != IROP_TAG_IMM32 || irop_get_imm32(scale) != 0)))
        continue;
      off += irop_get_imm32(idx);
      acc = is_store ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_dest(ir, q);
      acc.aux = base.aux;
      if (irop_is_64bit(acc))
        continue;
    }
    if (irop_get_btype(acc) == IROP_BTYPE_STRUCT || acc.is_complex || tcc_ir_access_is_volatile(ir, acc))
      continue;

    /* Keep the slot's own identity: a frontend temporary slot carries a
     * negative vreg (VR_TEMP_LOCAL), which passes match its accesses by. */
    IROperand slot = irop_make_stackoff(slot_vr, off, 1, 0, 0, irop_get_btype(acc));
    slot.is_unsigned = acc.is_unsigned;
    slot.aux = acc.aux;
    if (indexed)
      q->op = is_store ? TCCIR_OP_STORE : TCCIR_OP_LOAD;
    if (is_store)
      tcc_ir_set_dest(ir, i, slot);
    else
      tcc_ir_set_src1(ir, i, slot);

    /* The pointer is no longer used here unless another operand names it. */
    int still = 0;
    if (irop_config[q->op].has_dest)
      still |= irop_get_vreg(tcc_ir_op_get_dest(ir, q)) == bvr;
    if (irop_config[q->op].has_src1)
      still |= irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == bvr;
    if (irop_config[q->op].has_src2)
      still |= irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == bvr;
    if (!still)
      ssa_opt_remove_use_instr(ssa_opt_vinfo(ctx, bvr), i);
    changes++;
  }
  return changes;
}

/*
 *  TCC IR - SSA Constant Folding
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

/* ============================================================================
 * Constant Folding: evaluate ALU ops with two immediate operands at compile
 * time, replacing the instruction with ASSIGN dest = #result.
 *
 * Also handles algebraic identities:
 *   x + 0, x - 0, x * 1, x << 0, x >> 0, x | 0, x ^ 0, x & ~0 → x
 *   x * 0, x & 0 → 0
 *   x | ~0 → ~0
 *   x - x, x ^ x → 0
 * ============================================================================ */

static int has_barrel_shift_annotation(TCCIRState *ir, const IRQuadCompact *q)
{
  return ir->barrel_shifts && q->orig_index >= 0 &&
         q->orig_index <= ir->max_orig_index &&
         ir->barrel_shifts[q->orig_index] != 0;
}

/* Resolve a vreg operand back to its constant defining ASSIGN, if any.
 * In SSA a TEMP is single-def, so following its def to an ASSIGN #imm gives
 * the value the operand will carry at runtime.  Returns 1 and sets *out_val
 * when the vreg's single def is an `ASSIGN #imm32` with non-lval src.
 *
 * Restricted to defs in the SAME basic block as the use: a cross-block
 * forward through a join point can lose information when multiple paths
 * each define the value differently (the bug_switch_goto_or pattern —
 * see comment on cprop_imm in ssa_opt_cprop.c).  Same-block defs have
 * exactly one path from def to use, so the resolution is unambiguous. */
static int try_resolve_const_vreg(IRSSAOptCtx *ctx, IROperand op, int use_idx, int32_t *out_val)
{
  if (op.is_lval || op.is_local || op.is_llocal || op.is_sym)
    return 0;
  if (op.tag != IROP_TAG_VREG)
    return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_count != 1 || vi->def_instr < 0)
    return 0;
  IRCFG *cfg = ctx->cfg;
  if (!cfg)
    return 0;
  if (cfg->instr_to_block[vi->def_instr] != cfg->instr_to_block[use_idx])
    return 0;
  IRQuadCompact *dq = &ctx->ir->compact_instructions[vi->def_instr];
  if (dq->op != TCCIR_OP_ASSIGN)
    return 0;
  IROperand dsrc = tcc_ir_op_get_src1(ctx->ir, dq);
  if (dsrc.tag != IROP_TAG_IMM32 || dsrc.is_lval)
    return 0;
  *out_val = dsrc.u.imm32;
  return 1;
}

static int fold_binary(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  IROperand dest = tcc_ir_op_get_dest(ir, q);

  if (has_barrel_shift_annotation(ir, q))
    return 0;

  int32_t dest_vr = irop_get_vreg(dest);
  if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  int src1_is_imm = (src1.tag == IROP_TAG_IMM32 && !src1.is_lval);
  int src2_is_imm = (src2.tag == IROP_TAG_IMM32 && !src2.is_lval);
  int32_t val1 = src1.u.imm32;
  int32_t val2 = src2.u.imm32;

  /* Resolve vreg operands whose single ASSIGN def carries a constant —
   * fold_binary can then catch identities like `0 + x` even when the 0
   * arrives via an intermediate vreg.  Materialise the resolved value as
   * a real immediate operand and drop the vreg use; DCE then cleans up
   * the dead constant ASSIGN if it has no other users.  The fold logic
   * below then proceeds unchanged on the immediate. */
  int32_t resolved_v1 = 0, resolved_v2 = 0;
  if (!src1_is_imm && try_resolve_const_vreg(ctx, src1, idx, &resolved_v1)) {
    int32_t old_vr = irop_get_vreg(src1);
    IROperand imm1 = irop_make_imm32(0, resolved_v1, irop_get_btype(src1));
    tcc_ir_op_set_src1(ir, q, imm1);
    IRSSAVregInfo *uvi = ssa_opt_vinfo(ctx, old_vr);
    if (uvi)
      ssa_opt_remove_use_instr(uvi, idx);
    src1 = imm1;
    src1_is_imm = 1;
    val1 = resolved_v1;
  }
  if (!src2_is_imm && try_resolve_const_vreg(ctx, src2, idx, &resolved_v2)) {
    int32_t old_vr = irop_get_vreg(src2);
    IROperand imm2 = irop_make_imm32(0, resolved_v2, irop_get_btype(src2));
    tcc_ir_op_set_src2(ir, q, imm2);
    IRSSAVregInfo *uvi = ssa_opt_vinfo(ctx, old_vr);
    if (uvi)
      ssa_opt_remove_use_instr(uvi, idx);
    src2 = imm2;
    src2_is_imm = 1;
    val2 = resolved_v2;
  }

  /* Both operands immediate: full constant fold */
  if (src1_is_imm && src2_is_imm) {
    int64_t result;
    switch (q->op) {
    case TCCIR_OP_ADD: result = (int64_t)((uint64_t)(uint32_t)val1 + (uint64_t)(uint32_t)val2); break;
    case TCCIR_OP_SUB: result = (int64_t)((uint64_t)(uint32_t)val1 - (uint64_t)(uint32_t)val2); break;
    case TCCIR_OP_MUL: result = (int64_t)((uint64_t)(uint32_t)val1 * (uint64_t)(uint32_t)val2); break;
    case TCCIR_OP_AND: result = val1 & val2; break;
    case TCCIR_OP_OR:  result = val1 | val2; break;
    case TCCIR_OP_XOR: result = val1 ^ val2; break;
    case TCCIR_OP_SHL:
      if ((uint32_t)val2 >= 32) result = 0;
      else result = (int64_t)((uint32_t)val1 << (uint32_t)val2);
      break;
    case TCCIR_OP_SHR:
      if ((uint32_t)val2 >= 32) result = 0;
      else result = (uint32_t)val1 >> (uint32_t)val2;
      break;
    case TCCIR_OP_ROR:
    {
      uint32_t v = (uint32_t)val1;
      uint32_t n = (uint32_t)val2 & 31;
      result = (int64_t)(int32_t)((v >> n) | (v << (32 - n)));
      break;
    }
    case TCCIR_OP_SAR:
      if ((uint32_t)val2 >= 32) result = val1 >> 31;
      else result = val1 >> val2;
      break;
    case TCCIR_OP_DIV:
      if (val2 == 0) return 0;
      /* INT_MIN / -1 overflows and traps on hardware divide. */
      if (val2 == -1 && (int32_t)val1 == INT32_MIN) return 0;
      result = val1 / val2;
      break;
    case TCCIR_OP_UDIV:
      if (val2 == 0) return 0;
      result = (uint32_t)val1 / (uint32_t)val2;
      break;
    case TCCIR_OP_IMOD:
      if (val2 == 0) return 0;
      if (val2 == -1 && (int32_t)val1 == INT32_MIN) return 0;
      result = val1 % val2;
      break;
    case TCCIR_OP_UMOD:
      if (val2 == 0) return 0;
      result = (uint32_t)val1 % (uint32_t)val2;
      break;
    default:
      return 0;
    }

    IROperand imm = irop_make_imm32(0, (int32_t)result, dest.btype);
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_op_set_src1(ir, q, imm);
    tcc_ir_op_set_src2(ir, q, IROP_NONE);

    IRSSAVregInfo *vi;
    vi = ssa_opt_vinfo(ctx, irop_get_vreg(src1));
    if (vi) ssa_opt_remove_use_instr(vi, idx);
    vi = ssa_opt_vinfo(ctx, irop_get_vreg(src2));
    if (vi) ssa_opt_remove_use_instr(vi, idx);
    return 1;
  }

  /* Algebraic identities with one immediate operand */
  int32_t src1_vr = irop_get_vreg(src1);
  int32_t src2_vr = irop_get_vreg(src2);

  /* x op x patterns */
  if (!src1_is_imm && !src2_is_imm && src1_vr >= 0 && src1_vr == src2_vr &&
      src1.tag == IROP_TAG_VREG && src2.tag == IROP_TAG_VREG &&
      !src1.is_lval && !src2.is_lval) {
    int fold_to_zero = 0;
    int fold_to_self = 0;
    switch (q->op) {
    case TCCIR_OP_SUB: fold_to_zero = 1; break;
    case TCCIR_OP_XOR: fold_to_zero = 1; break;
    case TCCIR_OP_AND: fold_to_self = 1; break;
    case TCCIR_OP_OR:  fold_to_self = 1; break;
    default: break;
    }
    if (fold_to_zero) {
      IROperand imm = irop_make_imm32(0, 0, dest.btype);
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_op_set_src1(ir, q, imm);
      tcc_ir_op_set_src2(ir, q, IROP_NONE);
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, src1_vr);
      if (vi) {
        ssa_opt_remove_use_instr(vi, idx);
        ssa_opt_remove_use_instr(vi, idx);
      }
      return 1;
    }
    if (fold_to_self) {
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_op_set_src2(ir, q, IROP_NONE);
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, src1_vr);
      if (vi) ssa_opt_remove_use_instr(vi, idx);
      return 1;
    }
  }

  /* Bit-complement identity:
   *   a | (a ^ -1) = -1
   *   a & (a ^ -1) = 0
   *
   * Recognises the pattern where one operand is a single-def TEMP whose
   * defining op is `XOR a #-1`, and the other operand is `a` itself.
   * Common after LOAD-CSE folds duplicate reads of the same address —
   * see pr60502.c where `*x ^ m1 | *x` with m1 = all-FF collapses to -1
   * per byte.  Restricts to same-block defs to keep the fold safe under
   * control-flow joins (mirrors try_resolve_const_vreg). */
  if ((q->op == TCCIR_OP_OR || q->op == TCCIR_OP_AND) &&
      !src1_is_imm && !src2_is_imm &&
      src1.tag == IROP_TAG_VREG && src2.tag == IROP_TAG_VREG &&
      !src1.is_lval && !src2.is_lval &&
      src1_vr >= 0 && src2_vr >= 0) {
    IRCFG *cfg = ctx->cfg;
    for (int trial = 0; trial < 2 && cfg; trial++) {
      int32_t a_vr = trial ? src2_vr : src1_vr;
      int32_t x_vr = trial ? src1_vr : src2_vr;
      if (TCCIR_DECODE_VREG_TYPE(x_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      IRSSAVregInfo *xvi = ssa_opt_vinfo(ctx, x_vr);
      if (!xvi || xvi->def_count != 1 || xvi->def_instr < 0)
        continue;
      if (cfg->instr_to_block[xvi->def_instr] != cfg->instr_to_block[idx])
        continue;
      IRQuadCompact *xdef = &ctx->ir->compact_instructions[xvi->def_instr];
      if (xdef->op != TCCIR_OP_XOR)
        continue;
      IROperand xs1 = tcc_ir_op_get_src1(ir, xdef);
      IROperand xs2 = tcc_ir_op_get_src2(ir, xdef);
      /* Identify which XOR operand carries the -1 constant.  The other
       * operand is the value being complemented; it must match the OR/AND's
       * "a" operand (same vreg, non-lval). */
      int s1_neg1 = (xs1.tag == IROP_TAG_IMM32 && !xs1.is_lval && xs1.u.imm32 == -1);
      int s2_neg1 = (xs2.tag == IROP_TAG_IMM32 && !xs2.is_lval && xs2.u.imm32 == -1);
      if (!s1_neg1 && !s2_neg1)
        continue;
      IROperand x_inner = s1_neg1 ? xs2 : xs1;
      if (x_inner.is_lval || x_inner.tag != IROP_TAG_VREG)
        continue;
      if (irop_get_vreg(x_inner) != a_vr)
        continue;
      /* Match.  OR -> -1, AND -> 0. */
      int32_t fold_val = (q->op == TCCIR_OP_OR) ? -1 : 0;
      IROperand imm = irop_make_imm32(0, fold_val, dest.btype);
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_op_set_src1(ir, q, imm);
      tcc_ir_op_set_src2(ir, q, IROP_NONE);
      IRSSAVregInfo *vi;
      vi = ssa_opt_vinfo(ctx, a_vr);
      if (vi) ssa_opt_remove_use_instr(vi, idx);
      vi = ssa_opt_vinfo(ctx, x_vr);
      if (vi) ssa_opt_remove_use_instr(vi, idx);
      return 1;
    }
  }

  /* Identity: x + 0, x - 0, x | 0, x ^ 0, x << 0, x >> 0, x * 1 → x */
  if (src2_is_imm && !src1.is_lval) {
    int is_identity = 0;
    int is_absorb_zero = 0;
    switch (q->op) {
    case TCCIR_OP_ADD: is_identity = (val2 == 0); break;
    case TCCIR_OP_SUB: is_identity = (val2 == 0); break;
    case TCCIR_OP_OR:  is_identity = (val2 == 0); break;
    case TCCIR_OP_XOR: is_identity = (val2 == 0); break;
    case TCCIR_OP_SHL: is_identity = (val2 == 0); break;
    case TCCIR_OP_SHR: is_identity = (val2 == 0); break;
    case TCCIR_OP_SAR: is_identity = (val2 == 0); break;
    case TCCIR_OP_ROR: is_identity = (val2 == 0); break;
    case TCCIR_OP_MUL: is_identity = (val2 == 1); is_absorb_zero = (val2 == 0); break;
    case TCCIR_OP_AND: is_identity = (val2 == -1); is_absorb_zero = (val2 == 0); break;
    default: break;
    }
    if (is_identity) {
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_op_set_src2(ir, q, IROP_NONE);
      return 1;
    }
    if (is_absorb_zero) {
      IROperand imm = irop_make_imm32(0, 0, dest.btype);
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_op_set_src1(ir, q, imm);
      tcc_ir_op_set_src2(ir, q, IROP_NONE);
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, src1_vr);
      if (vi) ssa_opt_remove_use_instr(vi, idx);
      return 1;
    }
  }

  /* Double-negation collapse: `T_b = #0 SUB T_a` where T_a's single def is
   * `T_a = #0 SUB T_z` → fold to `T_b = ASSIGN T_z`.  Iteratively with cprop
   * + GVN this collapses goto-chain idioms like gcc.c-torture/compile/961126-1.c
   * where `i = -i; if (*p != i) goto quit;` is repeated 32 times — each
   * alternate iteration's SUB folds away. */
  if (q->op == TCCIR_OP_SUB && src1_is_imm && val1 == 0 && !src2_is_imm &&
      src2.tag == IROP_TAG_VREG && !src2.is_lval &&
      src2_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src2_vr) == TCCIR_VREG_TYPE_TEMP) {
    IRSSAVregInfo *avi = ssa_opt_vinfo(ctx, src2_vr);
    if (avi && avi->def_count == 1 && avi->def_instr >= 0) {
      IRQuadCompact *dq = &ctx->ir->compact_instructions[avi->def_instr];
      if (dq->op == TCCIR_OP_SUB) {
        IROperand ds1 = tcc_ir_op_get_src1(ir, dq);
        IROperand ds2 = tcc_ir_op_get_src2(ir, dq);
        if (ds1.tag == IROP_TAG_IMM32 && !ds1.is_lval && ds1.u.imm32 == 0 &&
            ds2.tag == IROP_TAG_VREG && !ds2.is_lval) {
          /* Width must match — the inner SUB writes T_a with the dest btype;
           * if the outer SUB has a different width, the fold would skip an
           * implicit narrowing/widening that the second negation enforces. */
          if (irop_get_btype(dest) == irop_get_btype(ds2)) {
            IROperand new_src = ds2;
            new_src.is_lval = 0;
            q->op = TCCIR_OP_ASSIGN;
            tcc_ir_op_set_src1(ir, q, new_src);
            tcc_ir_op_set_src2(ir, q, IROP_NONE);
            ssa_opt_remove_use_instr(avi, idx);
            int32_t tz_vr = irop_get_vreg(ds2);
            IRSSAVregInfo *zvi = ssa_opt_vinfo(ctx, tz_vr);
            if (zvi)
              ssa_opt_add_use_instr(zvi, idx);
            return 1;
          }
        }
      }
    }
  }

  /* Commutative identity: 0 + x, 0 | x, 0 ^ x, 1 * x → x */
  if (src1_is_imm && !src2.is_lval && src2.tag == IROP_TAG_VREG) {
    int is_identity = 0;
    switch (q->op) {
    case TCCIR_OP_ADD: is_identity = (val1 == 0); break;
    case TCCIR_OP_OR:  is_identity = (val1 == 0); break;
    case TCCIR_OP_XOR: is_identity = (val1 == 0); break;
    case TCCIR_OP_MUL: is_identity = (val1 == 1); break;
    case TCCIR_OP_AND: is_identity = (val1 == -1); break;
    default: break;
    }
    if (is_identity) {
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_op_set_src1(ir, q, src2);
      tcc_ir_op_set_src2(ir, q, IROP_NONE);
      return 1;
    }
    /* Absorbing: 0 * x, 0 & x → 0 */
    int is_absorb = 0;
    switch (q->op) {
    case TCCIR_OP_MUL: is_absorb = (val1 == 0); break;
    case TCCIR_OP_AND: is_absorb = (val1 == 0); break;
    default: break;
    }
    if (is_absorb) {
      IROperand imm = irop_make_imm32(0, 0, dest.btype);
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_op_set_src1(ir, q, imm);
      tcc_ir_op_set_src2(ir, q, IROP_NONE);
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, src2_vr);
      if (vi) ssa_opt_remove_use_instr(vi, idx);
      return 1;
    }
  }

  return 0;
}

/* ============================================================================
 * Generator Table
 * ============================================================================ */

static const IRSSAOptGen fold_gens[] = {
  { TCCIR_OP_ADD,  fold_binary, "fold_add" },
  { TCCIR_OP_SUB,  fold_binary, "fold_sub" },
  { TCCIR_OP_MUL,  fold_binary, "fold_mul" },
  { TCCIR_OP_DIV,  fold_binary, "fold_div" },
  { TCCIR_OP_UDIV, fold_binary, "fold_udiv" },
  { TCCIR_OP_IMOD,  fold_binary, "fold_mod" },
  { TCCIR_OP_UMOD, fold_binary, "fold_umod" },
  { TCCIR_OP_AND,  fold_binary, "fold_and" },
  { TCCIR_OP_OR,   fold_binary, "fold_or" },
  { TCCIR_OP_XOR,  fold_binary, "fold_xor" },
  { TCCIR_OP_SHL,  fold_binary, "fold_shl" },
  { TCCIR_OP_SHR,  fold_binary, "fold_shr" },
  { TCCIR_OP_SAR,  fold_binary, "fold_sar" },
  { TCCIR_OP_ROR,  fold_binary, "fold_ror" },
};

/* ============================================================================
 * Pass Entry Point
 * ============================================================================ */

int ssa_opt_fold(IRSSAOptCtx *ctx)
{
  return ssa_opt_run_gens(ctx, fold_gens, sizeof(fold_gens) / sizeof(fold_gens[0]));
}

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

static int fold_binary(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  IROperand dest = tcc_ir_op_get_dest(ir, q);

  int32_t dest_vr = irop_get_vreg(dest);
  if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  int src1_is_imm = (src1.tag == IROP_TAG_IMM32 && !src1.is_lval);
  int src2_is_imm = (src2.tag == IROP_TAG_IMM32 && !src2.is_lval);
  int32_t val1 = src1.u.imm32;
  int32_t val2 = src2.u.imm32;

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
      result = val1 / val2;
      break;
    case TCCIR_OP_UDIV:
      if (val2 == 0) return 0;
      result = (uint32_t)val1 / (uint32_t)val2;
      break;
    case TCCIR_OP_IMOD:
      if (val2 == 0) return 0;
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

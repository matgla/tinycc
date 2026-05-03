/*
 *  TCC IR - SSA Branch Folding
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
 * Branch Folding: when CMP or TEST_ZERO has constant operands (after cprop
 * propagated immediates), evaluate the comparison at compile time and convert
 * the JUMPIF to unconditional JUMP or NOP.
 *
 * Patterns:
 *   CMP #a, #b; JUMPIF cond → JUMP (if cond(a,b) is true)
 *   CMP #a, #b; JUMPIF cond → NOP  (if cond(a,b) is false)
 *   TEST_ZERO #a; JUMPIF EQ  → JUMP/NOP based on a==0
 *   CMP #a, #b; SETIF cond  → ASSIGN #0 or #1
 * ============================================================================ */

static int eval_cond(int64_t v1, int64_t v2, int tok)
{
  switch (tok) {
  case 0x94: return v1 == v2;
  case 0x95: return v1 != v2;
  case 0x9c: return v1 < v2;
  case 0x9d: return v1 >= v2;
  case 0x9e: return v1 <= v2;
  case 0x9f: return v1 > v2;
  case 0x92: return (uint64_t)v1 < (uint64_t)v2;
  case 0x93: return (uint64_t)v1 >= (uint64_t)v2;
  case 0x96: return (uint64_t)v1 <= (uint64_t)v2;
  case 0x97: return (uint64_t)v1 > (uint64_t)v2;
  default: return -1;
  }
}

static int ssa_fold_cmp_jumpif(IRSSAOptCtx *ctx, int cmp_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *cmp_q = &ir->compact_instructions[cmp_idx];
  int n = ir->next_instruction_index;

  IROperand src1 = tcc_ir_op_get_src1(ir, cmp_q);
  IROperand src2 = tcc_ir_op_get_src2(ir, cmp_q);

  int64_t v1, v2;
  int have_values = 0;

  /* Case 1: both operands are immediates (chase ASSIGN #const) */
  {
    IROperand ops[2] = { src1, src2 };
    int64_t vals[2];
    int got[2] = { 0, 0 };
    for (int oi = 0; oi < 2; oi++) {
      if (irop_is_immediate(ops[oi])) {
        vals[oi] = irop_get_imm64_ex(ir, ops[oi]);
        got[oi] = 1;
      } else {
        int32_t vr = irop_get_vreg(ops[oi]);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
            ops[oi].tag == IROP_TAG_VREG && !ops[oi].is_lval) {
          IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
          if (vi && vi->def_instr >= 0 && vi->def_count <= 1) {
            IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
            if (dq->op == TCCIR_OP_ASSIGN) {
              IROperand ds = tcc_ir_op_get_src1(ir, dq);
              if (irop_is_immediate(ds) && !ds.is_lval) {
                vals[oi] = irop_get_imm64_ex(ir, ds);
                got[oi] = 1;
              }
            }
          }
        }
      }
    }
    if (got[0] && got[1]) {
      v1 = vals[0];
      v2 = vals[1];
      /* Truncate to operand width to avoid sign-extension mismatches */
      int cmp_btype = irop_get_btype(src1);
      if (cmp_btype != IROP_BTYPE_INT64) {
        v1 = (int64_t)(int32_t)(uint32_t)v1;
        v2 = (int64_t)(int32_t)(uint32_t)v2;
      }
      have_values = 1;
    }
  }

  /* Case 2: both operands resolve to the same SSA TEMP vreg (CMP x, x).
   * Chase single-def ASSIGN copies to find the root vreg.
   * Use 0,0 as representative — all reflexive comparisons give the
   * same boolean result regardless of the actual value. */
  if (!have_values) {
    int32_t vr1 = irop_get_vreg(src1);
    int32_t vr2 = irop_get_vreg(src2);

    /* Chase ASSIGN copies: T6 = T0 → root is T0 */
    for (int hop = 0; hop < 4 && vr1 >= 0; hop++) {
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr1);
      if (!vi || vi->def_instr < 0 || vi->def_count > 1) break;
      IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
      if (dq->op != TCCIR_OP_ASSIGN) break;
      IROperand ds = tcc_ir_op_get_src1(ir, dq);
      if (ds.is_lval || ds.tag != IROP_TAG_VREG) break;
      int32_t nv = irop_get_vreg(ds);
      if (nv < 0 || TCCIR_DECODE_VREG_TYPE(nv) != TCCIR_VREG_TYPE_TEMP) break;
      vr1 = nv;
    }
    for (int hop = 0; hop < 4 && vr2 >= 0; hop++) {
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr2);
      if (!vi || vi->def_instr < 0 || vi->def_count > 1) break;
      IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
      if (dq->op != TCCIR_OP_ASSIGN) break;
      IROperand ds = tcc_ir_op_get_src1(ir, dq);
      if (ds.is_lval || ds.tag != IROP_TAG_VREG) break;
      int32_t nv = irop_get_vreg(ds);
      if (nv < 0 || TCCIR_DECODE_VREG_TYPE(nv) != TCCIR_VREG_TYPE_TEMP) break;
      vr2 = nv;
    }

    if (vr1 >= 0 && vr1 == vr2 &&
        TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_TEMP &&
        src1.is_lval == src2.is_lval &&
        src1.tag == IROP_TAG_VREG && src2.tag == IROP_TAG_VREG) {
      v1 = 0;
      v2 = 0;
      have_values = 1;
    }
  }

  if (!have_values)
    return 0;

  int j = cmp_idx + 1;
  while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
    j++;
  if (j >= n)
    return 0;

  IRQuadCompact *next_q = &ir->compact_instructions[j];

  if (next_q->op == TCCIR_OP_JUMPIF) {
    IROperand cond = tcc_ir_op_get_src1(ir, next_q);
    int tok = (int)irop_get_imm64_ex(ir, cond);
    int result = eval_cond(v1, v2, tok);
    if (result < 0)
      return 0;

    /* Remove uses of CMP operands */
    IRSSAVregInfo *vi;
    vi = ssa_opt_vinfo(ctx, irop_get_vreg(src1));
    if (vi) ssa_opt_remove_use_instr(vi, cmp_idx);
    vi = ssa_opt_vinfo(ctx, irop_get_vreg(src2));
    if (vi) ssa_opt_remove_use_instr(vi, cmp_idx);

    if (result) {
      IROperand dest = tcc_ir_op_get_dest(ir, next_q);
      cmp_q->op = TCCIR_OP_NOP;
      next_q->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, j, dest);
    } else {
      cmp_q->op = TCCIR_OP_NOP;
      next_q->op = TCCIR_OP_NOP;
    }
    return 1;
  }

  if (next_q->op == TCCIR_OP_SETIF) {
    IROperand cond = tcc_ir_op_get_src1(ir, next_q);
    int tok = (int)irop_get_imm64_ex(ir, cond);
    int result = eval_cond(v1, v2, tok);
    if (result < 0)
      return 0;

    IRSSAVregInfo *vi;
    vi = ssa_opt_vinfo(ctx, irop_get_vreg(src1));
    if (vi) ssa_opt_remove_use_instr(vi, cmp_idx);
    vi = ssa_opt_vinfo(ctx, irop_get_vreg(src2));
    if (vi) ssa_opt_remove_use_instr(vi, cmp_idx);

    IROperand dest = tcc_ir_op_get_dest(ir, next_q);
    IROperand imm = irop_make_imm32(0, result ? 1 : 0, dest.btype);
    cmp_q->op = TCCIR_OP_NOP;
    next_q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, j, imm);
    tcc_ir_set_src2(ir, j, IROP_NONE);
    return 1;
  }

  return 0;
}

static int ssa_fold_test_zero(IRSSAOptCtx *ctx, int tz_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *tz_q = &ir->compact_instructions[tz_idx];
  int n = ir->next_instruction_index;

  IROperand src1 = tcc_ir_op_get_src1(ir, tz_q);
  if (!irop_is_immediate(src1))
    return 0;

  int64_t val = irop_get_imm64_ex(ir, src1);

  int j = tz_idx + 1;
  while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
    j++;
  if (j >= n)
    return 0;

  IRQuadCompact *next_q = &ir->compact_instructions[j];
  if (next_q->op != TCCIR_OP_JUMPIF)
    return 0;

  IROperand cond = tcc_ir_op_get_src1(ir, next_q);
  int tok = (int)irop_get_imm64_ex(ir, cond);

  int branch_taken;
  if (tok == 0x94)
    branch_taken = (val == 0);
  else if (tok == 0x95)
    branch_taken = (val != 0);
  else
    return 0;

  if (branch_taken) {
    IROperand dest = tcc_ir_op_get_dest(ir, next_q);
    tz_q->op = TCCIR_OP_NOP;
    next_q->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, j, dest);
  } else {
    tz_q->op = TCCIR_OP_NOP;
    next_q->op = TCCIR_OP_NOP;
  }
  return 1;
}

static const IRSSAOptGen branch_gens[] = {
  { TCCIR_OP_CMP,       ssa_fold_cmp_jumpif, "branch_cmp" },
  { TCCIR_OP_TEST_ZERO, ssa_fold_test_zero,  "branch_tz" },
};

int ssa_opt_branch(IRSSAOptCtx *ctx)
{
  return ssa_opt_run_gens(ctx, branch_gens,
                          sizeof(branch_gens) / sizeof(branch_gens[0]));
}

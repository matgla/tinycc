/*
 *  TCC IR - SSA Reassociation
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
#include "opt_dsl.h"
#include "opt_dsl_ssa.h"
#include "opt/ssa/reassoc.h"
#include "opt/ssa/ssa_opt_helpers.h"

/* ============================================================================
 * Reassociation: reorder associative/commutative operations so that constant
 * operands bubble together, enabling the fold pass to collapse them.
 *
 *   (x + c1) + c2  →  x + (c1 + c2)   [fold handles the c1+c2 part]
 *   (x - c1) + c2  →  x + (c2 - c1)
 *   (x + c1) - c2  →  x + (c1 - c2)
 *   (x OP c1) OP c2 → x OP (c1 OP c2)   for MUL/AND/OR/XOR/SHL/SHR/SAR/ROR
 *
 * Expressed via source/opt/framework: the inner def is the single-use TEMP
 * producer of src1, resolved by PAIR; RETIRE_PAIR forwards the use onto the
 * inner's own src1.  reassoc_add_cancel additionally matches BOTH source
 * producers — PAIR binds the src1 def, the src2 def is resolved by hand.
 * ============================================================================ */

/* Combine the two immediates for `(x inner_op c1) outer_op c2`, choosing the
 * resulting opcode.  Returns 0 (no rewrite) for op pairs that do not reassociate
 * or shift amounts that would overflow 32 bits.  Mirrors the legacy switch. */
static int reassoc_combine(int outer_op, int inner_op, int32_t c1, int32_t c2,
                           int *new_op, int32_t *combined)
{
  *new_op = outer_op;
  if (outer_op == TCCIR_OP_ADD && inner_op == TCCIR_OP_ADD) {
    *combined = c1 + c2;
  } else if (outer_op == TCCIR_OP_ADD && inner_op == TCCIR_OP_SUB) {
    int32_t v = c2 - c1;
    *new_op = v >= 0 ? TCCIR_OP_ADD : TCCIR_OP_SUB;
    *combined = v >= 0 ? v : -v;
  } else if (outer_op == TCCIR_OP_SUB && inner_op == TCCIR_OP_ADD) {
    int32_t v = c1 - c2;
    *new_op = v >= 0 ? TCCIR_OP_ADD : TCCIR_OP_SUB;
    *combined = v >= 0 ? v : -v;
  } else if (outer_op == TCCIR_OP_SUB && inner_op == TCCIR_OP_SUB) {
    *new_op = TCCIR_OP_SUB;
    *combined = c1 + c2;
  } else if (outer_op == inner_op) {
    switch (outer_op) {
    case TCCIR_OP_MUL: *combined = c1 * c2; break;
    case TCCIR_OP_AND: *combined = c1 & c2; break;
    case TCCIR_OP_OR:  *combined = c1 | c2; break;
    case TCCIR_OP_XOR: *combined = c1 ^ c2; break;
    case TCCIR_OP_SHL:
    case TCCIR_OP_SHR:
    case TCCIR_OP_SAR:
      if (c1 + c2 >= 32) return 0;
      *combined = c1 + c2;
      break;
    case TCCIR_OP_ROR: *combined = (c1 + c2) & 31; break;
    default: return 0;
    }
  } else {
    return 0;
  }
  return 1;
}

/* `(a OP1 c1)` and `(a OP2 c2)` cancel when c1*sign1 + c2*sign2 == 0, sign from
 * ADD (+1) / SUB (-1).  64-bit accumulate to avoid INT_MIN overflow. */
static int reassoc_consts_cancel(int op1, int op2, IROperand imm1, IROperand imm2)
{
  int32_t c1 = irop_get_imm32(imm1);
  int32_t c2 = irop_get_imm32(imm2);
  int s1 = (op1 == TCCIR_OP_ADD) ? 1 : -1;
  int s2 = (op2 == TCCIR_OP_ADD) ? 1 : -1;
  return (int64_t)c1 * s1 + (int64_t)c2 * s2 == 0;
}

/* (x OP c1) OP c2 → x OP (c1 OP c2).  Serves every reassociable binary op; the
 * generator reads q->op at runtime, so one dispatch covers the whole table. */
OPT_GEN_SSA(reassoc_bin, 0) {
  int new_op = OPT_DSL_KEEP_OP;
  int32_t combined = 0;
  PATTERN(.constraints = { .src2 = IR_CONSTRAINT_IMM });
  GUARD(
    when(is_imm32(src2));
    and_not(tcc_ir_barrel_shift_at(ir, q)));
  PAIR(.link = IR_PAIR_DEF_OF_SRC1, .op = -1, .single_use = 1);
  GUARD(
    when(is_imm32(psrc2));
    and_not(psrc1.is_lval);
    and_not(psrc1.is_local);
    and_not(psrc1.is_llocal);
    and_not(tcc_ir_barrel_shift_at(ir, &ir->compact_instructions[pidx]));
    and(reassoc_combine(q->op, pop, (int32_t)psrc2.u.imm32,
                        (int32_t)src2.u.imm32, &new_op, &combined)));
  RETIRE_PAIR(psrc1, 0);
  REWRITE(.new_op = new_op, .src1 = psrc1,
          .src2 = mk_imm_bt(combined, dest.btype));
}

/* (a + c) + (a - c) → a + a (and the symmetric orderings): both operands are
 * single-use TEMPs defined by ADD/SUB of the same base by canceling constants. */
OPT_GEN_SSA(reassoc_add_cancel, TCCIR_OP_ADD) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(src1.tag == IROP_TAG_VREG);
    and(src2.tag == IROP_TAG_VREG);
    and_not(src1.is_lval);
    and_not(src2.is_lval);
    and(TCCIR_DECODE_VREG_TYPE(vreg(src1)) == TCCIR_VREG_TYPE_TEMP);
    and(TCCIR_DECODE_VREG_TYPE(vreg(src2)) == TCCIR_VREG_TYPE_TEMP);
    and_not(tcc_ir_barrel_shift_at(ir, q)));

  PAIR(.link = IR_PAIR_DEF_OF_SRC1, .op = -1, .single_use = 1);

  IRSSAVregInfo *vi2 = ssa_opt_vinfo(ctx, vreg(src2));
  GUARD(
    when(vi2 != NULL);
    and(vi2->def_count == 1);
    and(vi2->use_count == 1);
    and(vi2->def_instr >= 0));

  IRQuadCompact *d1 = &ir->compact_instructions[pidx];
  IRQuadCompact *d2 = &ir->compact_instructions[vi2->def_instr];
  IROperand d2s1 = tcc_ir_op_get_src1(ir, d2);
  IROperand d2s2 = tcc_ir_op_get_src2(ir, d2);
  GUARD(
    when(pop == TCCIR_OP_ADD || pop == TCCIR_OP_SUB);
    and(d2->op == TCCIR_OP_ADD || d2->op == TCCIR_OP_SUB);
    and_not(tcc_ir_barrel_shift_at(ir, d1));
    and_not(tcc_ir_barrel_shift_at(ir, d2));
    and(psrc1.tag == IROP_TAG_VREG);
    and(d2s1.tag == IROP_TAG_VREG);
    and_not(psrc1.is_lval);
    and_not(d2s1.is_lval);
    and(irop_is_immediate(psrc2));
    and(irop_is_immediate(d2s2));
    and(vreg(psrc1) == vreg(d2s1));
    and(reassoc_consts_cancel(pop, d2->op, psrc2, d2s2));
    and(irop_get_btype(psrc1) == irop_get_btype(dest)));

  IROperand a_op = irop_make_vreg(vreg(psrc1), irop_get_btype(dest));
  RETIRE_PAIR(a_op, 0);
  opt_dsl_drop_use(ctx, src2, i);
  opt_dsl_add_use(ctx, a_op, i);
  REWRITE(.src1 = a_op, .src2 = a_op);
}

/* ADD tries the cancel pattern first, then the generic constant merge. */
static int reassoc_add_dispatch(IRSSAOptCtx *ctx, int i)
{
  int r = opt_dsl_dispatch_reassoc_add_cancel(ctx, i);
  if (r)
    return r;
  return opt_dsl_dispatch_reassoc_bin(ctx, i);
}

static const IRSSAOptGen reassoc_gens[] = {
  { TCCIR_OP_ADD, reassoc_add_dispatch,         "reassoc_add" },
  { TCCIR_OP_SUB, opt_dsl_dispatch_reassoc_bin, "reassoc_sub" },
  { TCCIR_OP_MUL, opt_dsl_dispatch_reassoc_bin, "reassoc_mul" },
  { TCCIR_OP_AND, opt_dsl_dispatch_reassoc_bin, "reassoc_and" },
  { TCCIR_OP_OR,  opt_dsl_dispatch_reassoc_bin, "reassoc_or" },
  { TCCIR_OP_XOR, opt_dsl_dispatch_reassoc_bin, "reassoc_xor" },
  { TCCIR_OP_SHL, opt_dsl_dispatch_reassoc_bin, "reassoc_shl" },
  { TCCIR_OP_SHR, opt_dsl_dispatch_reassoc_bin, "reassoc_shr" },
  { TCCIR_OP_SAR, opt_dsl_dispatch_reassoc_bin, "reassoc_sar" },
  { TCCIR_OP_ROR, opt_dsl_dispatch_reassoc_bin, "reassoc_ror" },
};

int ssa_opt_reassoc(IRSSAOptCtx *ctx)
{
  return ssa_opt_run_gens(ctx, reassoc_gens, OPT_DSL_TABLE_COUNT(reassoc_gens));
}

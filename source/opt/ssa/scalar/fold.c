/*
 *  TCC SSA opt - constant folding (ALU ops with constant operands)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
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
#include "opt/ssa/fold.h"
#include "opt/ssa/ssa_opt_helpers.h"
#include "opt_xform.h"

/* 32-bit immediate, inline (IMM32) or pooled (I64 tag on a non-64-bit operand);
 * pool constants keep only their low 32 bits — exact for 32-bit arithmetic. */
static int fold_read_imm32(const TCCIRState *ir, IROperand op, int32_t *out)
{
  if (op.is_lval)
    return 0;
  if (op.tag == IROP_TAG_IMM32) {
    *out = op.u.imm32;
    return 1;
  }
  if (op.tag == IROP_TAG_I64 && irop_get_btype(op) != IROP_BTYPE_INT64) {
    int64_t v = irop_get_imm64_ex(ir, op);
    if (v != (int64_t)(int32_t)v && v != (int64_t)(uint32_t)v)
      return 0;
    *out = (int32_t)v;
    return 1;
  }
  return 0;
}

/* Single dominating def of a TEMP operand — a non-dominating join can lose
 * information (the bug_switch_goto_or pattern; see cprop_imm in ssa_opt_cprop.c). */
static IRQuadCompact *fold_single_dom_def(IRSSAOptCtx *ctx, IROperand op, int use_idx)
{
  if (op.is_lval || op.is_local || op.is_llocal || op.is_sym)
    return NULL;
  if (op.tag != IROP_TAG_VREG)
    return NULL;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return NULL;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_count != 1 || vi->def_instr < 0)
    return NULL;
  IRCFG *cfg = ctx->cfg;
  if (!cfg)
    return NULL;
  int def_blk = cfg->instr_to_block[vi->def_instr];
  int use_blk = cfg->instr_to_block[use_idx];
  if (def_blk != use_blk) {
    int b = use_blk, steps = 0;
    while (b >= 0 && b != def_blk && steps++ < cfg->num_blocks) {
      int id = cfg->blocks[b].idom;
      if (id == b)
        break;
      b = id;
    }
    if (b != def_blk)
      return NULL;
  }
  return &ctx->ir->compact_instructions[vi->def_instr];
}

/* Resolve a vreg to its constant `ASSIGN #imm` single dominating def. */
static int try_resolve_const_vreg(IRSSAOptCtx *ctx, IROperand op, int use_idx, int32_t *out_val)
{
  IRQuadCompact *dq = fold_single_dom_def(ctx, op, use_idx);
  if (!dq || dq->op != TCCIR_OP_ASSIGN)
    return 0;
  return fold_read_imm32(ctx->ir, tcc_ir_op_get_src1(ctx->ir, dq), out_val);
}

static void fold_clear_barrel(TCCIRState *ir, const IRQuadCompact *q)
{
  if (ir->barrel_shifts && q->orig_index >= 0 && q->orig_index < ir->barrel_shifts_len)
    ir->barrel_shifts[q->orig_index] = 0;
}

/* Materialise src operands whose single dominating ASSIGN def carries a
 * constant as real immediates; DCE later removes the dead def. */
static void fold_materialize_consts(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  int32_t v;
  if (!fold_read_imm32(ir, src1, &v) && try_resolve_const_vreg(ctx, src1, idx, &v)) {
    tcc_ir_op_set_src1(ir, q, mk_imm_bt(v, irop_get_btype(src1)));
    opt_dsl_drop_use(ctx, src1, idx);
  }
  if (!fold_read_imm32(ir, src2, &v) && try_resolve_const_vreg(ctx, src2, idx, &v)) {
    tcc_ir_op_set_src2(ir, q, mk_imm_bt(v, irop_get_btype(src2)));
    opt_dsl_drop_use(ctx, src2, idx);
  }
}

/* Apply a barrel-shift annotation ((stype<<5)|amount) to a maybe-set-bits mask. */
static uint32_t fold_apply_barrel(uint32_t m, uint8_t bs)
{
  int amount = bs & 0x1F;
  switch (bs >> 5) {
  case 1: return m << amount;
  case 2: return m >> amount;
  case 3: return (m >> amount) |
                 (((m & 0x80000000u) && amount) ? ~(0xFFFFFFFFu >> amount) : 0u);
  case 4: return amount ? ((m >> amount) | (m << (32 - amount))) : m;
  default: return m;
  }
}

/* Upper bound on the possibly-set bits of a 32-bit value, walking single-dom-def
 * AND/OR/XOR/SHL/SHR/SAR/ASSIGN chains (barrel-annotation-aware, runs
 * post-fusion); anything unmodeled is all-ones; *budget caps node visits. */
static uint32_t fold_maybe_set_bits(IRSSAOptCtx *ctx, IROperand op, int at_idx,
                                    int depth, int *budget)
{
  TCCIRState *ir = ctx->ir;
  int32_t v;
  if (fold_read_imm32(ir, op, &v))
    return (uint32_t)v;
  if (depth <= 0 || --*budget <= 0 || irop_is_64bit(op))
    return 0xFFFFFFFFu;
  IRQuadCompact *dq = fold_single_dom_def(ctx, op, at_idx);
  if (!dq)
    return 0xFFFFFFFFu;
  int d_idx = (int)(dq - ir->compact_instructions);
  IROperand dd = tcc_ir_op_get_dest(ir, dq);
  if (irop_is_64bit(dd) || dd.is_lval)
    return 0xFFFFFFFFu;
  uint8_t bs = tcc_ir_barrel_shift_at(ir, dq);
  IROperand s1 = irop_config[dq->op].has_src1 ? tcc_ir_op_get_src1(ir, dq) : IROP_NONE;
  IROperand s2 = irop_config[dq->op].has_src2 ? tcc_ir_op_get_src2(ir, dq) : IROP_NONE;
  switch (dq->op) {
  case TCCIR_OP_AND:
    return fold_maybe_set_bits(ctx, s1, d_idx, depth - 1, budget) &
           fold_apply_barrel(fold_maybe_set_bits(ctx, s2, d_idx, depth - 1, budget), bs);
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
    return fold_maybe_set_bits(ctx, s1, d_idx, depth - 1, budget) |
           fold_apply_barrel(fold_maybe_set_bits(ctx, s2, d_idx, depth - 1, budget), bs);
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR: {
    if (bs)
      return 0xFFFFFFFFu;
    int32_t sh;
    if (!fold_read_imm32(ir, s2, &sh) || sh < 0 || sh >= 32)
      return 0xFFFFFFFFu;
    uint32_t m = fold_maybe_set_bits(ctx, s1, d_idx, depth - 1, budget);
    if (dq->op == TCCIR_OP_SHL)
      return m << sh;
    if (dq->op == TCCIR_OP_SHR)
      return m >> sh;
    return (m >> sh) | ((m & 0x80000000u) && sh ? ~(0xFFFFFFFFu >> sh) : 0u);
  }
  case TCCIR_OP_ASSIGN:
    if (irop_get_btype(dd) == IROP_BTYPE_INT32 && !s1.is_lval)
      return fold_maybe_set_bits(ctx, s1, d_idx, depth - 1, budget);
    return 0xFFFFFFFFu;
  default:
    return 0xFFFFFFFFu;
  }
}

/* Barrel-shift-annotated op: fold only the all-constant case, peek-only —
 * materialising into an annotated src2 would drop the shift (the
 * var_to_param_forward LSL-drop bug class). */
OPT_GEN_SSA(fold_barrel, -1) {
  int32_t v1 = 0, v2 = 0;
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(!irop_is_64bit(dest) && !irop_is_64bit(src1) && !irop_is_64bit(src2));
    and(fold_read_imm32(ir, src1, &v1) || try_resolve_const_vreg(ctx, src1, i, &v1));
    and(fold_read_imm32(ir, src2, &v2) || try_resolve_const_vreg(ctx, src2, i, &v2)));
  uint8_t bs = tcc_ir_barrel_shift_at(ir, q);
  uint32_t stype = (bs >> 5) & 7;
  uint32_t samt = bs & 31;
  uint32_t u2 = (uint32_t)v2;
  switch (stype) {
  case 1: u2 <<= samt; break;
  case 2: u2 = (samt == 0) ? 0 : u2 >> samt; break;
  case 3: u2 = (uint32_t)((int32_t)u2 >> (samt == 0 ? 31 : samt)); break;
  case 4:
    if (samt == 0)
      return 0;
    u2 = (u2 >> samt) | (u2 << (32 - samt));
    break;
  default:
    return 0;
  }
  uint32_t u1 = (uint32_t)v1;
  uint32_t bres;
  switch (q->op) {
  case TCCIR_OP_ADD: bres = u1 + u2; break;
  case TCCIR_OP_SUB: bres = u1 - u2; break;
  case TCCIR_OP_AND: bres = u1 & u2; break;
  case TCCIR_OP_OR:  bres = u1 | u2; break;
  case TCCIR_OP_XOR: bres = u1 ^ u2; break;
  default:
    return 0;
  }
  if (src1.tag == IROP_TAG_VREG)
    opt_dsl_drop_use(ctx, src1, i);
  if (src2.tag == IROP_TAG_VREG)
    opt_dsl_drop_use(ctx, src2, i);
  fold_clear_barrel(ir, q);
  REWRITE(.new_op = TCCIR_OP_ASSIGN,
          .src1 = mk_imm_bt((int32_t)bres, irop_get_btype(dest)));
}

/* symref+addend ± #imm -> ASSIGN symref+(addend±imm), so address consumers
 * (ssa:const_string_fold in particular) see a constant symbol address. */
OPT_GEN_SSA(fold_symref_addend, -1) {
  int32_t off = 0;
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB);
    and(irop_get_tag(src1) == IROP_TAG_SYMREF && !src1.is_lval);
    and(fold_read_imm32(ir, src2, &off)));
  IRPoolSymref *sr = irop_get_symref_ex(ir, src1);
  if (!sr || !sr->sym)
    return 0;
  int32_t na = (q->op == TCCIR_OP_ADD) ? sr->addend + off : sr->addend - off;
  uint32_t nidx = tcc_ir_pool_add_symref(ir, sr->sym, na, sr->flags);
  REWRITE(.new_op = TCCIR_OP_ASSIGN,
          .src1 = irop_make_symref(0, nidx, 0, src1.is_local, src1.is_const,
                                   irop_get_btype(src1)));
}

/* Division/modulo by constant 0 is UB (C11 6.5.5p5): rewrite to TRAP and NOP
 * the block tail so TRAP becomes the terminator (mirrors div_by_zero_trap). */
OPT_GEN_SSA(fold_div_zero_trap, -1) {
  int32_t v2 = 0;
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(q->op == TCCIR_OP_DIV || q->op == TCCIR_OP_UDIV ||
         q->op == TCCIR_OP_IMOD || q->op == TCCIR_OP_UMOD);
    and(fold_read_imm32(ir, src2, &v2) && v2 == 0);
    and(ctx->cfg && i >= 0 && i < ctx->cfg->num_instrs));
  opt_dsl_drop_use(ctx, src1, i);
  opt_dsl_drop_use(ctx, src2, i);
  int end = ctx->cfg->blocks[ctx->cfg->instr_to_block[i]].end_idx;
  for (int j = i + 1; j < end && j < ir->next_instruction_index; j++)
    ssa_opt_nop_instr(ctx, j);
  REWRITE(.new_op = TCCIR_OP_TRAP);
}

/* dest = A SELECT A [cond]  ->  dest = A.  Identical immediate arms make the
 * result independent of the condition, whose value then becomes dead (DCE'd).
 * Cleans up const diamonds that vrp / if-conversion leave as `#k SELECT #k`
 * (sccp propagates through instruction defs, not phi/select-fed merges). */
OPT_GEN_SSA(fold_select_equal, TCCIR_OP_SELECT) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(irop_get_vreg(src1) < 0 && irop_get_vreg(src2) < 0);
    and(irop_is_immediate(src1) && irop_is_immediate(src2) &&
        !src1.is_sym && !src2.is_sym &&
        irop_get_btype(src1) == irop_get_btype(src2) &&
        irop_get_imm64_ex(ir, src1) == irop_get_imm64_ex(ir, src2)));
  opt_dsl_drop_use(ctx, tcc_ir_op_get_cond(ir, q), i);
  REWRITE(.new_op = TCCIR_OP_ASSIGN, .src1 = src1);
}

/* Both operands immediate: full constant fold.  An IMM32 operand of a 64-bit
 * op is a sign-extended 64-bit constant; 32-bit evaluation would lose the high
 * word (fuzz longlong seed 3161: `#imm SHR #32`). */
OPT_GEN_SSA(fold_const_eval, -1) {
  int32_t val1 = 0, val2 = 0;
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(fold_read_imm32(ir, src1, &val1));
    and(fold_read_imm32(ir, src2, &val2)));
  int is_64 = irop_is_64bit(dest);
  if (!is_64 && (irop_is_64bit(src1) || irop_is_64bit(src2)))
    return 0;
  int64_t result;
  if (is_64) {
    int64_t v1 = (int64_t)val1;
    int64_t v2 = (int64_t)val2;
    switch (q->op) {
    case TCCIR_OP_ADD: result = (int64_t)((uint64_t)v1 + (uint64_t)v2); break;
    case TCCIR_OP_SUB: result = (int64_t)((uint64_t)v1 - (uint64_t)v2); break;
    case TCCIR_OP_MUL: result = (int64_t)((uint64_t)v1 * (uint64_t)v2); break;
    case TCCIR_OP_AND: result = v1 & v2; break;
    case TCCIR_OP_OR:  result = v1 | v2; break;
    case TCCIR_OP_XOR: result = v1 ^ v2; break;
    case TCCIR_OP_SHL:
      if ((uint64_t)v2 >= 64) result = 0;
      else result = (int64_t)((uint64_t)v1 << v2);
      break;
    case TCCIR_OP_SHR:
      if ((uint64_t)v2 >= 64) result = 0;
      else result = (int64_t)((uint64_t)v1 >> v2);
      break;
    case TCCIR_OP_SAR:
      if ((uint64_t)v2 >= 64) result = v1 >> 63;
      else result = v1 >> v2;
      break;
    case TCCIR_OP_DIV:
      if (v2 == 0) return 0;
      if (v2 == -1 && v1 == INT64_MIN) return 0;
      result = v1 / v2;
      break;
    case TCCIR_OP_UDIV:
      if (v2 == 0) return 0;
      result = (int64_t)((uint64_t)v1 / (uint64_t)v2);
      break;
    case TCCIR_OP_IMOD:
      if (v2 == 0) return 0;
      if (v2 == -1 && v1 == INT64_MIN) return 0;
      result = v1 % v2;
      break;
    case TCCIR_OP_UMOD:
      if (v2 == 0) return 0;
      result = (int64_t)((uint64_t)v1 % (uint64_t)v2);
      break;
    default:
      /* ROR has no 64-bit form */
      return 0;
    }
  } else {
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
  }
  IROperand imm = (is_64 && result != (int64_t)(int32_t)result)
                      ? irop_make_i64(0, tcc_ir_pool_add_i64(ir, result), dest.btype)
                      : mk_imm_bt((int32_t)result, dest.btype);
  opt_dsl_drop_use(ctx, src1, i);
  opt_dsl_drop_use(ctx, src2, i);
  REWRITE(.new_op = TCCIR_OP_ASSIGN, .src1 = imm);
}

/* x - x, x ^ x, x % x -> 0;  x / x -> 1;  x & x, x | x -> x.  x/x and x%x
 * assume x != 0 (division by zero is UB, so the zero case never reaches here);
 * src1 == src2 rules out the INT_MIN/-1 overflow trap. */
OPT_GEN_SSA(fold_x_op_x, -1) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(src1.tag == IROP_TAG_VREG && src2.tag == IROP_TAG_VREG);
    and(!src1.is_lval && !src2.is_lval);
    and(irop_get_vreg(src1) >= 0 && irop_get_vreg(src1) == irop_get_vreg(src2)));
  switch (q->op) {
  case TCCIR_OP_SUB:
  case TCCIR_OP_XOR:
  case TCCIR_OP_IMOD:
  case TCCIR_OP_UMOD:
    opt_dsl_drop_use(ctx, src1, i);
    opt_dsl_drop_use(ctx, src1, i);
    REWRITE(.new_op = TCCIR_OP_ASSIGN,
            .src1 = mk_imm_bt(0, dest.btype));
  case TCCIR_OP_DIV:
  case TCCIR_OP_UDIV:
    opt_dsl_drop_use(ctx, src1, i);
    opt_dsl_drop_use(ctx, src1, i);
    REWRITE(.new_op = TCCIR_OP_ASSIGN,
            .src1 = mk_imm_bt(1, dest.btype));
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
    opt_dsl_drop_use(ctx, src1, i);
    REWRITE(.new_op = TCCIR_OP_ASSIGN);
  default:
    return 0;
  }
}

/* *g / *g -> 1, *g % *g -> 0 for the same non-volatile, non-FP global symref
 * read (matching sym + addend) on both sides.  Both operands are read by this
 * one quad, so the value is identical; x/x and x%x assume x != 0 (UB otherwise).
 * The register form is handled by fold_x_op_x; this covers the direct symref-lval
 * operands (`a / a` on a global) that never get a vreg. */
OPT_GEN_SSA(fold_divmod_self_symref, -1) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(q->op == TCCIR_OP_DIV || q->op == TCCIR_OP_UDIV ||
         q->op == TCCIR_OP_IMOD || q->op == TCCIR_OP_UMOD);
    and(src1.is_sym && src1.is_lval && src2.is_sym && src2.is_lval));
  IRPoolSymref *a_ref = irop_get_symref_ex(ir, src1);
  IRPoolSymref *b_ref = irop_get_symref_ex(ir, src2);
  if (!a_ref || !b_ref || a_ref->sym != b_ref->sym ||
      a_ref->addend != b_ref->addend)
    return 0;
  int ttype = a_ref->sym->type.t;
  int btype = ttype & VT_BTYPE;
  if ((ttype & VT_VOLATILE) ||
      btype == VT_FLOAT || btype == VT_DOUBLE || btype == VT_LDOUBLE)
    return 0;
  int fold_val = (q->op == TCCIR_OP_IMOD || q->op == TCCIR_OP_UMOD) ? 0 : 1;
  tcc_ir_set_src2(ir, i, IROP_NONE);
  REWRITE(.new_op = TCCIR_OP_ASSIGN,
          .src1 = mk_imm_bt(fold_val, dest.btype));
}

/* One XOR arm is #-1, the other is `a` itself (common after LOAD-CSE folds
 * duplicate reads of the same address — pr60502.c). */
static int fold_bitcomp_inner_matches(IROperand xs1, IROperand xs2, IROperand a)
{
  int s1_neg1 = is_imm32(xs1) && xs1.u.imm32 == -1;
  int s2_neg1 = is_imm32(xs2) && xs2.u.imm32 == -1;
  if (!s1_neg1 && !s2_neg1)
    return 0;
  IROperand x_inner = s1_neg1 ? xs2 : xs1;
  return !x_inner.is_lval && x_inner.tag == IROP_TAG_VREG &&
         irop_get_vreg(x_inner) == irop_get_vreg(a);
}

/* a | (a ^ -1) = -1;  a & (a ^ -1) = 0.  Same-block defs only — keeps the
 * fold safe under control-flow joins (mirrors try_resolve_const_vreg). */
OPT_GEN_SSA(fold_bitcomp_src2, -1) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  PAIR(.link = IR_PAIR_DEF_OF_SRC2, .op = TCCIR_OP_XOR);
  GUARD(
    when(q->op == TCCIR_OP_OR || q->op == TCCIR_OP_AND);
    and(src1.tag == IROP_TAG_VREG && !src1.is_lval && irop_get_vreg(src1) >= 0);
    and(pvi->def_count == 1);
    and(ctx->cfg && ctx->cfg->instr_to_block[pidx] == ctx->cfg->instr_to_block[i]);
    and(fold_bitcomp_inner_matches(psrc1, psrc2, src1)));
  opt_dsl_drop_use(ctx, src1, i);
  opt_dsl_drop_use(ctx, src2, i);
  REWRITE(.new_op = TCCIR_OP_ASSIGN,
          .src1 = mk_imm_bt((q->op == TCCIR_OP_OR) ? -1 : 0, dest.btype));
}

OPT_GEN_SSA(fold_bitcomp_src1, -1) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  PAIR(.link = IR_PAIR_DEF_OF_SRC1, .op = TCCIR_OP_XOR);
  GUARD(
    when(q->op == TCCIR_OP_OR || q->op == TCCIR_OP_AND);
    and(src2.tag == IROP_TAG_VREG && !src2.is_lval && irop_get_vreg(src2) >= 0);
    and(pvi->def_count == 1);
    and(ctx->cfg && ctx->cfg->instr_to_block[pidx] == ctx->cfg->instr_to_block[i]);
    and(fold_bitcomp_inner_matches(psrc1, psrc2, src2)));
  opt_dsl_drop_use(ctx, src2, i);
  opt_dsl_drop_use(ctx, src1, i);
  REWRITE(.new_op = TCCIR_OP_ASSIGN,
          .src1 = mk_imm_bt((q->op == TCCIR_OP_OR) ? -1 : 0, dest.btype));
}

/* Non-lval single-def TEMP vreg — value-stable at any dominated use site.
 * Multi-def TEMPs exist (the ?: merge-temp class), so def_count matters. */
static int fold_single_def_temp(IRSSAOptCtx *ctx, IROperand op)
{
  if (op.tag != IROP_TAG_VREG || op.is_lval || !is_temp_vreg(op))
    return 0;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(op));
  return vi && vi->def_count == 1;
}

/* Same runtime value at both sites: matching single-def TEMP vregs or equal
 * inline immediates (multi-def vregs can be redefined in between). */
static int fold_same_value(IRSSAOptCtx *ctx, IROperand a, IROperand b)
{
  if (a.is_lval || b.is_lval)
    return 0;
  if (a.tag == IROP_TAG_VREG && b.tag == IROP_TAG_VREG)
    return irop_get_vreg(a) == irop_get_vreg(b) && fold_single_def_temp(ctx, a);
  if (a.tag == IROP_TAG_IMM32 && b.tag == IROP_TAG_IMM32)
    return a.u.imm32 == b.u.imm32;
  return 0;
}

/* (x ^ y) ^ y -> x, either arm order (swap/checksum cancellation; y = -1
 * covers double complement).  The kept operand must be value-stable at this
 * site (imm or TEMP) and widths must match end-to-end. */
OPT_GEN_SSA(fold_xor_cancel_src1, TCCIR_OP_XOR) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  PAIR(.link = IR_PAIR_DEF_OF_SRC1, .op = TCCIR_OP_XOR);
  GUARD(
    when(q->op == TCCIR_OP_XOR);
    and(pvi->def_count == 1);
    and_not(tcc_ir_barrel_shift_at(ir, &ir->compact_instructions[pidx]));
    and(irop_get_btype(pdest) == irop_get_btype(dest)));
  IROperand kept;
  if (fold_same_value(ctx, psrc1, src2))
    kept = psrc2;
  else if (fold_same_value(ctx, psrc2, src2))
    kept = psrc1;
  else
    return 0;
  GUARD(
    when(is_imm32(kept) || fold_single_def_temp(ctx, kept));
    and(irop_get_btype(dest) == irop_get_btype(kept)));
  opt_dsl_drop_use(ctx, src2, i);
  RETIRE_PAIR(kept, 0);
  REWRITE(.new_op = TCCIR_OP_ASSIGN, .src1 = kept);
}

OPT_GEN_SSA(fold_xor_cancel_src2, TCCIR_OP_XOR) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  PAIR(.link = IR_PAIR_DEF_OF_SRC2, .op = TCCIR_OP_XOR);
  GUARD(
    when(q->op == TCCIR_OP_XOR);
    and(pvi->def_count == 1);
    and_not(tcc_ir_barrel_shift_at(ir, &ir->compact_instructions[pidx]));
    and(irop_get_btype(pdest) == irop_get_btype(dest)));
  IROperand kept;
  if (fold_same_value(ctx, psrc1, src1))
    kept = psrc2;
  else if (fold_same_value(ctx, psrc2, src1))
    kept = psrc1;
  else
    return 0;
  GUARD(
    when(is_imm32(kept) || fold_single_def_temp(ctx, kept));
    and(irop_get_btype(dest) == irop_get_btype(kept)));
  opt_dsl_drop_use(ctx, src1, i);
  RETIRE_PAIR(kept, 0);
  REWRITE(.new_op = TCCIR_OP_ASSIGN, .src1 = kept);
}

/* Absorption: x & (x | y) -> x;  x | (x & y) -> x (either arm order).
 * The shared x must be value-stable across both sites (fold_same_value). */
OPT_GEN_SSA(fold_absorb_src2, -1) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  PAIR(.link = IR_PAIR_DEF_OF_SRC2, .op = -1);
  GUARD(
    when((q->op == TCCIR_OP_AND && pop == TCCIR_OP_OR) ||
         (q->op == TCCIR_OP_OR && pop == TCCIR_OP_AND));
    and(pvi->def_count == 1);
    and_not(tcc_ir_barrel_shift_at(ir, &ir->compact_instructions[pidx]));
    and(irop_get_btype(pdest) == irop_get_btype(dest));
    and(fold_same_value(ctx, psrc1, src1) || fold_same_value(ctx, psrc2, src1)));
  opt_dsl_drop_use(ctx, src2, i);
  REWRITE(.new_op = TCCIR_OP_ASSIGN);
}

OPT_GEN_SSA(fold_absorb_src1, -1) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  PAIR(.link = IR_PAIR_DEF_OF_SRC1, .op = -1);
  GUARD(
    when((q->op == TCCIR_OP_AND && pop == TCCIR_OP_OR) ||
         (q->op == TCCIR_OP_OR && pop == TCCIR_OP_AND));
    and(pvi->def_count == 1);
    and_not(tcc_ir_barrel_shift_at(ir, &ir->compact_instructions[pidx]));
    and(irop_get_btype(pdest) == irop_get_btype(dest));
    and(fold_same_value(ctx, psrc1, src2) || fold_same_value(ctx, psrc2, src2)));
  opt_dsl_drop_use(ctx, src1, i);
  REWRITE(.new_op = TCCIR_OP_ASSIGN, .src1 = src2);
}

/* Same-direction shift chains: (x shl a) shl b -> x shl (a+b) or 0 when
 * a+b >= 32; shr likewise; (x sar a) sar b -> x sar min(a+b, 31). */
OPT_GEN_SSA(fold_shift_chain, -1) {
  PATTERN(.constraints = { .src2 = IR_CONSTRAINT_IMM });
  PAIR(.link = IR_PAIR_DEF_OF_SRC1, .op = -1);
  GUARD(
    when(q->op == TCCIR_OP_SHL || q->op == TCCIR_OP_SHR || q->op == TCCIR_OP_SAR);
    and(pop == q->op);
    and(pvi->def_count == 1);
    and_not(tcc_ir_barrel_shift_at(ir, &ir->compact_instructions[pidx]));
    and_not(is_i64f64(dest) || is_i64f64(src1) || is_i64f64(pdest) || is_i64f64(psrc1));
    and(is_imm32(src2) && is_imm32(psrc2));
    and(imm(src2) >= 0 && imm(src2) < 32);
    and(imm(psrc2) >= 0 && imm(psrc2) < 32);
    and(is_imm32(psrc1) || fold_single_def_temp(ctx, psrc1)));
  int total = (int)imm(src2) + (int)imm(psrc2);
  if (q->op == TCCIR_OP_SAR && total > 31)
    total = 31;
  if (total >= 32) {
    opt_dsl_drop_use(ctx, src1, i);
    REWRITE(.new_op = TCCIR_OP_ASSIGN, .src1 = mk_imm_bt(0, dest.btype));
  }
  RETIRE_PAIR(psrc1, 0);
  REWRITE(.src1 = psrc1, .src2 = mk_imm(total));
}

/* x+0, x-0, x|0, x^0, x<<0, x>>0, x*1, x&~0, x/1 -> x;
 * x*0, x&0, x%1, x%-1 -> 0;  x|-1 -> -1;  x/-1 -> 0-x (32-bit).
 * Both-imm belongs to fold_const_eval exclusively — its bail (e.g. mixed
 * 64-bit width) must not fall into the identity folds. */
OPT_GEN_SSA(fold_identity_src2, -1) {
  int32_t v1 = 0, v2 = 0;
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(fold_read_imm32(ir, src2, &v2));
    and(!src1.is_lval);
    and_not(fold_read_imm32(ir, src1, &v1)));
  int is_identity = 0;
  int is_absorb = 0;
  int32_t absorb_val = 0;
  switch (q->op) {
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  case TCCIR_OP_ROR: is_identity = (v2 == 0); break;
  case TCCIR_OP_OR:  is_identity = (v2 == 0); is_absorb = (v2 == -1); absorb_val = -1; break;
  case TCCIR_OP_MUL: is_identity = (v2 == 1); is_absorb = (v2 == 0); break;
  case TCCIR_OP_AND: is_identity = (v2 == -1); is_absorb = (v2 == 0); break;
  case TCCIR_OP_DIV:
  case TCCIR_OP_UDIV: is_identity = (v2 == 1); break;
  case TCCIR_OP_IMOD: is_absorb = (v2 == 1 || v2 == -1); break;
  case TCCIR_OP_UMOD: is_absorb = (v2 == 1); break;
  default: break;
  }
  if (is_identity)
    REWRITE(.new_op = TCCIR_OP_ASSIGN);
  if (is_absorb) {
    opt_dsl_drop_use(ctx, src1, i);
    REWRITE(.new_op = TCCIR_OP_ASSIGN,
            .src1 = mk_imm_bt(absorb_val, dest.btype));
  }
  /* x/-1: RSB #0 beats MVN+SDIV; on ARM both wrap INT_MIN/-1 to INT_MIN. */
  if (q->op == TCCIR_OP_DIV && v2 == -1 &&
      !irop_is_64bit(dest) && !irop_is_64bit(src1))
    REWRITE(.new_op = TCCIR_OP_SUB,
            .src1 = mk_imm_bt(0, irop_get_btype(src1)),
            .src2 = src1);
  return 0;
}

/* `#0 SUB (#0 SUB z)` -> ASSIGN z; same width only — a differing dest btype
 * means the inner SUB enforces an implicit narrowing the fold would skip.
 * Iterated with cprop+GVN this collapses goto-chain `i = -i` idioms (961126-1.c). */
OPT_GEN_SSA(fold_double_neg, TCCIR_OP_SUB) {
  int32_t v1 = 0;
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  PAIR(.link = IR_PAIR_DEF_OF_SRC2, .op = TCCIR_OP_SUB);
  GUARD(
    when(q->op == TCCIR_OP_SUB);
    and(fold_read_imm32(ir, src1, &v1) && v1 == 0);
    and(pvi->def_count == 1);
    and(is_imm32(psrc1) && psrc1.u.imm32 == 0);
    and(psrc2.tag == IROP_TAG_VREG && !psrc2.is_lval);
    and(irop_get_btype(dest) == irop_get_btype(psrc2)));
  IROperand new_src = psrc2;
  new_src.is_lval = 0;
  RETIRE_PAIR(new_src, 0);
  REWRITE(.new_op = TCCIR_OP_ASSIGN, .src1 = new_src);
}

/* 0+x, 0|x, 0^x, 1*x, ~0&x -> x;  0*x, 0&x, 0<<x, 0>>x, 0 ror x -> 0;
 * -1|x -> -1 */
OPT_GEN_SSA(fold_identity_src1, -1) {
  int32_t v1 = 0;
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(fold_read_imm32(ir, src1, &v1));
    and(!src2.is_lval && src2.tag == IROP_TAG_VREG));
  int is_identity = 0;
  int is_absorb = 0;
  int32_t absorb_val = 0;
  switch (q->op) {
  case TCCIR_OP_ADD:
  case TCCIR_OP_XOR: is_identity = (v1 == 0); break;
  case TCCIR_OP_OR:  is_identity = (v1 == 0); is_absorb = (v1 == -1); absorb_val = -1; break;
  case TCCIR_OP_MUL: is_identity = (v1 == 1); is_absorb = (v1 == 0); break;
  case TCCIR_OP_AND: is_identity = (v1 == -1); is_absorb = (v1 == 0); break;
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  case TCCIR_OP_ROR: is_absorb = (v1 == 0); break;
  default: break;
  }
  if (is_identity)
    REWRITE(.new_op = TCCIR_OP_ASSIGN, .src1 = src2);
  if (is_absorb) {
    opt_dsl_drop_use(ctx, src2, i);
    REWRITE(.new_op = TCCIR_OP_ASSIGN,
            .src1 = mk_imm_bt(absorb_val, dest.btype));
  }
  return 0;
}

/* Known-zero folds for `d = s AND #M`: s provably clear of M -> #0; drop an OR
 * arm masked to nothing; `(x AND K) AND M` -> copy (K subset) or `x AND (K&M)`.
 * Kills the bitfield insert+re-extract left by the retme round-trip (20040709-2). */
OPT_GEN_SSA(fold_and_masked, TCCIR_OP_AND) {
  int32_t M = 0;
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(is_value_dest(dest));
    and_not(irop_is_64bit(dest) || irop_is_64bit(src1) || irop_is_64bit(src2));
    and_not(tcc_ir_barrel_shift_at(ir, q));
    and(fold_read_imm32(ir, src2, &M)));
  if (src1.tag == IROP_TAG_VREG && !src1.is_lval) {
    int budget = 4096;
    if ((fold_maybe_set_bits(ctx, src1, i, 24, &budget) & (uint32_t)M) == 0) {
      opt_dsl_drop_use(ctx, src1, i);
      REWRITE(.new_op = TCCIR_OP_ASSIGN,
              .src1 = mk_imm_bt(0, irop_get_btype(dest)));
    }
  }
  IRQuadCompact *dq = fold_single_dom_def(ctx, src1, i);
  if (!dq || tcc_ir_barrel_shift_at(ir, dq))
    return 0;
  if (dq->op == TCCIR_OP_AND) {
    int32_t K;
    if (irop_is_64bit(tcc_ir_op_get_src1(ir, dq)) ||
        !fold_read_imm32(ir, tcc_ir_op_get_src2(ir, dq), &K))
      return 0;
    if ((K & ~M) == 0)
      REWRITE(.new_op = TCCIR_OP_ASSIGN);
    /* X moves from dq's site here: imm / dominating single-def TEMP is stable,
     * an lval read only when no store/call/control transfer lies between. */
    IROperand X = tcc_ir_op_get_src1(ir, dq);
    int d_idx = (int)(dq - ir->compact_instructions);
    int32_t xtmp;
    if (X.is_lval) {
      if (!ir_xform_range_preserves_memory(ir, d_idx, i))
        return 0;
    } else if (!fold_read_imm32(ir, X, &xtmp) && !fold_single_dom_def(ctx, X, i)) {
      return 0;
    }
    opt_dsl_drop_use(ctx, src1, i);
    opt_dsl_add_use(ctx, X, i);
    REWRITE(.src1 = X,
            .src2 = mk_imm_bt(K & M, irop_get_btype(src2)));
  }
  if (dq->op != TCCIR_OP_OR)
    return 0;
  IROperand arms[2];
  arms[0] = tcc_ir_op_get_src1(ir, dq);
  arms[1] = tcc_ir_op_get_src2(ir, dq);
  if (irop_is_64bit(arms[0]) || irop_is_64bit(arms[1]))
    return 0;
  for (int a = 0; a < 2; a++) {
    IROperand arm = arms[a], other = arms[1 - a];
    int32_t K;
    int arm_dead = 0;
    if (fold_read_imm32(ir, arm, &K)) {
      arm_dead = ((K & M) == 0);
    } else {
      IRQuadCompact *aq = fold_single_dom_def(ctx, arm, i);
      if (aq && !tcc_ir_barrel_shift_at(ir, aq) && aq->op == TCCIR_OP_AND &&
          !irop_is_64bit(tcc_ir_op_get_src1(ir, aq)) &&
          fold_read_imm32(ir, tcc_ir_op_get_src2(ir, aq), &K))
        arm_dead = ((K & M) == 0);
    }
    if (!arm_dead || other.is_lval)
      continue;
    /* `other` must be value-stable from the OR's site to here: an immediate
     * or a single-def TEMP whose def dominates this use. */
    int32_t ov;
    if (!fold_read_imm32(ir, other, &ov) && !fold_single_dom_def(ctx, other, i))
      continue;
    opt_dsl_drop_use(ctx, src1, i);
    opt_dsl_add_use(ctx, other, i);
    REWRITE(.src1 = other);
  }
  return 0;
}

/* `d = s1 OR s2` with one arm provably all-zero (barrel-shift-aware) collapses
 * to a copy — kills the bitfield-insert OR whose inserted field folded to #0. */
OPT_GEN_SSA(fold_or_zero_arm, TCCIR_OP_OR) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(is_value_dest(dest) && !irop_is_64bit(dest));
    and_not(irop_is_64bit(src1) || irop_is_64bit(src2)));
  uint8_t bs = tcc_ir_barrel_shift_at(ir, q);
  int budget = 4096;
  uint32_t m2 = fold_apply_barrel(fold_maybe_set_bits(ctx, src2, i, 24, &budget), bs);
  int keep_src1;
  if (m2 == 0) {
    keep_src1 = 1;
  } else if (!bs) {
    budget = 4096;
    if (fold_maybe_set_bits(ctx, src1, i, 24, &budget) != 0)
      return 0;
    keep_src1 = 0;
  } else {
    return 0;
  }
  IROperand keep = keep_src1 ? src1 : src2;
  IROperand drop = keep_src1 ? src2 : src1;
  if (keep.is_lval)
    return 0;
  opt_dsl_drop_use(ctx, drop, i);
  fold_clear_barrel(ir, q);
  REWRITE(.new_op = TCCIR_OP_ASSIGN, .src1 = keep);
}

OPT_GEN_SSA(fold_bfi, TCCIR_OP_BFI) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(is_temp_vreg(dest));
    and_not(irop_is_64bit(dest) || irop_is_64bit(src1) || irop_is_64bit(src2)));
  uint16_t params = tcc_ir_bfi_params_at(ir, q);
  int lsb = params & 0xFF;
  int width = params >> 8;
  if (width <= 0 || lsb + width > 32)
    return 0;
  fold_materialize_consts(ctx, i);
  int32_t val1 = 0, val2 = 0;
  if (!fold_read_imm32(ir, tcc_ir_op_get_src1(ir, q), &val1) ||
      !fold_read_imm32(ir, tcc_ir_op_get_src2(ir, q), &val2))
    return 0;
  uint32_t mask = (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
  uint32_t result = ((uint32_t)val1 & ~(mask << lsb)) | (((uint32_t)val2 & mask) << lsb);
  REWRITE(.new_op = TCCIR_OP_ASSIGN,
          .src1 = mk_imm_bt((int32_t)result, irop_get_btype(dest)));
}

/* Constant U/SBFX extract: src2 encodes lsb (bits 0-4) | width<<5 (0 means 8),
 * same decode as tcc_gen_machine_ubfx_mop; SBFX sign-extends from the top
 * field bit so a cast chain over a constant (e.g. `(int8_t)0`) collapses. */
static int fold_bfx_value(IRSSAOptCtx *ctx, int i, int is_signed, int32_t *out)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  if (!is_temp_vreg(dest))
    return 0;
  if (irop_is_64bit(dest) || irop_is_64bit(src1) || !is_imm32(src2))
    return 0;
  int32_t val1 = 0;
  if (!fold_read_imm32(ir, src1, &val1)) {
    if (!try_resolve_const_vreg(ctx, src1, i, &val1))
      return 0;
    opt_dsl_drop_use(ctx, src1, i);
  }
  int lsb = src2.u.imm32 & 0x1F;
  int width = (src2.u.imm32 >> 5) & 0x1F;
  if (width == 0)
    width = 8;
  uint32_t mask = (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
  uint32_t field = ((uint32_t)val1 >> lsb) & mask;
  *out = (is_signed && width < 32 && (field & (1u << (width - 1))))
             ? (int32_t)(field | ~mask)
             : (int32_t)field;
  return 1;
}

OPT_GEN_SSA(fold_ubfx, TCCIR_OP_UBFX) {
  int32_t result = 0;
  PATTERN(.constraints = { .src2 = IR_CONSTRAINT_IMM });
  GUARD(when(fold_bfx_value(ctx, i, 0, &result)));
  REWRITE(.new_op = TCCIR_OP_ASSIGN,
          .src1 = mk_imm_bt(result, irop_get_btype(dest)));
}

OPT_GEN_SSA(fold_sbfx, TCCIR_OP_SBFX) {
  int32_t result = 0;
  PATTERN(.constraints = { .src2 = IR_CONSTRAINT_IMM });
  GUARD(when(fold_bfx_value(ctx, i, 1, &result)));
  REWRITE(.new_op = TCCIR_OP_ASSIGN,
          .src1 = mk_imm_bt(result, irop_get_btype(dest)));
}

/* Rule order matches the original monolith. */
static const OptDslSSARule fold_binary_rules[] = {
  opt_dsl_dispatch_fold_symref_addend,
  opt_dsl_dispatch_fold_div_zero_trap,
  opt_dsl_dispatch_fold_const_eval,
  opt_dsl_dispatch_fold_x_op_x,
  opt_dsl_dispatch_fold_divmod_self_symref,
  opt_dsl_dispatch_fold_bitcomp_src2,
  opt_dsl_dispatch_fold_bitcomp_src1,
  opt_dsl_dispatch_fold_xor_cancel_src1,
  opt_dsl_dispatch_fold_xor_cancel_src2,
  opt_dsl_dispatch_fold_absorb_src2,
  opt_dsl_dispatch_fold_absorb_src1,
  opt_dsl_dispatch_fold_shift_chain,
  opt_dsl_dispatch_fold_identity_src2,
  opt_dsl_dispatch_fold_double_neg,
  opt_dsl_dispatch_fold_identity_src1,
};

/* A barrel-annotated op folds only all-constant (or nothing at all). */
static int fold_binary(IRSSAOptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (!is_value_dest(tcc_ir_op_get_dest(ir, q)))
    return 0;
  if (tcc_ir_barrel_shift_at(ir, q))
    return opt_dsl_dispatch_fold_barrel(ctx, i);
  fold_materialize_consts(ctx, i);
  return opt_dsl_chain(ctx, i, fold_binary_rules,
                       OPT_DSL_TABLE_COUNT(fold_binary_rules));
}

static int fold_and_entry(IRSSAOptCtx *ctx, int idx)
{
  int c = fold_binary(ctx, idx);
  return c ? c : opt_dsl_dispatch_fold_and_masked(ctx, idx);
}

static int fold_or_entry(IRSSAOptCtx *ctx, int idx)
{
  int c = fold_binary(ctx, idx);
  return c ? c : opt_dsl_dispatch_fold_or_zero_arm(ctx, idx);
}

static const IRSSAOptGen fold_gens[] = {
  OPT_GEN_ENTRY(fold_bfi,  TCCIR_OP_BFI),
  OPT_GEN_ENTRY(fold_ubfx, TCCIR_OP_UBFX),
  OPT_GEN_ENTRY(fold_sbfx, TCCIR_OP_SBFX),
  OPT_GEN_ENTRY(fold_select_equal, TCCIR_OP_SELECT),
  { TCCIR_OP_ADD,  fold_binary,    "fold_add" },
  { TCCIR_OP_SUB,  fold_binary,    "fold_sub" },
  { TCCIR_OP_MUL,  fold_binary,    "fold_mul" },
  { TCCIR_OP_DIV,  fold_binary,    "fold_div" },
  { TCCIR_OP_UDIV, fold_binary,    "fold_udiv" },
  { TCCIR_OP_IMOD, fold_binary,    "fold_mod" },
  { TCCIR_OP_UMOD, fold_binary,    "fold_umod" },
  { TCCIR_OP_AND,  fold_and_entry, "fold_and" },
  { TCCIR_OP_OR,   fold_or_entry,  "fold_or" },
  { TCCIR_OP_XOR,  fold_binary,    "fold_xor" },
  { TCCIR_OP_SHL,  fold_binary,    "fold_shl" },
  { TCCIR_OP_SHR,  fold_binary,    "fold_shr" },
  { TCCIR_OP_SAR,  fold_binary,    "fold_sar" },
  { TCCIR_OP_ROR,  fold_binary,    "fold_ror" },
};

int ssa_opt_fold(IRSSAOptCtx *ctx)
{
  return ssa_opt_run_gens(ctx, fold_gens, OPT_DSL_TABLE_COUNT(fold_gens));
}

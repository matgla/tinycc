/*
 *  TCC SSA opt - narrowing / extension folding
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
#include "opt/ssa/narrow.h"
#include "opt/ssa/ssa_opt_helpers.h"
#include <string.h>

/* Call-param helpers from ir/opt_utils.c, declared locally: opt_utils.h's
 * is_power_of_2() prototype conflicts with the static inline in
 * ssa_opt_helpers.h, so the header cannot be included here. */
int ir_opt_get_call_param_operand(struct TCCIRState *ir, int call_idx,
                                  int param_idx, IROperand *out);
int ir_opt_get_call_param_index(struct TCCIRState *ir, int call_idx,
                                int param_idx);
void ir_opt_nop_call_params(struct TCCIRState *ir, int call_idx);
int change_callee_sym(struct TCCIRState *ir, int instr_idx, const char *new_name, int ret_btype);

static IROperand narrow_copy_src(IROperand dest, int32_t vr)
{
  IROperand s = dest;
  s.vr = vr;
  s.tag = IROP_TAG_VREG;
  s.is_lval = 0;
  s.is_local = 0;
  s.is_llocal = 0;
  s.u.imm32 = 0;
  return s;
}

/* (x SHL #n) SHR #n  ->  x AND #((1 << (32-n)) - 1)   [SHL #24/SHR #24 -> AND #0xFF].
 * The mask is 32-bit-only: at INT64 (x << 24) >> 24 masks 40 bits, not 8. */
OPT_GEN_SSA(narrow_shr, TCCIR_OP_SHR) {
  PATTERN(.constraints = { .src2 = IR_CONSTRAINT_IMM });
  PAIR(.link = IR_PAIR_DEF_OF_SRC1, .op = TCCIR_OP_SHL);
  GUARD(
    when(is_imm32(src2) && is_imm32(psrc2));
    and(imm(src2) > 0 && imm(src2) < 32);
    and(imm(psrc2) == imm(src2));
    and_not(is_i64f64(psrc1) || is_i64f64(pdest) || is_i64f64(dest) || is_i64f64(src1));
    and_not(psrc1.is_lval || psrc1.is_local || psrc1.is_llocal));

  uint32_t mask = (1u << (32 - imm(src2))) - 1;
  RETIRE_PAIR(psrc1, 0);
  REWRITE(
    .new_op = TCCIR_OP_AND,
    .src1   = psrc1,
    .src2   = irop_make_imm32(0, (int32_t)mask, dest.btype));
}

/* (x AND #a) AND #b -> x  when b keeps every a-bit;  (x SHR #n) AND #m -> x when
 * m covers all live bits, else (x SHR #n) AND #((1<<w)-1) -> UBFX(x,#n,#w). */
OPT_GEN_SSA(narrow_and, TCCIR_OP_AND) {
  PATTERN(.constraints = { .src2 = IR_CONSTRAINT_IMM });
  PAIR(.link = IR_PAIR_DEF_OF_SRC1, .op = -1);
  GUARD(when(is_imm32(src2)));

  uint32_t outer = (uint32_t)imm(src2);
  int32_t src1_vr = vreg(src1);

  /* Compose an AND-mask with a masking predecessor, where a `UBFX(v, 0, w)`
   * counts as `AND(v, (1<<w)-1)` (lsb 0 only):
   *   AND(AND(v, m1),      m2) -> AND(v, m1 & m2)
   *   AND(UBFX(v, 0, w),   m2) -> AND(v, ((1<<w)-1) & m2)
   * When the outer mask already covers the inner one (m1 ⊆ m2), the outer AND
   * is a no-op and folds to a copy of the inner result.  Otherwise, when the
   * inner def is single-use, drop it and mask v directly — this is the
   * backward-redundant "truncate to storage width, then mask to field width"
   * that bitfield reads emit (a `UBFX #0,#16` feeding `& 0x7ff`), which no
   * forward known-bits pass can see. */
  if (pop == TCCIR_OP_AND || pop == TCCIR_OP_UBFX) {
    if (!is_imm32(psrc2))
      return 0;
    /* 32-bit masks only: a 32-bit AND over a 64-bit value clears its high half. */
    if (irop_is_64bit(dest) || irop_is_64bit(src1) ||
        irop_is_64bit(pdest) || irop_is_64bit(psrc1))
      return 0;
    uint32_t inner;
    if (pop == TCCIR_OP_AND) {
      inner = (uint32_t)imm(psrc2);
    } else {
      int32_t param = (int32_t)imm(psrc2);
      int lsb = param & 31, width = (param >> 5) & 63;
      if (lsb != 0 || width <= 0 || width >= 32)
        return 0;
      inner = (1u << width) - 1;
    }
    /* outer covers inner: the outer AND changes nothing */
    if ((inner & outer) == inner)
      REWRITE(.new_op = TCCIR_OP_ASSIGN, .src1 = narrow_copy_src(dest, src1_vr));
    /* inner dies: compose the two masks and read v directly.  A contiguous
     * low-bits result becomes a UBFX (the canonical form this pass emits for
     * the SHR case, and the shape the bitfield store→load forward matches);
     * anything else stays a plain AND. */
    if (pvi->use_count == 1) {
      uint32_t m = inner & outer;
      int w = (m != 0 && (m & (m + 1)) == 0) ? __builtin_popcount(m) : 0;
      RETIRE_PAIR(psrc1, 1);
      if (w >= 1 && w < 32)
        REWRITE(
          .new_op = TCCIR_OP_UBFX,
          .src1   = psrc1,
          .src2   = irop_make_imm32(-1, 0 | (w << 5), IROP_BTYPE_INT32));
      REWRITE(
        .new_op = TCCIR_OP_AND,
        .src1   = psrc1,
        .src2   = irop_make_imm32(-1, (int32_t)m, dest.btype));
    }
    return 0;
  }

  if (pop == TCCIR_OP_SHR) {
    if (!is_imm32(psrc2))
      return 0;
    int32_t shift = (int32_t)imm(psrc2);
    if (shift <= 0 || shift >= 32)
      return 0;

    uint32_t max_bits = (1u << (32 - shift)) - 1;
    if ((outer & max_bits) == max_bits)
      REWRITE(.new_op = TCCIR_OP_ASSIGN, .src1 = narrow_copy_src(dest, src1_vr));

    int width = (outer != 0 && (outer & (outer + 1)) == 0)
                    ? __builtin_popcount(outer)
                    : 0;
    if (width >= 1 && width < 32 && shift + width <= 32 && pvi->use_count == 1) {
      RETIRE_PAIR(psrc1, 1);
      REWRITE(
        .new_op = TCCIR_OP_UBFX,
        .src1   = psrc1,
        .src2   = irop_make_imm32(-1, shift | (width << 5), IROP_BTYPE_INT32));
    }
  }

  return 0;
}

/* (x SHR #n) fed into UBFX(#lsb,#w) -> UBFX(x, #(lsb+n), #w) when the SHR dies. */
OPT_GEN_SSA(narrow_ubfx, TCCIR_OP_UBFX) {
  PATTERN(.constraints = { .src2 = IR_CONSTRAINT_IMM });
  PAIR(.link = IR_PAIR_DEF_OF_SRC1, .op = TCCIR_OP_SHR, .single_use = 1);
  GUARD(when(is_imm32(src2) && is_imm32(psrc2)));

  int32_t param = (int32_t)imm(src2);
  int lsb = param & 31, width = (param >> 5) & 63, n = (int)imm(psrc2);
  GUARD(
    when(width > 0);
    and(n > 0 && n < 32);
    and(lsb + n + width <= 32));

  RETIRE_PAIR(psrc1, 1);
  REWRITE(
    .new_op = TCCIR_OP_UBFX,
    .src1   = psrc1,
    .src2   = irop_make_imm32(-1, (lsb + n) | (width << 5), IROP_BTYPE_INT32));
}

static const IRSSAOptGen narrow_gens[] = {
  OPT_GEN_ENTRY(narrow_shr,  TCCIR_OP_SHR),
  OPT_GEN_ENTRY(narrow_and,  TCCIR_OP_AND),
  OPT_GEN_ENTRY(narrow_ubfx, TCCIR_OP_UBFX),
};

/* ========================================================================
 * Soft-FP double->float demotion fold (SSA analog of the retired flat
 * float_narrow pass): f2d(f) -> floor(double) [-> d2f] narrows to floorf(f).
 * ======================================================================== */

typedef struct
{
  const char *double_name;
  const char *float_name;
} NarrowFloatEntry;

static const NarrowFloatEntry narrow_float_table[] = {
    {"floor", "floorf"}, {"ceil", "ceilf"},           {"trunc", "truncf"}, {"round", "roundf"},
    {"fabs", "fabsf"},   {"nearbyint", "nearbyintf"}, {"rint", "rintf"},
};
#define NARROW_FLOAT_TABLE_COUNT (sizeof(narrow_float_table) / sizeof(narrow_float_table[0]))

/* Callee name of the FUNCCALLVAL at call_idx, NULL when not a valued call or
 * the callee is not a direct symbol. */
static const char *narrow_callee_name(TCCIRState *ir, int call_idx)
{
  IRQuadCompact *q = &ir->compact_instructions[call_idx];
  if (q->op != TCCIR_OP_FUNCCALLVAL)
    return NULL;
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  return callee ? get_tok_str(callee->v, NULL) : NULL;
}

/* Find the FUNCCALLVAL/FUNCCALLVOID owning the FUNCPARAMVAL at param_idx by
 * scanning forward for the first call and matching its call_id (params sit
 * immediately before their call).  Returns -1 when not found/mismatched. */
static int narrow_param_owner_call(TCCIRState *ir, int param_idx)
{
  IRQuadCompact *pq = &ir->compact_instructions[param_idx];
  if (pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID)
    return -1;
  uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, pq));
  int call_id = TCCIR_DECODE_CALL_ID(encoded);

  for (int i = param_idx + 1; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
      continue;
    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      return -1; /* something else between param and its call: bail */
    int id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
    return (id == call_id) ? i : -1;
  }
  return -1;
}

/* Single-def TEMP vreg defined by an __aeabi_f2d call; returns the defining
 * call's instruction index or -1. */
static int narrow_f2d_def(IRSSAOptCtx *ctx, IROperand op)
{
  if (op.tag != IROP_TAG_VREG || op.is_lval)
    return -1;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(op));
  if (!vi || vi->def_count != 1 || vi->def_instr < 0 || vi->use_count != 1)
    return -1;
  const char *name = narrow_callee_name(ctx->ir, vi->def_instr);
  return (name && !strcmp(name, "__aeabi_f2d")) ? vi->def_instr : -1;
}

/* Double immediate exactly representable as a float -> build the F32 immediate.
 * Arises when f2d(c_f) is const-folded into a double immediate before the SSA
 * stage.  Three encodings:
 *   - IMM32 + FLOAT64: integer-valued double shorthand, value (double)imm32.
 *     Always sound to narrow: for an integer-valued d, d2f(floor(d)) and
 *     floorf((float)d) are both (float)d (float-rounding an integer stays an
 *     integer, so floor is the identity on both paths).
 *   - F64 pool / I64-pool + FLOAT64: raw double bits; accepted only when the
 *     double is exactly a float value (float->double widening is exact, so
 *     such constants are precisely the f2d-folded ones).  Narrowing a
 *     non-exact double ahead of the math call would change rounding
 *     (e.g. (float)0.999...9 == 1.0f while floor(0.999...9) == 0.0). */
int tcc_ir_ssa_narrow_f64_imm_exact_f32(TCCIRState *ir, IROperand op, IROperand *out)
{
  if (op.is_lval)
    return 0;

  double d;
  if (op.tag == IROP_TAG_IMM32 && op.btype == IROP_BTYPE_FLOAT64)
  {
    d = (double)(int64_t)op.u.imm32;
  }
  else if (op.tag == IROP_TAG_F64 || (op.tag == IROP_TAG_I64 && op.btype == IROP_BTYPE_FLOAT64))
  {
    uint64_t dbits;
    if (op.tag == IROP_TAG_F64)
    {
      uint64_t *p = tcc_ir_pool_get_f64_ptr(ir, op.u.pool_idx);
      if (!p)
        return 0;
      dbits = *p;
    }
    else
    {
      int64_t *p = tcc_ir_pool_get_i64_ptr(ir, op.u.pool_idx);
      if (!p)
        return 0;
      dbits = (uint64_t)*p;
    }
    memcpy(&d, &dbits, sizeof(d));
    double back = (double)(float)d;
    uint64_t back_bits;
    memcpy(&back_bits, &back, sizeof(back_bits));
    if (back_bits != dbits)
      return 0; /* not an exact float value */
  }
  else
  {
    return 0;
  }

  float f = (float)d;
  uint32_t fbits;
  memcpy(&fbits, &f, sizeof(fbits));
  *out = irop_make_f32(0, fbits);
  return 1;
}

/* The lone use of vreg `v2` is param 0 of an __aeabi_d2f call; returns that
 * call's instruction index or -1. */
static int narrow_d2f_use(IRSSAOptCtx *ctx, int32_t v2)
{
  TCCIRState *ir = ctx->ir;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, v2);
  if (!vi || vi->use_count != 1 || vi->uses[0].kind != SSA_USE_INSTR)
    return -1;
  int pi = vi->uses[0].idx;
  IRQuadCompact *pq = &ir->compact_instructions[pi];
  if (pq->op != TCCIR_OP_FUNCPARAMVAL)
    return -1;
  uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, pq));
  if (TCCIR_DECODE_PARAM_IDX(encoded) != 0)
    return -1;
  int owner = narrow_param_owner_call(ir, pi);
  if (owner < 0)
    return -1;
  const char *name = narrow_callee_name(ir, owner);
  if (!name || strcmp(name, "__aeabi_d2f"))
    return -1;
  /* The d2f must produce a TEMP we can redirect the math call to. */
  IROperand d2f_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[owner]);
  if (d2f_dest.tag != IROP_TAG_VREG || !ssa_opt_vinfo(ctx, irop_get_vreg(d2f_dest)))
    return -1;
  return owner;
}

static int narrow_float_demote(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    const char *name = narrow_callee_name(ir, i);
    if (!name)
      continue;
    const char *float_name = NULL;
    for (size_t j = 0; j < NARROW_FLOAT_TABLE_COUNT; j++)
    {
      if (!strcmp(name, narrow_float_table[j].double_name))
      {
        float_name = narrow_float_table[j].float_name;
        break;
      }
    }
    if (!float_name)
      continue;

    /* Never narrow into a self-call: inside float_name's OWN definition
     * (libm's `float ceilf(float x) { return (float)ceil((double)x); }`)
     * the rewrite creates infinite recursion, which
     * infinite_self_recursion then collapses to a `b .` self-loop. */
    if (funcname && !strcmp(funcname, float_name))
      continue;

    /* Param 0 must be a TEMP whose single def / single use is an f2d call,
     * or a double immediate that is exactly a float value (an f2d-folded
     * constant).  `orig` is the operand the float variant will take. */
    IROperand arg0;
    if (!ir_opt_get_call_param_operand(ir, i, 0, &arg0))
      continue;
    IROperand orig;
    int f2d_idx = narrow_f2d_def(ctx, arg0);
    if (f2d_idx >= 0)
    {
      int f2d_param_idx = ir_opt_get_call_param_index(ir, f2d_idx, 0);
      if (f2d_param_idx < 0)
        continue;
      orig = tcc_ir_op_get_src1(ir, &ir->compact_instructions[f2d_param_idx]);
    }
    else if (!tcc_ir_ssa_narrow_f64_imm_exact_f32(ir, arg0, &orig))
    {
      continue;
    }

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int d2f_idx = (dest.tag == IROP_TAG_VREG) ? narrow_d2f_use(ctx, irop_get_vreg(dest)) : -1;

    if (d2f_idx >= 0)
    {
      /* Case 1: f2d -> func -> d2f  ==>  func_f(orig_float) -> d2f's dest;
       * NOP the f2d (if any) and d2f conversion calls (params included).
       * Apply the only fallible step (callee rename) before any operand
       * mutation. */
      int func_param_idx = ir_opt_get_call_param_index(ir, i, 0);
      if (func_param_idx < 0)
        continue;
      if (!change_callee_sym(ir, i, float_name, VT_FLOAT))
        continue;

      tcc_ir_set_src1(ir, func_param_idx, orig);
      IROperand d2f_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[d2f_idx]);
      tcc_ir_set_dest(ir, i, d2f_dest);

      if (f2d_idx >= 0)
      {
        ir_opt_nop_call_params(ir, f2d_idx);
        ir->compact_instructions[f2d_idx].op = TCCIR_OP_NOP;
      }
      ir_opt_nop_call_params(ir, d2f_idx);
      ir->compact_instructions[d2f_idx].op = TCCIR_OP_NOP;
      changes++;
    }
    else if (f2d_idx >= 0)
    {
      /* Case 2: f2d -> func, result stays double  ==>  swap callees:
       * floorf(orig_float) -> T, then f2d(T) -> dest.  The renames are only
       * fallible on symbol-table OOM (both symbols already exist).  (The
       * immediate form has no f2d instruction to repurpose, so it is
       * Case-1-only.) */
      if (!change_callee_sym(ir, f2d_idx, float_name, VT_FLOAT))
        continue;
      change_callee_sym(ir, i, "__aeabi_f2d", VT_INT);
      changes++;
    }
  }

  return changes;
}

int ssa_opt_narrow(IRSSAOptCtx *ctx)
{
  int changes = ssa_opt_run_gens(ctx, narrow_gens, OPT_DSL_TABLE_COUNT(narrow_gens));
  int demote = narrow_float_demote(ctx);
  if (demote)
    tcc_ir_ssa_opt_rebuild(ctx);
  return changes + demote;
}

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

  if (pop == TCCIR_OP_AND) {
    if (!is_imm32(psrc2))
      return 0;
    uint32_t inner = (uint32_t)imm(psrc2);
    if ((inner & outer) != inner)
      return 0;
    REWRITE(.new_op = TCCIR_OP_ASSIGN, .src1 = narrow_copy_src(dest, src1_vr));
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

int ssa_opt_narrow(IRSSAOptCtx *ctx)
{
  return ssa_opt_run_gens(ctx, narrow_gens, OPT_DSL_TABLE_COUNT(narrow_gens));
}

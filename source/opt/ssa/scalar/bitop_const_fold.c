/*
 *  TCC SSA opt - constant-fold __builtin bit-op helper calls
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"
#include "opt_dsl.h"
#include "bitop_const_fold.h"
#include <string.h>

typedef enum { BOP_NONE, BOP_CLZ, BOP_CTZ, BOP_POPCOUNT, BOP_PARITY, BOP_FFS, BOP_CLRSB, BOP_BSWAP } BitOpKind;

typedef struct {
  BitOpKind kind;
  int       is64;
} BitOp;

/* Map a __builtin_* helper name to its op + operand width; `l` variants fold to
 * the 32-bit helper because long is 32-bit on this target. */
static BitOp bitop_classify(const char *name)
{
  if (name) {
    if (!strcmp(name, "__clzsi2"))          return (BitOp){ BOP_CLZ, 0 };
    if (!strcmp(name, "__clzdi2"))          return (BitOp){ BOP_CLZ, 1 };
    if (!strcmp(name, "__ctzsi2"))          return (BitOp){ BOP_CTZ, 0 };
    if (!strcmp(name, "__ctzdi2"))          return (BitOp){ BOP_CTZ, 1 };
    if (!strcmp(name, "__popcountsi2"))     return (BitOp){ BOP_POPCOUNT, 0 };
    if (!strcmp(name, "__popcountdi2"))     return (BitOp){ BOP_POPCOUNT, 1 };
    if (!strcmp(name, "__paritysi2"))       return (BitOp){ BOP_PARITY, 0 };
    if (!strcmp(name, "__paritydi2"))       return (BitOp){ BOP_PARITY, 1 };
    if (!strcmp(name, "ffs"))               return (BitOp){ BOP_FFS, 0 };
    if (!strcmp(name, "ffsl"))              return (BitOp){ BOP_FFS, 0 };
    if (!strcmp(name, "ffsll"))             return (BitOp){ BOP_FFS, 1 };
    if (!strcmp(name, "__builtin_clrsb"))   return (BitOp){ BOP_CLRSB, 0 };
    if (!strcmp(name, "__builtin_clrsbl"))  return (BitOp){ BOP_CLRSB, 0 };
    if (!strcmp(name, "__builtin_clrsbll")) return (BitOp){ BOP_CLRSB, 1 };
    /* bswap16/32 both lower through __bswapsi2; the 64-bit __bswapdi3 needs a
     * 64-bit immediate the int-result fold path cannot materialize, so skip it. */
    if (!strcmp(name, "__bswapsi2"))        return (BitOp){ BOP_BSWAP, 0 };
  }
  return (BitOp){ BOP_NONE, 0 };
}

/* Returns 0 (do not fold) for clz/ctz of 0 (undefined); callers guard x != 0. */
static int bitop_eval(BitOp op, uint64_t v, int *out)
{
  if (op.is64) {
    switch (op.kind) {
    case BOP_CLZ:      if (!v) return 0; *out = __builtin_clzll(v); return 1;
    case BOP_CTZ:      if (!v) return 0; *out = __builtin_ctzll(v); return 1;
    case BOP_POPCOUNT: *out = __builtin_popcountll(v); return 1;
    case BOP_PARITY:   *out = __builtin_popcountll(v) & 1; return 1;
    case BOP_FFS:      *out = v ? __builtin_ctzll(v) + 1 : 0; return 1;
    case BOP_CLRSB:    *out = __builtin_clrsbll((long long)v); return 1;
    default:           return 0;
    }
  }
  uint32_t x = (uint32_t)v;
  switch (op.kind) {
  case BOP_CLZ:      if (!x) return 0; *out = __builtin_clz(x); return 1;
  case BOP_CTZ:      if (!x) return 0; *out = __builtin_ctz(x); return 1;
  case BOP_POPCOUNT: *out = __builtin_popcount(x); return 1;
  case BOP_PARITY:   *out = __builtin_popcount(x) & 1; return 1;
  case BOP_FFS:      *out = x ? __builtin_ctz(x) + 1 : 0; return 1;
  case BOP_CLRSB:    *out = __builtin_clrsb((int)x); return 1;
  case BOP_BSWAP:    *out = (int)__builtin_bswap32(x); return 1;
  default:           return 0;
  }
}

/* Classify `name` and evaluate the op on `val`; returns 1 and writes *out when
 * foldable, 0 otherwise (unknown op, or clz/ctz of 0).  Split out so unit tests
 * can exercise the fold math directly — the SSA driver path resolves the op by
 * callee name, which the UT harness's get_tok_str stub cannot provide. */
int tcc_ir_bitop_const_eval(const char *name, int64_t val, int *out)
{
  BitOp op = bitop_classify(name);
  if (op.kind == BOP_NONE)
    return 0;
  return bitop_eval(op, (uint64_t)val, out);
}

/* Classify FUNCCALLVAL i's callee and fold its single immediate argument; fills
 * *out and returns 1 when foldable.  Pulled out so the GUARD reads as one when(). */
static int bitop_call_fold(TCCIRState *ir, int i, IROperand callee_op, int *out)
{
  Sym *callee = irop_get_sym_ex(ir, callee_op);
  if (!callee)
    return 0;
  BitOp op = bitop_classify(get_tok_str(callee->v, NULL));
  if (op.kind == BOP_NONE)
    return 0;

  IROperand arg0;
  if (!ir_opt_get_call_param_operand(ir, i, 0, &arg0))
    return 0;
  int tag = irop_get_tag(arg0);
  if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_I64)
    return 0;

  return bitop_eval(op, (uint64_t)irop_get_imm64_ex(ir, arg0), out);
}

OPT_GEN_SSA(bitop_fold, TCCIR_OP_FUNCCALLVAL) {
  int result = 0;
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(when(bitop_call_fold(ir, i, src1, &result)));

  ir_opt_nop_call_params(ir, i);
  tcc_ir_set_src2(ir, i, IROP_NONE);
  REWRITE(
    .new_op = TCCIR_OP_ASSIGN,
    .src1   = irop_make_imm32(-1, result, VT_INT));
}

static const IRSSAOptGen bitop_gens[] = {
  OPT_GEN_ENTRY(bitop_fold, TCCIR_OP_FUNCCALLVAL),
};

int tcc_ir_ssa_opt_bitop_const_fold(IRSSAOptCtx *ctx)
{
  int changes = ssa_opt_run_gens(ctx, bitop_gens,
                                 OPT_DSL_TABLE_COUNT(bitop_gens));
  if (changes)
    tcc_ir_ssa_opt_rebuild(ctx);

  return changes;
}

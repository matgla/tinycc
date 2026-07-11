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
#include "bitop_const_fold.h"
#include <string.h>

typedef enum { BOP_NONE, BOP_CLZ, BOP_CTZ, BOP_POPCOUNT, BOP_PARITY, BOP_FFS, BOP_CLRSB } BitOp;

/* Map a helper name (as emitted by the __builtin_* lowering in tccgen) to its
 * operation and operand width.  `l` variants fold into the 32-bit helpers
 * because long is 32-bit on this target. */
static BitOp bitop_classify(const char *name, int *is64)
{
  *is64 = 0;
  if (!name)
    return BOP_NONE;
  if (!strcmp(name, "__clzsi2")) return BOP_CLZ;
  if (!strcmp(name, "__clzdi2")) { *is64 = 1; return BOP_CLZ; }
  if (!strcmp(name, "__ctzsi2")) return BOP_CTZ;
  if (!strcmp(name, "__ctzdi2")) { *is64 = 1; return BOP_CTZ; }
  if (!strcmp(name, "__popcountsi2")) return BOP_POPCOUNT;
  if (!strcmp(name, "__popcountdi2")) { *is64 = 1; return BOP_POPCOUNT; }
  if (!strcmp(name, "__paritysi2")) return BOP_PARITY;
  if (!strcmp(name, "__paritydi2")) { *is64 = 1; return BOP_PARITY; }
  if (!strcmp(name, "ffs")) return BOP_FFS;
  if (!strcmp(name, "ffsl")) return BOP_FFS;
  if (!strcmp(name, "ffsll")) { *is64 = 1; return BOP_FFS; }
  /* clrsb/clrsbl link against libtcc1 (__builtin_clrsb* aliases) at runtime;
   * only the constant case is unfolded.  long is 32-bit on this target. */
  if (!strcmp(name, "__builtin_clrsb")) return BOP_CLRSB;
  if (!strcmp(name, "__builtin_clrsbl")) return BOP_CLRSB;
  if (!strcmp(name, "__builtin_clrsbll")) { *is64 = 1; return BOP_CLRSB; }
  return BOP_NONE;
}

/* Evaluate the op on constant `v`.  Returns 0 (do not fold) for clz/ctz of 0,
 * whose result is undefined (the source guards `x != 0` before the call, so
 * this path is never a real fold, just defensive). */
static int bitop_eval(BitOp op, int is64, uint64_t v, int *out)
{
  if (is64) {
    switch (op) {
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
  switch (op) {
  case BOP_CLZ:      if (!x) return 0; *out = __builtin_clz(x); return 1;
  case BOP_CTZ:      if (!x) return 0; *out = __builtin_ctz(x); return 1;
  case BOP_POPCOUNT: *out = __builtin_popcount(x); return 1;
  case BOP_PARITY:   *out = __builtin_popcount(x) & 1; return 1;
  case BOP_FFS:      *out = x ? __builtin_ctz(x) + 1 : 0; return 1;
  case BOP_CLRSB:    *out = __builtin_clrsb((int)x); return 1;
  default:           return 0;
  }
}

/* Classify `name` and evaluate the op on `val`; returns 1 and writes *out when
 * foldable, 0 otherwise (unknown op, or clz/ctz of 0).  Split out so unit tests
 * can exercise the fold math directly — the SSA driver path resolves the op by
 * callee name, which the UT harness's get_tok_str stub cannot provide. */
int tcc_ir_bitop_const_eval(const char *name, int64_t val, int *out)
{
  int is64;
  BitOp op = bitop_classify(name, &is64);
  if (op == BOP_NONE)
    return 0;
  return bitop_eval(op, is64, (uint64_t)val, out);
}

int tcc_ir_ssa_opt_bitop_const_fold(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int changes = 0;

  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;
    int is64;
    BitOp op = bitop_classify(get_tok_str(callee->v, NULL), &is64);
    if (op == BOP_NONE)
      continue;

    IROperand arg0;
    if (!ir_opt_get_call_param_operand(ir, i, 0, &arg0))
      continue;
    int tag = irop_get_tag(arg0);
    if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_I64)
      continue;

    int result;
    if (!bitop_eval(op, is64, (uint64_t)irop_get_imm64_ex(ir, arg0), &result))
      continue;

    ir_opt_nop_call_params(ir, i);
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, irop_make_imm32(-1, result, VT_INT));
    tcc_ir_set_src2(ir, i, IROP_NONE);
    changes++;
  }

  if (changes)
    tcc_ir_ssa_opt_rebuild(ctx);

  return changes;
}

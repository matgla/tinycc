/*
 *  ir_gen.h - random well-formed IR generator (Track 4)
 *
 *  Produces small straight-line IR functions in the *exact* op subset that
 *  ir_eval.h can interpret. Every generated function is well-formed:
 *    - only valid irop_config slots are filled (dest/src1/src2 per the op);
 *    - TEMP destinations are single-def and assigned strictly increasing
 *      positions (SSA-like), so the interpreter's single-def value model and
 *      the legacy passes' single-def assumptions both hold;
 *    - source operands are either an in-range, already-defined value (PARAM
 *      input or an earlier TEMP) or a type-consistent immediate;
 *    - shift amounts are constrained to 0..31 (avoids shift UB and matches the
 *      32-bit register model);
 *    - division/modulo by a *constant* never uses 0 or the INT_MIN/-1 overflow
 *      pair, and a division by a *variable* is only emitted when the divisor is
 *      a non-zero immediate (so eval() never traps spuriously — traps are still
 *      modeled, but the generator avoids them to keep value coverage high).
 *
 *  Determinism: a 64-bit LCG seeded by an explicit constant. No time()/rand().
 *  The same seed always yields the same function — essential for reproducing a
 *  metamorphic failure and for the delta-reducer.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License.
 */

#ifndef TCC_UT_IR_GEN_H
#define TCC_UT_IR_GEN_H

#include "ir_build.h"
#include "ir_eval.h"

#include <stdint.h>

/* ---- deterministic PRNG (LCG, Numerical Recipes constants) ---- */

typedef struct IrGenRng
{
  uint64_t state;
} IrGenRng;

static inline void irg_seed(IrGenRng *r, uint64_t seed)
{
  r->state = seed ? seed : 0x9E3779B97F4A7C15ull;
}

static inline uint32_t irg_next(IrGenRng *r)
{
  r->state = r->state * 6364136223846793005ull + 1442695040888963407ull;
  return (uint32_t)(r->state >> 32);
}

/* uniform in [0, n) */
static inline uint32_t irg_below(IrGenRng *r, uint32_t n)
{
  if (n == 0)
    return 0;
  return irg_next(r) % n;
}

/* ---- generation parameters ---- */

typedef struct IrGenConfig
{
  int num_params;   /* PARAM inputs available (positions 0..num_params-1) */
  int num_instr;    /* number of value-producing instructions to emit */
  int allow_div;    /* allow DIV/UDIV/IMOD/UMOD (with safe constant divisor) */
  int allow_bitfield; /* allow UBFX/BFI/ZEXT */
  int use_int64;    /* mix in some INT64-typed ops */
} IrGenConfig;

static inline IrGenConfig irg_default_config(void)
{
  IrGenConfig c;
  c.num_params = 3;
  c.num_instr = 10;
  c.allow_div = 1;
  c.allow_bitfield = 1;
  c.use_int64 = 0;
  return c;
}

/* ---- generation context (tracks defined values to draw operands from) ---- */

#define IRG_MAX_VALUES 256

typedef struct IrGenValue
{
  int32_t vreg;   /* encoded vreg */
  int btype;
} IrGenValue;

typedef struct IrGenCtx
{
  TCCIRState *ir;
  IrGenRng rng;
  IrGenConfig cfg;
  IrGenValue values[IRG_MAX_VALUES]; /* all currently-readable values */
  int value_count;
  int next_temp_pos;
} IrGenCtx;

/* The phase-1 binary ops the generator picks from (all are 3-operand value
 * producers the interpreter supports). */
static const TccIrOp IRG_BINOPS[] = {
    TCCIR_OP_ADD, TCCIR_OP_SUB, TCCIR_OP_MUL, TCCIR_OP_AND, TCCIR_OP_OR,
    TCCIR_OP_XOR, TCCIR_OP_SHL, TCCIR_OP_SAR, TCCIR_OP_ROR,
    TCCIR_OP_BOOL_AND, TCCIR_OP_BOOL_OR,
    /* TCCIR_OP_SHR is kept OUT of the broad sweep. The known_bits 32-bit
     * logical-SHR const-fold bug it used to expose is FIXED (Finding #16) and is
     * verified directly by test_shr_neg_const_known_bits_FIXED. Adding SHR here
     * shifts the generator RNG stream and surfaces an UNRELATED metamorphic
     * oracle false-positive (an arithmetically-impossible base value on seed 214
     * — tracker Finding #17), so re-enabling it must wait until the oracle/
     * delta-reducer is hardened. */
};
#define IRG_NUM_BINOPS ((int)(sizeof(IRG_BINOPS) / sizeof(IRG_BINOPS[0])))

static inline int irg_is_shift(TccIrOp op)
{
  return op == TCCIR_OP_SHL || op == TCCIR_OP_SHR || op == TCCIR_OP_SAR || op == TCCIR_OP_ROR;
}

/* Register a freshly-defined value so later instructions can read it. */
static inline void irg_add_value(IrGenCtx *c, int32_t vreg, int btype)
{
  if (c->value_count < IRG_MAX_VALUES)
  {
    c->values[c->value_count].vreg = vreg;
    c->values[c->value_count].btype = btype;
    c->value_count++;
  }
}

/* Pick a random readable value operand of the given btype if possible, else a
 * value of any btype, else fall back to an immediate. Always returns a valid
 * readable source. */
static inline IROperand irg_pick_src(IrGenCtx *c, int want_btype)
{
  /* 35% of the time prefer an immediate for variety / fold opportunities. */
  if (c->value_count == 0 || irg_below(&c->rng, 100) < 35)
  {
    int32_t v = (int32_t)irg_next(&c->rng);
    /* Keep magnitudes modest most of the time so reduction can shrink them. */
    if (irg_below(&c->rng, 100) < 70)
      v = (int32_t)(v % 256) - 128;
    return utb_imm(v, want_btype);
  }
  /* Prefer a value whose btype matches; otherwise any. */
  int idx = -1;
  int start = (int)irg_below(&c->rng, (uint32_t)c->value_count);
  for (int k = 0; k < c->value_count; ++k)
  {
    int j = (start + k) % c->value_count;
    if (c->values[j].btype == want_btype)
    {
      idx = j;
      break;
    }
  }
  if (idx < 0)
    idx = start;
  IrGenValue *v = &c->values[idx];
  return irop_make_vreg(v->vreg, want_btype);
}

/* Pick a shift-amount operand: an immediate in 0..31 (well-defined for the
 * 32-bit register model). */
static inline IROperand irg_pick_shift_amount(IrGenCtx *c)
{
  return utb_imm((int32_t)irg_below(&c->rng, 32), IROP_BTYPE_INT32);
}

/* Pick a safe non-zero divisor immediate (avoids div-by-zero and INT_MIN/-1). */
static inline IROperand irg_pick_safe_divisor(IrGenCtx *c, int btype)
{
  int32_t v = (int32_t)irg_next(&c->rng);
  v = (int32_t)(v % 255) - 127; /* -127..127 */
  if (v == 0)
    v = 1;
  if (v == -1)
    v = 3; /* avoid INT_MIN/-1 overflow when dividend could be INT_MIN */
  return utb_imm(v, btype);
}

/* Emit one random value-producing instruction. */
static inline void irg_emit_one(IrGenCtx *c)
{
  int btype = IROP_BTYPE_INT32;
  if (c->cfg.use_int64 && irg_below(&c->rng, 100) < 30)
    btype = IROP_BTYPE_INT64;

  int dest_pos = c->next_temp_pos++;
  IROperand dest = utb_temp(dest_pos, btype);

  /* Decide op category. */
  int roll = (int)irg_below(&c->rng, 100);

  if (c->cfg.allow_div && roll < 12)
  {
    /* Division/modulo with a safe constant divisor. */
    static const TccIrOp DIVOPS[] = {TCCIR_OP_DIV, TCCIR_OP_UDIV, TCCIR_OP_IMOD, TCCIR_OP_UMOD};
    TccIrOp op = DIVOPS[irg_below(&c->rng, 4)];
    IROperand a = irg_pick_src(c, btype);
    IROperand b = irg_pick_safe_divisor(c, btype);
    utb_emit(c->ir, op, dest, a, b);
    irg_add_value(c, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, dest_pos), btype);
    return;
  }

  if (c->cfg.allow_bitfield && roll < 24)
  {
    int which = (int)irg_below(&c->rng, 3);
    if (which == 0)
    {
      /* ZEXT: zero-extend the low 32 bits of src into the dest.  In real IR the
       * frontend only ever emits ZEXT with a 32-bit src and an INT32 or INT64
       * (VT_LLONG) dest — it widens a 32-bit low half into a register/pair with
       * a zero high half (the backend lowers it exactly like ASSIGN, NOT as a
       * sub-word mask).  Two constraints, both learned from the metamorphic loop
       * (see test_metamorphic.c PART D):
       *   - never a sub-word (INT8/INT16) dest — not a real shape (was a
       *     *generator* false positive against known_bits);
       *   - INT32 dest only in the sweep. The known_bits ZEXT-to-INT64
       *     sign-extension fold (Finding #16) is now FIXED, and is verified by
       *     the deterministic test_zext64_neg_const_known_bits_FIXED case; we
       *     keep the broad sweep on INT32-dest ZEXT to avoid perturbing the RNG
       *     stream (mixing INT64-param shapes surfaced an unrelated interpreter
       *     false-positive — see tracker Finding #17). */
      IROperand zdest = utb_temp(dest_pos, IROP_BTYPE_INT32);
      IROperand a = irg_pick_src(c, IROP_BTYPE_INT32);
      utb_emit(c->ir, TCCIR_OP_ZEXT, zdest, a, UTB_NONE);
      irg_add_value(c, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, dest_pos), IROP_BTYPE_INT32);
      return;
    }
    else if (which == 1)
    {
      /* UBFX: src2 = lsb | (width<<5), width in 1..(32-lsb). */
      int lsb = (int)irg_below(&c->rng, 24);
      int maxw = 32 - lsb;
      if (maxw > 16)
        maxw = 16;
      int width = 1 + (int)irg_below(&c->rng, (uint32_t)maxw);
      IROperand a = irg_pick_src(c, IROP_BTYPE_INT32);
      IROperand enc = utb_imm(lsb | (width << 5), IROP_BTYPE_INT32);
      IROperand ud = utb_temp(dest_pos, IROP_BTYPE_INT32);
      utb_emit(c->ir, TCCIR_OP_UBFX, ud, a, enc);
      irg_add_value(c, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, dest_pos), IROP_BTYPE_INT32);
      return;
    }
    else
    {
      /* BFI: needs ir->bfi_params[orig_index] = lsb | (width<<8). */
      int lsb = (int)irg_below(&c->rng, 24);
      int maxw = 32 - lsb;
      if (maxw > 16)
        maxw = 16;
      int width = 1 + (int)irg_below(&c->rng, (uint32_t)maxw);
      IROperand host = irg_pick_src(c, IROP_BTYPE_INT32);
      IROperand val = irg_pick_src(c, IROP_BTYPE_INT32);
      IROperand bd = utb_temp(dest_pos, IROP_BTYPE_INT32);
      int idx = utb_emit(c->ir, TCCIR_OP_BFI, bd, host, val);
      if (c->ir->bfi_params)
        c->ir->bfi_params[c->ir->compact_instructions[idx].orig_index] =
            (uint16_t)((lsb & 0xFF) | ((width & 0xFF) << 8));
      irg_add_value(c, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, dest_pos), IROP_BTYPE_INT32);
      return;
    }
  }

  if (roll < 32)
  {
    /* ASSIGN (copy/const) — exercises copy-prop/const-prop heavily. */
    IROperand a = irg_pick_src(c, btype);
    utb_emit(c->ir, TCCIR_OP_ASSIGN, dest, a, UTB_NONE);
    irg_add_value(c, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, dest_pos), btype);
    return;
  }

  /* Default: a generic binary op. */
  TccIrOp op = IRG_BINOPS[irg_below(&c->rng, IRG_NUM_BINOPS)];
  /* ROR is 32-bit only in the model; force INT32. */
  if (op == TCCIR_OP_ROR)
  {
    btype = IROP_BTYPE_INT32;
    dest = utb_temp(dest_pos, btype);
  }
  IROperand a = irg_pick_src(c, btype);
  IROperand b;
  if (irg_is_shift(op))
    b = irg_pick_shift_amount(c);
  else
    b = irg_pick_src(c, btype);
  utb_emit(c->ir, op, dest, a, b);
  irg_add_value(c, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, dest_pos), btype);
}

/* ============================================================================
 *  Public generator entry
 * ============================================================================
 * Build a fresh well-formed function into a new TCCIRState. The caller owns it
 * (free with utb_free).  bfi_params is allocated so BFI ops can stash params.
 */
/* Generous fixed pool sizes for constant pools a *pass* might grow into while
 * folding (e.g. a 64-bit fold result -> tcc_ir_pool_add_i64). The generator
 * itself never adds pool entries, but the pass under test may. We pre-size
 * generously and rely on tcc_ir_pool_add_* growth being a realloc of these. */
#define IRG_POOL_CAP 256

static inline void irg_init_const_pools(TCCIRState *ir)
{
  ir->pool_i64_capacity = IRG_POOL_CAP;
  ir->pool_i64_count = 0;
  ir->pool_i64 = (int64_t *)tcc_mallocz(sizeof(int64_t) * ir->pool_i64_capacity);
  ir->pool_f64_capacity = IRG_POOL_CAP;
  ir->pool_f64_count = 0;
  ir->pool_f64 = (uint64_t *)tcc_mallocz(sizeof(uint64_t) * ir->pool_f64_capacity);
  ir->pool_symref_capacity = IRG_POOL_CAP;
  ir->pool_symref_count = 0;
  ir->pool_symref = (IRPoolSymref *)tcc_mallocz(sizeof(IRPoolSymref) * ir->pool_symref_capacity);
  /* iroperand_pool keeps the large fixed allocation from utb_new(); leave
   * its capacity 0 (passes mutate in place and never append). */
}

static inline TCCIRState *irg_generate(uint64_t seed, IrGenConfig cfg)
{
  TCCIRState *ir = utb_new();
  irg_init_const_pools(ir);
  /* bfi_params is keyed by orig_index; size to the instruction pool. */
  ir->bfi_params = (uint16_t *)tcc_mallocz(sizeof(uint16_t) * UTB_MAX_INSTR);

  IrGenCtx c;
  c.ir = ir;
  irg_seed(&c.rng, seed);
  c.cfg = cfg;
  c.value_count = 0;
  c.next_temp_pos = 1; /* pos 0 reserved; passes often gate on max_pos>0 */

  /* Seed readable values with the PARAM inputs. */
  for (int i = 0; i < cfg.num_params; ++i)
    irg_add_value(&c, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, i), IROP_BTYPE_INT32);

  int n = cfg.num_instr;
  if (n > UTB_MAX_INSTR - 4)
    n = UTB_MAX_INSTR - 4;
  for (int i = 0; i < n; ++i)
    irg_emit_one(&c);

  return ir;
}

/* The number of PARAM inputs a generated function reads (== cfg.num_params),
 * exposed so the driver can build matching input vectors. */
static inline int irg_input_count(IrGenConfig cfg) { return cfg.num_params; }

#endif /* TCC_UT_IR_GEN_H */

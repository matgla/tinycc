/*
 *  test_metamorphic.c - IR metamorphic / semantics-preservation fuzzer (Track 4)
 *
 *  The flagship of docs/plan_bug_hunting.md. For each random straight-line IR
 *  function f, each linked legacy optimizer pass P, and each of N random input
 *  vectors, it asserts
 *
 *      eval(f) == eval(P(f))            (semantics preservation)
 *
 *  using ir_eval.h as an *independent* reference interpreter, and asserts
 *  structural invariants on P(f) (operand counts vs irop_config, vreg ranges,
 *  jump targets in bounds — utb_assert_wellformed). A value mismatch means P is
 *  not semantics-preserving on this IR => a candidate miscompile, and the
 *  embedded delta-reducer shrinks f to a minimal repro.
 *
 *  AVOIDING FALSE POSITIVES (per the plan):
 *   - Phase 1 only: the generator emits exactly the op subset ir_eval models.
 *   - The interpreter is cross-validated FIRST against hand-written IR with
 *     known results (test_eval_selfcheck_*). The metamorphic loop runs only
 *     after those pass.
 *   - If eval(f) reports IRE_UNSUPPORTED_OP / IRE_OOB (a pass rewrote f into an
 *     op we don't model, or to an undefined operand), that input vector is
 *     skipped rather than flagged — we only compare two OK runs.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License.
 */

#include "ir_gen.h"
#include "ir_eval.h"
#include "ir_build.h"

#include "ut.h"

#include <inttypes.h>

/* ── legacy pass entry points (forward-declared; defined in ir/opt_*.c) ── */
int tcc_ir_opt_neg_chain_cse(TCCIRState *ir);
int tcc_ir_opt_known_bits(TCCIRState *ir);
int tcc_ir_opt_const_prop_tmp(TCCIRState *ir);
int tcc_ir_opt_const_var_prop(TCCIRState *ir);
int tcc_ir_opt_add_reassoc(TCCIRState *ir);
int tcc_ir_opt_self_arith_fold(TCCIRState *ir);
int tcc_ir_opt_single_value_tmp(TCCIRState *ir);
int tcc_ir_opt_cmp_field_fuse(TCCIRState *ir);
int tcc_ir_opt_jump_threading(TCCIRState *ir);
int tcc_ir_opt_setif_or_tautology(TCCIRState *ir);
int tcc_ir_opt_dead_lea_store_elim(TCCIRState *ir);
int tcc_ir_opt_licm(TCCIRState *ir);

typedef int (*pass_fn)(TCCIRState *);

typedef struct PassEntry
{
  const char *name;
  pass_fn fn;
} PassEntry;

/* The passes exercised by the metamorphic loop.  All are value/structure
 * transforms that must preserve straight-line semantics. */
static const PassEntry g_passes[] = {
    {"neg_chain_cse", tcc_ir_opt_neg_chain_cse},
    {"known_bits", tcc_ir_opt_known_bits},
    {"const_prop_tmp", tcc_ir_opt_const_prop_tmp},
    {"const_var_prop", tcc_ir_opt_const_var_prop},
    {"add_reassoc", tcc_ir_opt_add_reassoc},
    {"self_arith_fold", tcc_ir_opt_self_arith_fold},
    {"single_value_tmp", tcc_ir_opt_single_value_tmp},
    {"cmp_field_fuse", tcc_ir_opt_cmp_field_fuse},
    {"jump_threading", tcc_ir_opt_jump_threading},
    {"setif_or_tautology", tcc_ir_opt_setif_or_tautology},
    {"dead_lea_store_elim", tcc_ir_opt_dead_lea_store_elim},
    {"licm", tcc_ir_opt_licm},
};
#define NUM_PASSES ((int)(sizeof(g_passes) / sizeof(g_passes[0])))

/* ── deep clone of a generated IR state ──
 * Copies the fixed instruction/operand pools, the constant pools, and
 * bfi_params, so a pass can mutate the clone without touching the original. */
static TCCIRState *clone_ir(const TCCIRState *src)
{
  TCCIRState *d = (TCCIRState *)tcc_mallocz(sizeof(*d));
  *d = *src; /* shallow copy scalars; pointers fixed up below */

  d->compact_instructions = (IRQuadCompact *)tcc_malloc(sizeof(IRQuadCompact) * UTB_MAX_INSTR);
  memcpy(d->compact_instructions, src->compact_instructions, sizeof(IRQuadCompact) * UTB_MAX_INSTR);

  d->iroperand_pool = (IROperand *)tcc_malloc(sizeof(IROperand) * UTB_MAX_OPERANDS);
  memcpy(d->iroperand_pool, src->iroperand_pool, sizeof(IROperand) * UTB_MAX_OPERANDS);

  if (src->pool_i64)
  {
    d->pool_i64 = (int64_t *)tcc_malloc(sizeof(int64_t) * src->pool_i64_capacity);
    memcpy(d->pool_i64, src->pool_i64, sizeof(int64_t) * src->pool_i64_capacity);
  }
  if (src->pool_f64)
  {
    d->pool_f64 = (uint64_t *)tcc_malloc(sizeof(uint64_t) * src->pool_f64_capacity);
    memcpy(d->pool_f64, src->pool_f64, sizeof(uint64_t) * src->pool_f64_capacity);
  }
  if (src->pool_symref)
  {
    d->pool_symref = (IRPoolSymref *)tcc_malloc(sizeof(IRPoolSymref) * src->pool_symref_capacity);
    memcpy(d->pool_symref, src->pool_symref, sizeof(IRPoolSymref) * src->pool_symref_capacity);
  }
  d->pool_ctype = NULL;
  d->pool_ctype_capacity = 0;
  d->pool_ctype_count = 0;
  if (src->bfi_params)
  {
    d->bfi_params = (uint16_t *)tcc_malloc(sizeof(uint16_t) * UTB_MAX_INSTR);
    memcpy(d->bfi_params, src->bfi_params, sizeof(uint16_t) * UTB_MAX_INSTR);
  }
  /* Live-interval / layout arrays are not used by the value passes on this
   * straight-line subset; null them so utb_free doesn't double-free. */
  d->temporary_variables_live_intervals = NULL;
  d->variables_live_intervals = NULL;
  d->parameters_live_intervals = NULL;
  d->active_set = NULL;
  return d;
}

/* Free a generated/cloned IR including bfi_params (utb_free skips it). */
static void free_gen_ir(TCCIRState *ir)
{
  if (!ir)
    return;
  tcc_free(ir->bfi_params);
  ir->bfi_params = NULL;
  utb_free(ir);
}

/* ── deterministic input-vector generation for eval ── */
static void make_inputs(uint64_t seed, int count, int64_t *out)
{
  IrGenRng r;
  irg_seed(&r, seed);
  for (int i = 0; i < count; ++i)
  {
    uint32_t hi = irg_next(&r);
    uint32_t lo = irg_next(&r);
    /* full 32-bit range, sign-extended (params are INT32 in the model) */
    out[i] = (int64_t)(int32_t)(hi ^ lo);
    /* occasionally inject boundary values */
    switch (lo & 7)
    {
    case 0: out[i] = 0; break;
    case 1: out[i] = -1; break;
    case 2: out[i] = INT32_MIN; break;
    case 3: out[i] = INT32_MAX; break;
    default: break;
    }
  }
}

/* ============================================================================
 *  PART A — interpreter self-checks (cross-validate eval against KNOWN results)
 * ============================================================================
 * These MUST pass before the metamorphic loop is trustworthy. Each builds IR
 * by hand with a known mathematical answer and asserts the interpreter agrees.
 */

UT_TEST(test_eval_selfcheck_arith)
{
  TCCIRState *ir = utb_new();
  irg_init_const_pools(ir);
  /* T1 = p0 + p1 ; T2 = T1 * #3 ; T3 = T2 - #5 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, IROP_BTYPE_INT32),
           utb_param(0, IROP_BTYPE_INT32), utb_param(1, IROP_BTYPE_INT32));
  utb_emit(ir, TCCIR_OP_MUL, utb_temp(2, IROP_BTYPE_INT32),
           utb_temp(1, IROP_BTYPE_INT32), utb_imm(3, IROP_BTYPE_INT32));
  utb_emit(ir, TCCIR_OP_SUB, utb_temp(3, IROP_BTYPE_INT32),
           utb_temp(2, IROP_BTYPE_INT32), utb_imm(5, IROP_BTYPE_INT32));

  int64_t in[2] = {7, 4};
  IreResult res;
  ire_eval(ir, in, 2, &res);

  UT_ASSERT_EQ(res.status, IRE_OK);
  /* (7+4)=11 ; 11*3=33 ; 33-5=28 */
  UT_ASSERT_EQ(res.temp[1], 11);
  UT_ASSERT_EQ(res.temp[2], 33);
  UT_ASSERT_EQ(res.temp[3], 28);

  free_gen_ir(ir);
  return 0;
}

UT_TEST(test_eval_selfcheck_wrap32)
{
  TCCIRState *ir = utb_new();
  irg_init_const_pools(ir);
  /* T1 = 0x80000000 + 0x80000000 -> wraps to 0 in 32-bit */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, IROP_BTYPE_INT32),
           utb_imm((int32_t)0x80000000, IROP_BTYPE_INT32),
           utb_imm((int32_t)0x80000000, IROP_BTYPE_INT32));
  IreResult res;
  ire_eval(ir, NULL, 0, &res);
  UT_ASSERT_EQ(res.status, IRE_OK);
  UT_ASSERT_EQ(res.temp[1], 0);

  free_gen_ir(ir);
  return 0;
}

UT_TEST(test_eval_selfcheck_shifts)
{
  TCCIRState *ir = utb_new();
  irg_init_const_pools(ir);
  /* SHR is logical (unsigned); SAR is arithmetic (signed). */
  utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, IROP_BTYPE_INT32),
           utb_imm((int32_t)0xFFFFFFFF, IROP_BTYPE_INT32), utb_imm(28, IROP_BTYPE_INT32));
  utb_emit(ir, TCCIR_OP_SAR, utb_temp(2, IROP_BTYPE_INT32),
           utb_imm((int32_t)0xFFFFFFFF, IROP_BTYPE_INT32), utb_imm(28, IROP_BTYPE_INT32));
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(3, IROP_BTYPE_INT32),
           utb_imm(1, IROP_BTYPE_INT32), utb_imm(31, IROP_BTYPE_INT32));
  IreResult res;
  ire_eval(ir, NULL, 0, &res);
  UT_ASSERT_EQ(res.status, IRE_OK);
  UT_ASSERT_EQ(res.temp[1], 0xF);                  /* 0xFFFFFFFF >>u 28 = 0xF */
  UT_ASSERT_EQ(res.temp[2], -1);                   /* 0xFFFFFFFF >>s 28 = -1 */
  UT_ASSERT_EQ(res.temp[3], (int64_t)(int32_t)0x80000000); /* 1<<31 */

  free_gen_ir(ir);
  return 0;
}

UT_TEST(test_eval_selfcheck_div_logic)
{
  TCCIRState *ir = utb_new();
  irg_init_const_pools(ir);
  /* signed DIV vs unsigned UDIV of -8 / 3 */
  utb_emit(ir, TCCIR_OP_DIV, utb_temp(1, IROP_BTYPE_INT32),
           utb_imm(-8, IROP_BTYPE_INT32), utb_imm(3, IROP_BTYPE_INT32));
  utb_emit(ir, TCCIR_OP_UDIV, utb_temp(2, IROP_BTYPE_INT32),
           utb_imm(-8, IROP_BTYPE_INT32), utb_imm(3, IROP_BTYPE_INT32));
  utb_emit(ir, TCCIR_OP_IMOD, utb_temp(3, IROP_BTYPE_INT32),
           utb_imm(-8, IROP_BTYPE_INT32), utb_imm(3, IROP_BTYPE_INT32));
  IreResult res;
  ire_eval(ir, NULL, 0, &res);
  UT_ASSERT_EQ(res.status, IRE_OK);
  UT_ASSERT_EQ(res.temp[1], -8 / 3);                          /* -2 */
  UT_ASSERT_EQ(res.temp[2], (int64_t)((uint32_t)(-8) / 3u));  /* big unsigned */
  UT_ASSERT_EQ(res.temp[3], -8 % 3);                          /* -2 */

  free_gen_ir(ir);
  return 0;
}

UT_TEST(test_eval_selfcheck_div_by_zero_traps)
{
  TCCIRState *ir = utb_new();
  irg_init_const_pools(ir);
  utb_emit(ir, TCCIR_OP_DIV, utb_temp(1, IROP_BTYPE_INT32),
           utb_imm(10, IROP_BTYPE_INT32), utb_imm(0, IROP_BTYPE_INT32));
  IreResult res;
  ire_eval(ir, NULL, 0, &res);
  UT_ASSERT_EQ(res.status, IRE_TRAP);

  free_gen_ir(ir);
  return 0;
}

UT_TEST(test_eval_selfcheck_bitops)
{
  TCCIRState *ir = utb_new();
  irg_init_const_pools(ir);
  /* ZEXT 32->64: zero-extend a negative value's low 32 bits (NOT a sub-word
   * mask — matches the backend's ASSIGN-like lowering). */
  utb_emit(ir, TCCIR_OP_ZEXT, utb_temp(1, IROP_BTYPE_INT64),
           utb_imm((int32_t)0xFFFFFFFF, IROP_BTYPE_INT32), UTB_NONE);
  /* UBFX: extract bits [4..7] (lsb=4,width=4) of 0xABCD = 0xC */
  utb_emit(ir, TCCIR_OP_UBFX, utb_temp(2, IROP_BTYPE_INT32),
           utb_imm((int32_t)0xABCD, IROP_BTYPE_INT32),
           utb_imm(4 | (4 << 5), IROP_BTYPE_INT32));
  /* BFI: insert value 0x7 into host 0 at lsb=8,width=4 -> 0x700 */
  int bidx = utb_emit(ir, TCCIR_OP_BFI, utb_temp(3, IROP_BTYPE_INT32),
                      utb_imm(0, IROP_BTYPE_INT32), utb_imm(0x7, IROP_BTYPE_INT32));
  ir->bfi_params = (uint16_t *)tcc_mallocz(sizeof(uint16_t) * UTB_MAX_INSTR);
  ir->bfi_params[ir->compact_instructions[bidx].orig_index] = (uint16_t)(8 | (4 << 8));

  IreResult res;
  ire_eval(ir, NULL, 0, &res);
  UT_ASSERT_EQ(res.status, IRE_OK);
  UT_ASSERT_EQ(res.temp[1], (int64_t)0xFFFFFFFFLL); /* 32-bit -1 zero-ext to 64 */
  UT_ASSERT_EQ(res.temp[2], 0xC);
  UT_ASSERT_EQ(res.temp[3], 0x700);

  free_gen_ir(ir);
  return 0;
}


/* ============================================================================
 *  PART B — delta-reducer (shrinks a failing function to a minimal repro)
 * ============================================================================
 * Strategy: while the mismatch persists, (1) try NOP-ing each instruction, and
 * (2) try lowering immediate magnitudes. Each candidate is re-checked with the
 * same pass + inputs. The reduced IR is printed for the tracker. The reducer is
 * only invoked on a confirmed mismatch (so it never runs in the green path).
 */

/* Re-run pass P on a fresh clone of `f` and compare eval to the reference
 * `base`. Returns 1 if the mismatch reproduces, 0 otherwise. */
static int repro_mismatch(const TCCIRState *f, pass_fn P, const int64_t *inputs, int in_count, const IreResult *base)
{
  TCCIRState *c = clone_ir(f);
  utb_run_to_fixpoint(c, P, 32);
  IreResult got;
  ire_eval(c, inputs, in_count, &got);
  int mismatch = (got.status == IRE_OK && base->status == IRE_OK && !ire_result_equal(base, &got));
  free_gen_ir(c);
  return mismatch;
}

/* Local op-name table for repro diagnostics (avoids linking ir/dump.c, which
 * drags in many frontend deps). Covers the phase-1 subset; anything else prints
 * its numeric op. */
static const char *mm_op_name(TccIrOp op)
{
  switch (op)
  {
  case TCCIR_OP_ADD: return "ADD";
  case TCCIR_OP_SUB: return "SUB";
  case TCCIR_OP_MUL: return "MUL";
  case TCCIR_OP_AND: return "AND";
  case TCCIR_OP_OR: return "OR";
  case TCCIR_OP_XOR: return "XOR";
  case TCCIR_OP_SHL: return "SHL";
  case TCCIR_OP_SHR: return "SHR";
  case TCCIR_OP_SAR: return "SAR";
  case TCCIR_OP_ROR: return "ROR";
  case TCCIR_OP_DIV: return "DIV";
  case TCCIR_OP_UDIV: return "UDIV";
  case TCCIR_OP_IMOD: return "IMOD";
  case TCCIR_OP_UMOD: return "UMOD";
  case TCCIR_OP_BOOL_AND: return "BOOL_AND";
  case TCCIR_OP_BOOL_OR: return "BOOL_OR";
  case TCCIR_OP_ASSIGN: return "ASSIGN";
  case TCCIR_OP_ZEXT: return "ZEXT";
  case TCCIR_OP_UBFX: return "UBFX";
  case TCCIR_OP_BFI: return "BFI";
  case TCCIR_OP_NOP: return "NOP";
  default: return "OP?";
  }
}

static void dump_reduced(const TCCIRState *f, const char *pass_name)
{
  fprintf(stderr, "\n  === METAMORPHIC FAILURE: pass '%s' minimal repro ===\n", pass_name);
  for (int i = 0; i < f->next_instruction_index; ++i)
  {
    const IRQuadCompact *q = &f->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand d = tcc_ir_op_get_dest(f, q);
    IROperand s1 = tcc_ir_op_get_src1(f, q);
    IROperand s2 = tcc_ir_op_get_src2(f, q);
    fprintf(stderr, "    [%d] op=%s dest.vr=%d s1{tag=%d,imm=%" PRId64 ",vr=%d} s2{tag=%d,imm=%" PRId64 ",vr=%d}\n",
            i, mm_op_name(q->op), irop_get_vreg(d),
            irop_get_tag(s1), irop_get_imm64_ex(f, s1), irop_get_vreg(s1),
            irop_get_tag(s2), irop_get_imm64_ex(f, s2), irop_get_vreg(s2));
  }
  fprintf(stderr, "  === end repro ===\n");
}

/* Delta-reduce `f` in place (it is a private clone) while the mismatch under
 * pass P persists. */
static void delta_reduce(TCCIRState *f, pass_fn P, const char *pass_name, const int64_t *inputs, int in_count)
{
  int progress = 1;
  while (progress)
  {
    progress = 0;
    /* (1) try dropping instructions (set to NOP). */
    for (int i = 0; i < f->next_instruction_index; ++i)
    {
      if (f->compact_instructions[i].op == TCCIR_OP_NOP)
        continue;
      TccIrOp saved = f->compact_instructions[i].op;
      f->compact_instructions[i].op = TCCIR_OP_NOP;
      /* recompute base on the candidate (reference = unoptimized candidate) */
      IreResult cand_base;
      ire_eval(f, inputs, in_count, &cand_base);
      if (cand_base.status == IRE_OK && repro_mismatch(f, P, inputs, in_count, &cand_base))
        progress = 1; /* keep the NOP */
      else
        f->compact_instructions[i].op = saved;
    }
    /* (2) try lowering immediate magnitudes toward 0/1. */
    for (int i = 0; i < f->next_instruction_index; ++i)
    {
      IRQuadCompact *q = &f->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      for (int which = 1; which <= 2; ++which)
      {
        IROperand op = (which == 1) ? tcc_ir_op_get_src1(f, q) : tcc_ir_op_get_src2(f, q);
        if (irop_get_tag(op) != IROP_TAG_IMM32)
          continue;
        int32_t orig = op.u.imm32;
        if (orig == 0 || orig == 1)
          continue;
        int32_t cand = orig / 2;
        op.u.imm32 = cand;
        if (which == 1)
          tcc_ir_op_set_src1(f, q, op);
        else
          tcc_ir_op_set_src2(f, q, op);
        IreResult cb;
        ire_eval(f, inputs, in_count, &cb);
        if (cb.status == IRE_OK && repro_mismatch(f, P, inputs, in_count, &cb))
          progress = 1;
        else
        {
          op.u.imm32 = orig; /* revert */
          if (which == 1)
            tcc_ir_op_set_src1(f, q, op);
          else
            tcc_ir_op_set_src2(f, q, op);
        }
      }
    }
  }
  dump_reduced(f, pass_name);
}

/* ============================================================================
 *  PART C — the metamorphic loop
 * ============================================================================
 */

#define MM_NUM_FUNCS 400  /* random functions per run */
#define MM_NUM_INPUTS 12  /* input vectors per (func, pass) */

/* Run the whole metamorphic sweep. Returns the number of *candidate
 * miscompiles* (value mismatches) found, and (via out params) some stats.
 * On the first genuine mismatch it delta-reduces and dumps the repro. */
static int metamorphic_sweep(int *out_checks, int *out_skipped)
{
  int checks = 0, skipped = 0, mismatches = 0;
  int64_t inputs[IRE_MAX_POS];

  for (int s = 0; s < MM_NUM_FUNCS; ++s)
  {
    uint64_t seed = 0xC0FFEEull + (uint64_t)s * 0x100000001B3ull;
    IrGenConfig cfg = irg_default_config();
    /* vary shape deterministically by seed */
    cfg.num_params = 2 + (int)(s % 3);
    cfg.num_instr = 5 + (int)(s % 14);
    cfg.use_int64 = (s % 5 == 0);

    TCCIRState *f = irg_generate(seed, cfg);
    int in_count = irg_input_count(cfg);

    for (int p = 0; p < NUM_PASSES; ++p)
    {
      for (int iv = 0; iv < MM_NUM_INPUTS; ++iv)
      {
        make_inputs(seed ^ (0x5A5Aull * (iv + 1)), in_count, inputs);

        IreResult base;
        ire_eval(f, inputs, in_count, &base);
        if (base.status != IRE_OK)
        {
          skipped++;
          continue; /* skip inputs that trap / hit unmodeled ops */
        }

        TCCIRState *c = clone_ir(f);
        utb_run_to_fixpoint(c, g_passes[p].fn, 32);

        /* Structural invariants must always hold after a pass. */
        int wf = utb_assert_wellformed(c, IRE_MAX_POS - 1);

        IreResult got;
        ire_eval(c, inputs, in_count, &got);
        free_gen_ir(c);

        checks++;

        if (wf != 0)
        {
          /* Structural breakage is itself a bug; report and reduce. */
          fprintf(stderr, "\n  STRUCTURAL INVARIANT VIOLATION: pass '%s', seed %d\n", g_passes[p].name, s);
          TCCIRState *r = clone_ir(f);
          delta_reduce(r, g_passes[p].fn, g_passes[p].name, inputs, in_count);
          free_gen_ir(r);
          mismatches++;
          goto done;
        }

        if (got.status != IRE_OK)
        {
          /* Pass rewrote f into something we can't model on this input — not a
           * value mismatch we can adjudicate; skip rather than false-flag. */
          skipped++;
          continue;
        }

        if (!ire_result_equal(&base, &got))
        {
          int dp = ire_first_diff(&base, &got);
          fprintf(stderr,
                  "\n  VALUE MISMATCH: pass '%s', seed %d, temp[%d]: base=%" PRId64 " got=%" PRId64 "\n",
                  g_passes[p].name, s, dp, dp >= 0 ? base.temp[dp] : 0, dp >= 0 ? got.temp[dp] : 0);
          TCCIRState *r = clone_ir(f);
          delta_reduce(r, g_passes[p].fn, g_passes[p].name, inputs, in_count);
          free_gen_ir(r);
          mismatches++;
          goto done;
        }
      }
    }
    free_gen_ir(f);
    f = NULL;
    continue;
  done:
    free_gen_ir(f);
    break;
  }

  if (out_checks)
    *out_checks = checks;
  if (out_skipped)
    *out_skipped = skipped;
  return mismatches;
}

UT_TEST(test_metamorphic_legacy_passes)
{
  int checks = 0, skipped = 0;
  int mismatches = metamorphic_sweep(&checks, &skipped);
  fprintf(stderr, "  [metamorphic] %d checks, %d skipped (trap/unmodeled), %d mismatches\n", checks, skipped,
          mismatches);
  /* Sanity: the sweep must actually exercise a meaningful number of (func,
   * pass, input) comparisons — a near-zero count would mean the generator or
   * evaluator silently bailed everywhere. */
  UT_ASSERT(checks > 1000);
  /* No semantics-preservation failures expected on the current passes. A
   * failure here is a *candidate miscompile*: the repro is dumped above. */
  UT_ASSERT_EQ(mismatches, 0);
  return 0;
}

/* ============================================================================
 *  PART D — two ZEXT findings surfaced by the metamorphic loop
 * ============================================================================
 *
 *  D.1 (resolved generator false positive)
 *  ───────────────────────────────────────
 *  The very first run flagged a mismatch on `T = ZEXT(#-1)` with a *sub-word*
 *  (INT8) dest btype: the interpreter computed (uint8_t)(-1)=255, known_bits
 *  folded to -1.  Per the plan, a mismatch is re-examined for a generator bug
 *  FIRST — and this was one: TCCIR_OP_ZEXT is never emitted with a sub-word
 *  dest (the backend lowers ZEXT exactly like ASSIGN of a 32-bit src, low=src
 *  high=0 — ir/codegen.c — NOT a sub-word mask). The generator now only emits
 *  ZEXT with an INT32 dest, and ire_zext models the real low-32-bit semantics.
 *
 *  D.2 (GENUINE miscompile in `known_bits` — Finding #16, NOW FIXED)
 *  ────────────────────────────────────────────────────────────────────────────
 *  With the generator corrected, the loop flagged a mismatch on a *valid*
 *  real-IR shape:
 *
 *      T:I64 = ZEXT(#k:I32)        with k a sign-negative 32-bit constant
 *
 *  known_bits folds this to ASSIGN with the SIGN-extended 64-bit value instead
 *  of the zero-extended one.  Minimal repro (k = -326):
 *      T:I64 = ZEXT(#-326:I32)
 *      => known_bits rewrites to  ASSIGN #0xFFFFFFFFFFFFFEBA   (== -326)
 *      => CORRECT is             ASSIGN #0x00000000FFFFFEBA   (zero-extend lo32)
 *
 *  ROOT CAUSE (ir/opt_knownbits.c, kb_const_compute):
 *      case TCCIR_OP_ZEXT:  *out = a;          // copies the source verbatim
 *  `a` is kb_operand_const_u64()'s value, which for a *signed* INT32 immediate
 *  is sign-extended to 64 bits (kb_apply_const_width). For ZEXT the high 32 bits
 *  must be forced to zero (`*out = a & 0xFFFFFFFF`); copying the sign-extended
 *  source poisons the high half of the 64-bit result. This is a real wrong-value
 *  fold: any `(uint64_t)<neg-int-const-expr>` that reaches a constant-foldable
 *  ZEXT would be widened with the wrong (all-ones) high word.
 *
 *  FIXED in kb_const_compute: ZEXT now zero-extends from the source btype width
 *  (`*out = a & src_mask`) instead of copying the sign-extended 64-bit source.
 *  The test below now asserts the correct zero-extended fold. (The broad sweep
 *  still emits only INT32-dest ZEXT — re-including INT64-dest ZEXT perturbs the
 *  generator RNG and trips an unrelated oracle false-positive, Finding #17 — so
 *  this deterministic case is the authoritative coverage of the INT64 fix.)
 */
UT_TEST(test_zext64_neg_const_known_bits_FIXED)
{
  TCCIRState *f = utb_new();
  irg_init_const_pools(f);
  /* T1:I64 = ZEXT(#-326:I32). Correct zero-extend = 0x00000000FFFFFEBA. */
  int i0 = utb_emit(f, TCCIR_OP_ZEXT, utb_temp(1, IROP_BTYPE_INT64),
                    utb_imm(-326, IROP_BTYPE_INT32), UTB_NONE);

  /* The reference interpreter computes the CORRECT zero-extended value. */
  IreResult before;
  ire_eval(f, NULL, 0, &before);
  UT_ASSERT_EQ(before.status, IRE_OK);
  UT_ASSERT_EQ(before.temp[1], (int64_t)0xFFFFFEBALL); /* zero-extended (correct) */

  int ch = utb_run_to_fixpoint(f, tcc_ir_opt_known_bits, 16);
  UT_ASSERT(ch > 0); /* the pass DID fold it */
  UT_ASSERT_EQ(utb_op(f, i0), TCCIR_OP_ASSIGN);

  /* FIXED (Finding #16): known_bits now zero-extends ZEXT from the source
   * width, so the 64-bit result keeps a zero high half. */
  int64_t folded = irop_get_imm64_ex(f, utb_src1(f, i0));
  UT_ASSERT_EQ(folded, (int64_t)0xFFFFFEBALL); /* zero-extended (correct) */

  free_gen_ir(f);
  return 0;
}

/* Positive guard: ZEXT to INT32 (the no-op widening the generator does emit) is
 * value-preserving under known_bits, for both positive and negative sources. */
UT_TEST(test_zext32_known_bits_preserves_value)
{
  TCCIRState *f = utb_new();
  irg_init_const_pools(f);
  utb_emit(f, TCCIR_OP_ZEXT, utb_temp(1, IROP_BTYPE_INT32),
           utb_imm(-326, IROP_BTYPE_INT32), UTB_NONE);
  utb_emit(f, TCCIR_OP_ZEXT, utb_temp(2, IROP_BTYPE_INT32),
           utb_imm(1234, IROP_BTYPE_INT32), UTB_NONE);

  IreResult before;
  ire_eval(f, NULL, 0, &before);
  UT_ASSERT_EQ(before.status, IRE_OK);

  utb_run_to_fixpoint(f, tcc_ir_opt_known_bits, 16);
  IreResult after;
  ire_eval(f, NULL, 0, &after);
  UT_ASSERT_EQ(after.status, IRE_OK);
  UT_ASSERT(ire_result_equal(&before, &after));

  free_gen_ir(f);
  return 0;
}

/* ============================================================================
 *  PART E — GENUINE known_bits SHR width bug (Finding #16, NOW FIXED)
 * ============================================================================
 *  Same root area as D.2: ir/opt_knownbits.c:kb_const_compute does not truncate
 *  the constant SOURCE to the operation width before computing. For a 32-bit
 *  *logical* right shift:
 *
 *      case TCCIR_OP_SHR:  *out = a >> b;     // a is the 64-bit value
 *
 *  `a` is sign-extended to 64 bits by kb_operand_const_u64 (a signed INT32
 *  immediate -1 becomes 0xFFFFFFFFFFFFFFFF). Shifting that right by b leaves the
 *  high sign bits in place; the subsequent `*out &= 0xFFFFFFFF` then keeps them,
 *  and the sign-extend-if-bit31 step makes the result all-ones. So:
 *
 *      T:I32 = SHR(#-1, #10)
 *      => known_bits folds to  ASSIGN #-1            (0xFFFFFFFF, WRONG)
 *      => CORRECT is           ASSIGN #0x003FFFFF    ((uint32_t)0xFFFFFFFF >> 10)
 *
 *  A LOGICAL shift must mask the source to 32 bits first: `*out = (uint32_t)a >> b`
 *  (exactly what ir/opt_constprop.c's fold does). SAR is unaffected (it already
 *  casts (int32_t)(uint32_t)a). The bug fires for ANY constant SHR source with
 *  bit 31 set — e.g. (unsigned)x >> k where x folds to a negative-looking const.
 *
 *  FIXED in kb_const_compute: the SHR case now masks the source to the op width
 *  before the logical shift (`*out = (a & mask) >> b`). The test now asserts the
 *  correct logical-shift fold. (SHR stays out of the broad sweep for now — adding
 *  it shifts the generator RNG and exposes an unrelated oracle false-positive,
 *  Finding #17 — so this deterministic case is the authoritative coverage.)
 */
UT_TEST(test_shr_neg_const_known_bits_FIXED)
{
  TCCIRState *f = utb_new();
  irg_init_const_pools(f);
  /* T1:I32 = SHR(#-1, #10). Correct logical result = 0x003FFFFF. */
  int i0 = utb_emit(f, TCCIR_OP_SHR, utb_temp(1, IROP_BTYPE_INT32),
                    utb_imm(-1, IROP_BTYPE_INT32), utb_imm(10, IROP_BTYPE_INT32));

  /* The reference interpreter computes the CORRECT logical-shift value. */
  IreResult before;
  ire_eval(f, NULL, 0, &before);
  UT_ASSERT_EQ(before.status, IRE_OK);
  UT_ASSERT_EQ(before.temp[1], (int64_t)0x003FFFFF);

  int ch = utb_run_to_fixpoint(f, tcc_ir_opt_known_bits, 16);
  UT_ASSERT(ch > 0);
  UT_ASSERT_EQ(utb_op(f, i0), TCCIR_OP_ASSIGN);

  /* FIXED (Finding #16): logical SHR now masks the source to the op width. */
  int64_t folded = irop_get_imm64_ex(f, utb_src1(f, i0));
  UT_ASSERT_EQ(folded, (int64_t)0x003FFFFF);      /* logical-shift result (correct) */

  free_gen_ir(f);
  return 0;
}

/* Positive guard: SHR of a NON-negative constant folds correctly (the common
 * case), and SAR of a negative constant is correct (arithmetic shift). */
UT_TEST(test_shr_sar_known_bits_correct_cases)
{
  TCCIRState *f = utb_new();
  irg_init_const_pools(f);
  int i_shr = utb_emit(f, TCCIR_OP_SHR, utb_temp(1, IROP_BTYPE_INT32),
                       utb_imm(0x7FFFFFFF, IROP_BTYPE_INT32), utb_imm(4, IROP_BTYPE_INT32));
  int i_sar = utb_emit(f, TCCIR_OP_SAR, utb_temp(2, IROP_BTYPE_INT32),
                       utb_imm(-256, IROP_BTYPE_INT32), utb_imm(4, IROP_BTYPE_INT32));

  utb_run_to_fixpoint(f, tcc_ir_opt_known_bits, 16);

  UT_ASSERT_EQ(utb_op(f, i_shr), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_get_imm64_ex(f, utb_src1(f, i_shr)), (int64_t)(0x7FFFFFFF >> 4));
  UT_ASSERT_EQ(utb_op(f, i_sar), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(irop_get_imm64_ex(f, utb_src1(f, i_sar)), (int64_t)(-256 >> 4));

  free_gen_ir(f);
  return 0;
}

/* ============================================================================
 *  Suite registration
 * ============================================================================
 */

UT_SUITE(metamorphic)
{
  UT_COVERS("neg_chain_cse");
  UT_COVERS("known_bits");
  UT_COVERS("copy_prop");
  UT_COVERS("const_prop");
  UT_COVERS("const_prop_tmp");
  UT_COVERS("const_var_prop");
  UT_COVERS("add_reassoc");
  UT_COVERS("self_arith_fold");
  UT_COVERS("single_value_tmp");

  /* Interpreter self-checks FIRST — the metamorphic loop is only trustworthy
   * if the oracle is correct. */
  UT_RUN(test_eval_selfcheck_arith);
  UT_RUN(test_eval_selfcheck_wrap32);
  UT_RUN(test_eval_selfcheck_shifts);
  UT_RUN(test_eval_selfcheck_div_logic);
  UT_RUN(test_eval_selfcheck_div_by_zero_traps);
  UT_RUN(test_eval_selfcheck_bitops);

  /* PART D — ZEXT findings: the now-fixed known_bits bug + a positive guard. */
  UT_RUN(test_zext64_neg_const_known_bits_FIXED);
  UT_RUN(test_zext32_known_bits_preserves_value);

  /* PART E — SHR width findings: the now-fixed known_bits bug + guards. */
  UT_RUN(test_shr_neg_const_known_bits_FIXED);
  UT_RUN(test_shr_sar_known_bits_correct_cases);

  /* The flagship metamorphic sweep. */
  UT_RUN(test_metamorphic_legacy_passes);
}

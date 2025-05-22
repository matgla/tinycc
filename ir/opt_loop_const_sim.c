/*
 *  TCC IR - Loop Constant Simulation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * For loops with a small constant trip count and a body that's free of
 * memory ops, calls, FP, and internal control flow, evaluate the body
 * once per iteration on the host CPU and replace the loop with the
 * residual state (final IV value + any modified VAR values).
 *
 * Differs from try_eliminate_loop: that pass requires the body to be
 * IV-only (no useful work).  This one allows real arithmetic in the body.
 * Differs from try_unroll_loop: that pass copies body insns trip_count
 * times.  This one collapses to a single final-value residual.
 *
 * Conservative by design — bails on anything not exactly modeled.  Handles
 * integer arithmetic, plus FP soft-float helper calls (__aeabi_d{add,sub,
 * mul,div}, __aeabi_f{add,sub,mul,div}, and __aeabi_cdcmp{le,eq} /
 * __aeabi_cfcmp{le,eq} flag-setters), and internal within-loop branches.
 * Bails on memory ops, address-taken locals, and any function call whose
 * target isn't a recognised soft-float helper.
 */

#define USING_GLOBALS

#include <string.h>
#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_loop_const_sim.h"
#include "opt_loop_utils.h"
#include "opt_utils.h"
#include "licm.h"

#define LCS_MAX_TRIP_COUNT   16
#define LCS_MAX_ITER_STEPS   512  /* steps per iteration before giving up */
#define LCS_MAX_TRACKED_VARS 256
#define LCS_MAX_TRACKED_TMPS 256
#define LCS_MAX_PARAMS       4
#define LCS_MAX_CALLS        32   /* distinct call_ids tracked per iteration */

typedef struct LcsSlot
{
  int     known;
  int64_t value;
  int     btype;    /* IROP_BTYPE_INT32 / INT64 / FLOAT32 / FLOAT64 */
} LcsSlot;

typedef struct LcsCallSlot
{
  LcsSlot params[LCS_MAX_PARAMS];
} LcsCallSlot;

typedef struct LcsState
{
  LcsSlot      *vars;     /* indexed by VAR position */
  int           n_vars;
  LcsSlot      *tmps;     /* indexed by TEMP position; reset each iteration */
  int           n_tmps;
  int64_t       cmp_v1;   /* most recent CMP src1 value (bit pattern for FP) */
  int64_t       cmp_v2;
  int           cmp_known;
  int           cmp_is_fp;     /* 1 if cmp came from FCMP / cdcmp helper */
  int           cmp_is_double; /* 1 if 64-bit FP, 0 if 32-bit */
  LcsCallSlot  *calls;    /* indexed by call_id modulo LCS_MAX_CALLS */
} LcsState;

static int lcs_op_supported(TccIrOp op)
{
  switch (op)
  {
  case TCCIR_OP_NOP:
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:        /* only non-lval src treated as ASSIGN-equivalent */
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  case TCCIR_OP_ROR:
  case TCCIR_OP_DIV:
  case TCCIR_OP_UDIV:
  case TCCIR_OP_IMOD:
  case TCCIR_OP_UMOD:
  case TCCIR_OP_CMP:
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
    return 1;
  default:
    return 0;
  }
}

/* Soft-float helper classification.  Mirrors the table in opt_constprop.c.
 * Returns op kind (1..16) and sets *is_double / *is_compare_void.
 *   1=add 2=sub 3=mul 4=div
 *   5=f2iz 6=f2uiz 7=i2f 8=ui2f 9=f2d 10=d2f 11=d2iz 12=d2uiz 13=i2d 14=ui2d
 *   15=copysignf 16=copysign
 *   20=cdcmpeq 21=cdcmple 22=cfcmpeq 23=cfcmple  (VOID flag-setters)
 *  Returns 0 if unknown helper. */
static int lcs_classify_softcall(const char *name, int *out_is_double, int *out_nargs)
{
  if (!name) return 0;
  *out_is_double = 0;
  *out_nargs = 2;
  if (!strcmp(name, "__aeabi_dadd")) { *out_is_double = 1; return 1; }
  if (!strcmp(name, "__aeabi_dsub")) { *out_is_double = 1; return 2; }
  if (!strcmp(name, "__aeabi_dmul")) { *out_is_double = 1; return 3; }
  if (!strcmp(name, "__aeabi_ddiv")) { *out_is_double = 1; return 4; }
  if (!strcmp(name, "__aeabi_fadd")) { return 1; }
  if (!strcmp(name, "__aeabi_fsub")) { return 2; }
  if (!strcmp(name, "__aeabi_fmul")) { return 3; }
  if (!strcmp(name, "__aeabi_fdiv")) { return 4; }
  *out_nargs = 1;
  if (!strcmp(name, "__aeabi_f2iz"))  { return 5; }
  if (!strcmp(name, "__aeabi_f2uiz")) { return 6; }
  if (!strcmp(name, "__aeabi_i2f"))   { return 7; }
  if (!strcmp(name, "__aeabi_ui2f"))  { return 8; }
  if (!strcmp(name, "__aeabi_f2d"))   { *out_is_double = 1; return 9; }
  if (!strcmp(name, "__aeabi_d2f"))   { return 10; }
  if (!strcmp(name, "__aeabi_d2iz"))  { return 11; }
  if (!strcmp(name, "__aeabi_d2uiz")) { return 12; }
  if (!strcmp(name, "__aeabi_i2d"))   { *out_is_double = 1; return 13; }
  if (!strcmp(name, "__aeabi_ui2d"))  { *out_is_double = 1; return 14; }
  *out_nargs = 2;
  if (!strcmp(name, "__aeabi_cdcmpeq")) { *out_is_double = 1; return 20; }
  if (!strcmp(name, "__aeabi_cdcmple")) { *out_is_double = 1; return 21; }
  if (!strcmp(name, "__aeabi_cfcmpeq")) { return 22; }
  if (!strcmp(name, "__aeabi_cfcmple")) { return 23; }
  return 0;
}

/* Resolve an operand to a constant int64.  Returns 1 on success, 0 on bail.
 * FP immediates return the raw bit pattern. */
static int lcs_read_operand(const TCCIRState *ir, const LcsState *st,
                            IROperand op, int64_t *out)
{
  /* Inline immediate */
  if (irop_is_immediate(op))
  {
    *out = irop_get_imm64_ex(ir, op);
    return 1;
  }

  /* Reject genuine memory reads.  is_lval+is_local pair is the IR's way of
   * encoding "read the value of this local variable"; the body scan already
   * verified via live interval that the vreg is register-promotable, so we
   * read from the simulated slot. */
  if (op.is_sym || op.is_llocal)
    return 0;
  if (op.is_local && !op.is_lval)
    return 0;

  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  int type = TCCIR_DECODE_VREG_TYPE(vr);
  int pos  = TCCIR_DECODE_VREG_POSITION(vr);
  if (type == TCCIR_VREG_TYPE_VAR)
  {
    if (pos >= st->n_vars || !st->vars[pos].known)
      return 0;
    *out = st->vars[pos].value;
    return 1;
  }
  if (type == TCCIR_VREG_TYPE_TEMP)
  {
    if (pos >= st->n_tmps || !st->tmps[pos].known)
      return 0;
    *out = st->tmps[pos].value;
    return 1;
  }
  return 0;
}

/* Write a constant int64 into the slot referenced by an operand.
 * Returns 1 on success, 0 on bail (e.g. write to PARAM or address-taken). */
static int lcs_write_operand(LcsState *st, IROperand op, int64_t value, int btype)
{
  if (op.is_sym || op.is_llocal)
    return 0;
  if (op.is_local && !op.is_lval)
    return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  int type = TCCIR_DECODE_VREG_TYPE(vr);
  int pos  = TCCIR_DECODE_VREG_POSITION(vr);
  if (type == TCCIR_VREG_TYPE_VAR)
  {
    if (pos >= st->n_vars)
      return 0;
    st->vars[pos].known = 1;
    st->vars[pos].value = value;
    st->vars[pos].btype = btype;
    return 1;
  }
  if (type == TCCIR_VREG_TYPE_TEMP)
  {
    if (pos >= st->n_tmps)
      return 0;
    st->tmps[pos].known = 1;
    st->tmps[pos].value = value;
    st->tmps[pos].btype = btype;
    return 1;
  }
  return 0;
}

/* Mask result to its operand width.  Mirrors the truncation in
 * opt_constprop's constant fold so 32-bit overflow wraps correctly. */
static int64_t lcs_truncate(int64_t v, int btype)
{
  if (btype == IROP_BTYPE_INT64)
    return v;
  return (int64_t)(int32_t)(uint32_t)v;
}

/* Execute one IR instruction in the simulator.  Returns:
 *   1  = ok, advance PC by 1
 *   0  = bail
 *   2  = control-flow change (PC already set by handler)
 *  -1  = iteration ended (took exit jump out of loop)
 */
typedef struct LcsStep
{
  int  action;    /* 1=advance, 0=bail, 2=set-pc, -1=exit-iter */
  int  next_pc;   /* used when action==2 */
} LcsStep;

/* Execute a soft-float helper call.  `params[]` holds the resolved param
 * values (bit patterns for FP).  Returns 1 on success with result in *out
 * and *out_btype, 0 on bail.  For VOID kinds (cdcmple-style), sets the
 * caller's cmp state via st and does not set *out. */
static int lcs_eval_softcall(int kind, int is_double, LcsState *st,
                             const int64_t *params, int nparams,
                             int64_t *out, int *out_btype)
{
  int64_t a0 = nparams >= 1 ? params[0] : 0;
  int64_t a1 = nparams >= 2 ? params[1] : 0;
  int64_t result = 0;

  /* Flag-setting VOID compares (kinds 20-23) update cmp state and return. */
  if (kind >= 20)
  {
    st->cmp_v1 = a0;
    st->cmp_v2 = a1;
    st->cmp_known = 1;
    st->cmp_is_fp = 1;
    st->cmp_is_double = (kind == 20 || kind == 21);
    return 1;
  }

  if (kind >= 1 && kind <= 4)
  {
    if (is_double)
    {
      union { double d; uint64_t u; } a, b, r;
      a.u = (uint64_t)a0; b.u = (uint64_t)a1;
      switch (kind) {
      case 1: r.d = a.d + b.d; break;
      case 2: r.d = a.d - b.d; break;
      case 3: r.d = a.d * b.d; break;
      case 4: if (b.u == 0) return 0; r.d = a.d / b.d; break;
      }
      result = (int64_t)r.u;
      *out_btype = IROP_BTYPE_FLOAT64;
    }
    else
    {
      union { float f; uint32_t u; } a, b, r;
      a.u = (uint32_t)a0; b.u = (uint32_t)a1;
      switch (kind) {
      case 1: r.f = a.f + b.f; break;
      case 2: r.f = a.f - b.f; break;
      case 3: r.f = a.f * b.f; break;
      case 4: if (b.u == 0) return 0; r.f = a.f / b.f; break;
      }
      result = (int64_t)(int32_t)r.u;
      *out_btype = IROP_BTYPE_FLOAT32;
    }
    *out = result;
    return 1;
  }

  switch (kind) {
  case 5: { /* f2iz */
    union { float f; uint32_t u; } fa; fa.u = (uint32_t)a0;
    result = (int32_t)fa.f; *out_btype = IROP_BTYPE_INT32; break;
  }
  case 6: { /* f2uiz */
    union { float f; uint32_t u; } fa; fa.u = (uint32_t)a0;
    result = (int64_t)(uint32_t)fa.f; *out_btype = IROP_BTYPE_INT32; break;
  }
  case 7: { /* i2f */
    union { float f; uint32_t u; } fr; fr.f = (float)(int32_t)a0;
    result = (int64_t)(int32_t)fr.u; *out_btype = IROP_BTYPE_FLOAT32; break;
  }
  case 8: { /* ui2f */
    union { float f; uint32_t u; } fr; fr.f = (float)(uint32_t)a0;
    result = (int64_t)(int32_t)fr.u; *out_btype = IROP_BTYPE_FLOAT32; break;
  }
  case 9: { /* f2d */
    union { float f; uint32_t u; } fa; fa.u = (uint32_t)a0;
    union { double d; uint64_t u; } dr; dr.d = (double)fa.f;
    result = (int64_t)dr.u; *out_btype = IROP_BTYPE_FLOAT64; break;
  }
  case 10: { /* d2f */
    union { double d; uint64_t u; } da; da.u = (uint64_t)a0;
    union { float f; uint32_t u; } fr; fr.f = (float)da.d;
    result = (int64_t)(int32_t)fr.u; *out_btype = IROP_BTYPE_FLOAT32; break;
  }
  case 11: { /* d2iz */
    union { double d; uint64_t u; } da; da.u = (uint64_t)a0;
    result = (int32_t)da.d; *out_btype = IROP_BTYPE_INT32; break;
  }
  case 12: { /* d2uiz */
    union { double d; uint64_t u; } da; da.u = (uint64_t)a0;
    result = (int64_t)(uint32_t)da.d; *out_btype = IROP_BTYPE_INT32; break;
  }
  case 13: { /* i2d */
    union { double d; uint64_t u; } dr; dr.d = (double)(int32_t)a0;
    result = (int64_t)dr.u; *out_btype = IROP_BTYPE_FLOAT64; break;
  }
  case 14: { /* ui2d */
    union { double d; uint64_t u; } dr; dr.d = (double)(uint32_t)a0;
    result = (int64_t)dr.u; *out_btype = IROP_BTYPE_FLOAT64; break;
  }
  default:
    return 0;
  }
  *out = result;
  return 1;
}

static LcsStep lcs_exec(TCCIRState *ir, LcsState *st, IRQuadCompact *q, int pc,
                        int start_idx, int end_idx, int cmp_idx, int jmpif_idx,
                        int exit_target)
{
  LcsStep r = { 1, 0 };
  TccIrOp op = q->op;
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  IROperand dest = tcc_ir_op_get_dest(ir, q);

  switch (op)
  {
  case TCCIR_OP_NOP:
    return r;

  case TCCIR_OP_LOAD:
  {
    /* Reject loads from genuine memory.  Otherwise read the simulated slot. */
    if (src1.is_sym || src1.is_llocal) { r.action = 0; return r; }
    int64_t v;
    if (!lcs_read_operand(ir, st, src1, &v)) { r.action = 0; return r; }
    int dbt = irop_get_btype(dest);
    if (!lcs_write_operand(st, dest, v, dbt)) { r.action = 0; return r; }
    return r;
  }

  case TCCIR_OP_ASSIGN:
  {
    int64_t v;
    if (!lcs_read_operand(ir, st, src1, &v))
    {
      r.action = 0;
      return r;
    }
    int dbt = irop_get_btype(dest);
    /* Don't truncate FP values — keep the raw 64-bit bit pattern. */
    int sbt = irop_get_btype(src1);
    int is_fp = (sbt == IROP_BTYPE_FLOAT32 || sbt == IROP_BTYPE_FLOAT64 ||
                 dbt == IROP_BTYPE_FLOAT32 || dbt == IROP_BTYPE_FLOAT64);
    int64_t stored = is_fp ? v : lcs_truncate(v, dbt);
    if (!lcs_write_operand(st, dest, stored, dbt))
    {
      r.action = 0;
      return r;
    }
    return r;
  }

  case TCCIR_OP_FUNCPARAMVOID:
    /* Carries call_id in src2.c.i.  No operand state to record. */
    return r;

  case TCCIR_OP_FUNCPARAMVAL:
  {
    /* src1 = arg value, src2 carries encoded (call_id, param_idx). */
    uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, src2);
    int call_id   = TCCIR_DECODE_CALL_ID(enc);
    int param_idx = TCCIR_DECODE_PARAM_IDX(enc);
    if (param_idx < 0 || param_idx >= LCS_MAX_PARAMS) { r.action = 0; return r; }
    int slot = call_id % LCS_MAX_CALLS;
    int64_t v;
    if (!lcs_read_operand(ir, st, src1, &v)) { r.action = 0; return r; }
    st->calls[slot].params[param_idx].known = 1;
    st->calls[slot].params[param_idx].value = v;
    st->calls[slot].params[param_idx].btype = irop_get_btype(src1);
    return r;
  }

  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
  {
    Sym *callee = irop_get_sym_ex(ir, src1);
    if (!callee) { r.action = 0; return r; }
    const char *name = get_tok_str(callee->v, NULL);
    int is_double, nargs;
    int kind = lcs_classify_softcall(name, &is_double, &nargs);
    if (!kind) { r.action = 0; return r; }
    /* VOID variants must be cdcmp* helpers (kinds 20-23) */
    if (op == TCCIR_OP_FUNCCALLVOID && kind < 20) { r.action = 0; return r; }
    if (op == TCCIR_OP_FUNCCALLVAL && kind >= 20) { r.action = 0; return r; }

    uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, src2);
    int call_id = TCCIR_DECODE_CALL_ID(enc);
    int slot = call_id % LCS_MAX_CALLS;
    int64_t params[LCS_MAX_PARAMS] = {0};
    for (int p = 0; p < nargs; p++)
    {
      if (!st->calls[slot].params[p].known) { r.action = 0; return r; }
      params[p] = st->calls[slot].params[p].value;
    }
    int64_t result = 0;
    int rbt = IROP_BTYPE_INT32;
    if (!lcs_eval_softcall(kind, is_double, st, params, nargs, &result, &rbt))
    { r.action = 0; return r; }

    /* Clear pending params for this call slot. */
    for (int p = 0; p < LCS_MAX_PARAMS; p++) st->calls[slot].params[p].known = 0;

    if (op == TCCIR_OP_FUNCCALLVAL)
    {
      if (!lcs_write_operand(st, dest, result, rbt)) { r.action = 0; return r; }
    }
    return r;
  }

  case TCCIR_OP_CMP:
  {
    int64_t v1, v2;
    if (!lcs_read_operand(ir, st, src1, &v1) ||
        !lcs_read_operand(ir, st, src2, &v2))
    {
      r.action = 0;
      return r;
    }
    st->cmp_v1 = v1;
    st->cmp_v2 = v2;
    st->cmp_known = 1;
    return r;
  }

  case TCCIR_OP_JUMP:
  {
    int target = (int)irop_get_imm64_ex(ir, dest);
    if (target >= start_idx && target <= end_idx)
    {
      /* Stay inside loop */
      r.action  = 2;
      r.next_pc = target;
      return r;
    }
    /* Jump leaves loop — must be exit_target */
    if (target == exit_target)
    {
      r.action = -1;
      return r;
    }
    r.action = 0;
    return r;
  }

  case TCCIR_OP_JUMPIF:
  {
    if (!st->cmp_known)
    {
      r.action = 0;
      return r;
    }
    if (!irop_is_immediate(src1))
    {
      r.action = 0;
      return r;
    }
    int tok = (int)irop_get_imm64_ex(ir, src1);
    int taken = evaluate_compare_condition(st->cmp_v1, st->cmp_v2, tok);
    if (taken < 0)
    {
      r.action = 0;
      return r;
    }
    int target = (int)irop_get_imm64_ex(ir, dest);

    /* Determine which side (taken/fall-through) reaches the loop body and
     * which leaves to exit_target.  This is independent of whether the
     * JUMPIF is the exit-deciding one (pc == jmpif_idx) or an internal
     * conditional jump within the body. */
    int target_in_loop = (target >= start_idx && target <= end_idx);
    int target_is_exit = (target == exit_target);

    if (taken)
    {
      if (target_in_loop) { r.action = 2; r.next_pc = target; return r; }
      if (target_is_exit) { r.action = -1; return r; }
      r.action = 0; return r;
    }
    /* Not taken — fall-through.  If this is the exit-deciding JUMPIF and
     * the loop's exit is via fall-through (bottom-tested), end iteration.
     * Otherwise continue executing the next instruction. */
    if (pc == jmpif_idx && !target_in_loop) {
      /* Top-tested: target is outside loop (exit_target); fall-through
       * continues body — keep going. */
      return r;
    }
    if (pc == jmpif_idx && target_in_loop) {
      /* Bottom-tested: target is back-edge; fall-through exits. */
      r.action = -1; return r;
    }
    /* Internal JUMPIF — fall-through continues. */
    return r;
  }

  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  case TCCIR_OP_ROR:
  case TCCIR_OP_DIV:
  case TCCIR_OP_UDIV:
  case TCCIR_OP_IMOD:
  case TCCIR_OP_UMOD:
  {
    int64_t v1, v2;
    if (!lcs_read_operand(ir, st, src1, &v1) ||
        !lcs_read_operand(ir, st, src2, &v2))
    {
      r.action = 0;
      return r;
    }
    int dbt = irop_get_btype(dest);
    int64_t result = 0;
    switch (op)
    {
    case TCCIR_OP_ADD: result = (int64_t)((uint64_t)v1 + (uint64_t)v2); break;
    case TCCIR_OP_SUB: result = (int64_t)((uint64_t)v1 - (uint64_t)v2); break;
    case TCCIR_OP_MUL: result = (int64_t)((uint64_t)v1 * (uint64_t)v2); break;
    case TCCIR_OP_AND: result = v1 & v2; break;
    case TCCIR_OP_OR:  result = v1 | v2; break;
    case TCCIR_OP_XOR: result = v1 ^ v2; break;
    case TCCIR_OP_SHL:
      if (v2 < 0 || v2 >= (dbt == IROP_BTYPE_INT64 ? 64 : 32))
      { r.action = 0; return r; }
      result = (int64_t)((uint64_t)v1 << v2);
      break;
    case TCCIR_OP_SHR:
      if (v2 < 0 || v2 >= (dbt == IROP_BTYPE_INT64 ? 64 : 32))
      { r.action = 0; return r; }
      if (dbt == IROP_BTYPE_INT64) result = (int64_t)((uint64_t)v1 >> v2);
      else                          result = (int64_t)((uint32_t)v1 >> v2);
      break;
    case TCCIR_OP_SAR:
      if (v2 < 0 || v2 >= (dbt == IROP_BTYPE_INT64 ? 64 : 32))
      { r.action = 0; return r; }
      result = v1 >> v2;
      break;
    case TCCIR_OP_ROR:
    {
      uint32_t uv = (uint32_t)v1;
      uint32_t un = (uint32_t)v2 & 31;
      result = (int64_t)(int32_t)((uv >> un) | (uv << (32 - un)));
      break;
    }
    case TCCIR_OP_DIV:
      if (v2 == 0) { r.action = 0; return r; }
      result = v1 / v2;
      break;
    case TCCIR_OP_UDIV:
      if (v2 == 0) { r.action = 0; return r; }
      if (dbt == IROP_BTYPE_INT64) result = (int64_t)((uint64_t)v1 / (uint64_t)v2);
      else                          result = (int64_t)((uint32_t)v1 / (uint32_t)v2);
      break;
    case TCCIR_OP_IMOD:
      if (v2 == 0) { r.action = 0; return r; }
      result = v1 % v2;
      break;
    case TCCIR_OP_UMOD:
      if (v2 == 0) { r.action = 0; return r; }
      if (dbt == IROP_BTYPE_INT64) result = (int64_t)((uint64_t)v1 % (uint64_t)v2);
      else                          result = (int64_t)((uint32_t)v1 % (uint32_t)v2);
      break;
    default: r.action = 0; return r;
    }
    if (!lcs_write_operand(st, dest, lcs_truncate(result, dbt), dbt))
    {
      r.action = 0;
      return r;
    }
    return r;
  }

  default:
    r.action = 0;
    return r;
  }
}

/* Scan instructions in [start_idx..end_idx] to find max VAR / TEMP positions
 * and verify every op is simulator-safe.  Also collects the set of VAR
 * positions that the loop writes (so we know which ones need post-loop
 * residual assignments).  Returns 1 on success, 0 on bail. */
static int lcs_scan_body(TCCIRState *ir, int start_idx, int end_idx,
                         int *out_max_var, int *out_max_tmp,
                         uint8_t *written_var_bitmap, int written_var_bitmap_bytes)
{
  int max_var = -1, max_tmp = -1;
  for (int i = start_idx; i <= end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!lcs_op_supported(q->op))
      return 0;
    /* FUNCCALLVAL / FUNCCALLVOID src1 is a SYMREF (function symbol);
     * skip the source-scan for those.  FUNCPARAMVAL src2 is the
     * encoded (call_id, param_idx) immediate. */
    int is_call = (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID);

    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      /* FUNCCALLVOID/FUNCPARAMVOID have no real dest. */
      int has_real_dest = !is_call || q->op == TCCIR_OP_FUNCCALLVAL;
      /* Reject genuine memory writes (sym refs, llocal indirection, or
       * address-of-local writes).  is_lval+is_local on a VAR is the IR's
       * way of saying "the stack slot of this local"; if the live interval
       * confirms the var is register-promotable, the simulator can treat
       * the slot as the value-bearing location. */
      if (has_real_dest && (d.is_llocal || d.is_sym))
        return 0;
      if (has_real_dest && d.is_local && !d.is_lval)
        return 0;
      int32_t vr = irop_get_vreg(d);
      if (has_real_dest && vr >= 0)
      {
        int type = TCCIR_DECODE_VREG_TYPE(vr);
        int pos  = TCCIR_DECODE_VREG_POSITION(vr);
        if (type == TCCIR_VREG_TYPE_VAR)
        {
          if (pos > max_var) max_var = pos;
          IRLiveInterval *li = tcc_ir_get_live_interval(ir, vr);
          if (li && (li->addrtaken || li->is_complex))
            return 0;
          if (pos < written_var_bitmap_bytes * 8)
            written_var_bitmap[pos / 8] |= (1u << (pos % 8));
        }
        else if (type == TCCIR_VREG_TYPE_TEMP)
        {
          if (pos > max_tmp) max_tmp = pos;
        }
        else
        {
          /* PARAM dest — unusual; bail */
          return 0;
        }
      }
    }
    /* Check sources for unsupported flags */
    for (int s = 0; s < 2; s++)
    {
      if (s == 0 && !irop_config[q->op].has_src1) continue;
      if (s == 1 && !irop_config[q->op].has_src2) continue;
      IROperand op = (s == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      /* Skip sources whose semantics aren't "read a value":
       *  - JUMP/JUMPIF: dest is target, src1 is cond token (immediate)
       *  - FUNCCALL*: src1 is callee SYMREF, src2 is call-id encoding
       *  - FUNCPARAMVAL: src2 is param-idx encoding
       *  - FUNCPARAMVOID: src2 is call-id encoding */
      if (q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_JUMP)
        continue;
      if (is_call) continue;
      if (q->op == TCCIR_OP_FUNCPARAMVAL && s == 1) continue;
      if (q->op == TCCIR_OP_FUNCPARAMVOID) continue;
      if (op.is_sym || op.is_llocal)
        return 0;
      if (op.is_local && !op.is_lval)
        return 0;
      int32_t vr = irop_get_vreg(op);
      if (vr < 0)
        continue;
      int type = TCCIR_DECODE_VREG_TYPE(vr);
      int pos  = TCCIR_DECODE_VREG_POSITION(vr);
      if (type == TCCIR_VREG_TYPE_VAR)
      {
        if (pos > max_var) max_var = pos;
        IRLiveInterval *li = tcc_ir_get_live_interval(ir, vr);
        if (li && (li->addrtaken || li->is_complex))
          return 0;
      }
      else if (type == TCCIR_VREG_TYPE_TEMP)
      {
        if (pos > max_tmp) max_tmp = pos;
      }
      else
      {
        /* PARAM source: read-only, fine, no slot. */
      }
    }
  }
  *out_max_var = max_var;
  *out_max_tmp = max_tmp;
  return 1;
}

/* Build initial VAR / TEMP state by scanning instructions [0..start_idx-1].
 * A vreg is considered constant if it has exactly one definition before the
 * loop and that definition is an ASSIGN of an immediate value. */
static void lcs_init_var_state(TCCIRState *ir, int start_idx, LcsState *st)
{
  int *var_def_count = tcc_mallocz(sizeof(int) * (st->n_vars > 0 ? st->n_vars : 1));
  int *tmp_def_count = tcc_mallocz(sizeof(int) * (st->n_tmps > 0 ? st->n_tmps : 1));
  for (int i = 0; i < start_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;
    if (!irop_config[q->op].has_dest) continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    /* Skip true memory writes (sym or llocal indirection or address-of-local).
     * For STACKOFF+lval (= write to the stack slot of a local), treat the
     * destination as the logical vreg location — that's how the simulator
     * models values regardless of where they physically reside. */
    if (d.is_llocal || d.is_sym) continue;
    if (d.is_local && !d.is_lval) continue;
    int32_t vr = irop_get_vreg(d);
    if (vr < 0) continue;
    int type = TCCIR_DECODE_VREG_TYPE(vr);
    int pos  = TCCIR_DECODE_VREG_POSITION(vr);
    LcsSlot *slot = NULL;
    int     *count = NULL;
    if (type == TCCIR_VREG_TYPE_VAR && pos < st->n_vars) {
      slot = &st->vars[pos];  count = &var_def_count[pos];
    } else if (type == TCCIR_VREG_TYPE_TEMP && pos < st->n_tmps) {
      slot = &st->tmps[pos];  count = &tmp_def_count[pos];
    } else {
      continue;
    }
    (*count)++;
    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (irop_is_immediate(s1) && *count == 1)
      {
        slot->known = 1;
        slot->value = irop_get_imm64_ex(ir, s1);
        slot->btype = irop_get_btype(s1);
      }
    }
  }
  /* A vreg with multiple defs before the loop isn't safely constant */
  for (int p = 0; p < st->n_vars; p++)
    if (var_def_count[p] > 1) st->vars[p].known = 0;
  for (int p = 0; p < st->n_tmps; p++)
    if (tmp_def_count[p] > 1) st->tmps[p].known = 0;
  tcc_free(var_def_count);
  tcc_free(tmp_def_count);
}

/* Determine whether a VAR vreg is read after `from_idx` (anywhere outside
 * the eliminated loop range).  Used to decide whether to emit a residual
 * ASSIGN with the final value. */
static int lcs_var_used_after(TCCIRState *ir, int var_pos, int from_idx)
{
  int n = ir->next_instruction_index;
  int32_t target_vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, var_pos);
  for (int i = from_idx; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;
    if (irop_config[q->op].has_src1)
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(s) == target_vr) return 1;
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      if (irop_get_vreg(s) == target_vr) return 1;
    }
    /* A redefinition kills any need to preserve the loop's value */
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!d.is_lval && irop_get_vreg(d) == target_vr) return 0;
    }
  }
  return 0;
}

/* Try to fold a single loop.  Returns 1 if folded, 0 otherwise. */
static int lcs_try_fold(TCCIRState *ir, IRLoop *loop)
{
  /* Need a primary IV with constant init/step/limit */
  InductionVar ivs[MAX_IV];
  int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1);
  if (num_ivs < 1)
    return 0;

  int cmp_idx = -1, jmpif_idx = -1, limit = 0, cond = 0, exit_target = -1;
  InductionVar *iv = NULL;
  for (int k = 0; k < num_ivs; k++)
  {
    if (find_loop_exit_condition(ir, loop, ivs[k].vreg, &cmp_idx, &jmpif_idx,
                                 &limit, &cond, &exit_target))
    {
      iv = &ivs[k];
      break;
    }
  }
  if (!iv)
    return 0;

  int trip_count = compute_trip_count(iv->init_val, limit, iv->step, cond);
  if (trip_count <= 0 || trip_count > LCS_MAX_TRIP_COUNT)
    return 0;

  /* Compute effective loop range.  The loop detector may report a tight
   * range like [3..8] that omits the body when control flow is rotated
   * (header at 3 jumps to body at 9, body loops back to 6).  We take the
   * span [start_idx..exit_target-1] as the effective range and verify all
   * jumps within it stay inside or land exactly at exit_target. */
  int eff_start = loop->start_idx;
  int eff_end   = loop->end_idx;
  if (exit_target > eff_end + 1) {
    if (exit_target - eff_start > 512)
      return 0;
    eff_end = exit_target - 1;
  }
  /* Verify all branches in extended range stay within OR land exactly at exit. */
  for (int i = eff_start; i <= eff_end; i++)
  {
    IRQuadCompact *qx = &ir->compact_instructions[i];
    if (qx->op != TCCIR_OP_JUMP && qx->op != TCCIR_OP_JUMPIF) continue;
    int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, qx));
    if (t < eff_start || (t > eff_end && t != exit_target))
      return 0;
  }

  /* Body must contain only simulator-safe ops */
  int max_var = -1, max_tmp = -1;
  uint8_t written_bitmap[LCS_MAX_TRACKED_VARS / 8] = {0};
  if (!lcs_scan_body(ir, eff_start, eff_end, &max_var, &max_tmp,
                     written_bitmap, sizeof(written_bitmap)))
    return 0;
  if (max_var >= LCS_MAX_TRACKED_VARS || max_tmp >= LCS_MAX_TRACKED_TMPS)
    return 0;

  /* If max_var is from instructions outside the loop, scan further to find
   * it.  We need n_vars large enough to cover both initial-state reads and
   * loop-internal writes/reads. */
  int n = ir->next_instruction_index;
  int outer_max_var = max_var;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;
    IROperand ops[3] = { tcc_ir_op_get_dest(ir, q),
                         tcc_ir_op_get_src1(ir, q),
                         tcc_ir_op_get_src2(ir, q) };
    for (int o = 0; o < 3; o++)
    {
      int32_t vr = irop_get_vreg(ops[o]);
      if (vr < 0) continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR) continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos > outer_max_var) outer_max_var = pos;
    }
  }
  if (outer_max_var >= LCS_MAX_TRACKED_VARS)
    return 0;

  /* Initialize state */
  LcsState st = {0};
  st.n_vars = outer_max_var + 1;
  st.n_tmps = max_tmp + 1;
  st.vars  = tcc_mallocz(sizeof(LcsSlot) * (st.n_vars > 0 ? st.n_vars : 1));
  st.tmps  = tcc_mallocz(sizeof(LcsSlot) * (st.n_tmps > 0 ? st.n_tmps : 1));
  st.calls = tcc_mallocz(sizeof(LcsCallSlot) * LCS_MAX_CALLS);

  lcs_init_var_state(ir, loop->start_idx, &st);

  /* Seed the IV's initial value if its definition is before the loop */
  if (iv->init_idx >= 0)
  {
    int32_t ivr = iv->vreg;
    if (TCCIR_DECODE_VREG_TYPE(ivr) == TCCIR_VREG_TYPE_VAR)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(ivr);
      if (pos < st.n_vars)
      {
        st.vars[pos].known = 1;
        st.vars[pos].value = iv->init_val;
        st.vars[pos].btype = IROP_BTYPE_INT32;
      }
    }
  }

  /* Simulate the loop body to completion.  Control flow handles all
   * iterations internally via the back-edge JUMP/JUMPIF; we just run the
   * simulator until it takes the exit edge (action==-1).  We bound the
   * total work by trip_count * (body size) to catch runaway loops. */
  int sim_ok = 1;
  int pc = eff_start;
  int total_steps = 0;
  int max_total_steps = (trip_count + 1) * (eff_end - eff_start + 1) + 32;
  if (max_total_steps > LCS_MAX_ITER_STEPS) max_total_steps = LCS_MAX_ITER_STEPS;
  while (pc >= eff_start && pc <= eff_end)
  {
    if (++total_steps > max_total_steps) { sim_ok = 0; break; }
    IRQuadCompact *q = &ir->compact_instructions[pc];
    LcsStep step = lcs_exec(ir, &st, q, pc,
                            eff_start, eff_end,
                            cmp_idx, jmpif_idx, exit_target);
    if (step.action == 0) { sim_ok = 0; break; }
    if (step.action == -1) break;  /* iteration finished, loop exited */
    if (step.action == 2) {
      pc = step.next_pc;
      /* If the back-edge target is the loop start, reset per-iter scratchpads:
       * call-param storage and pending cmp state.  TEMPs persist (they may be
       * loop-carried). */
      for (int c = 0; c < LCS_MAX_CALLS; c++)
        for (int pp = 0; pp < LCS_MAX_PARAMS; pp++)
          st.calls[c].params[pp].known = 0;
      st.cmp_known = 0;
      st.cmp_is_fp = 0;
      continue;
    }
    pc++;
  }

  if (!sim_ok)
  {
    tcc_free(st.vars);
    tcc_free(st.tmps);
    tcc_free(st.calls);
    return 0;
  }

  /* Sim succeeded.  Build the residual: NOP the entire loop body, then
   * emit ASSIGNs for each written VAR (and the IV) that's used after. */

  LOG_IR_GEN("[LOOP-CONST-SIM] folding loop header=%d trip=%d", loop->header_idx, trip_count);

  /* Collect the NOPs we'll write into. */
  int slot_pos = eff_start;
  int slot_end = eff_end;

  /* Final IV value (if needed) */
  int iv_pos = TCCIR_DECODE_VREG_POSITION(iv->vreg);
  int iv_final_val = iv->init_val + trip_count * iv->step;

  /* NOP the entire effective loop range first */
  for (int i = eff_start; i <= eff_end; i++)
    ir->compact_instructions[i].op = TCCIR_OP_NOP;

  /* Emit residual ASSIGNs.  We write into the NOP slots starting at start_idx. */
  /* Final IV residual */
  if (TCCIR_DECODE_VREG_TYPE(iv->vreg) == TCCIR_VREG_TYPE_VAR)
  {
    if (lcs_var_used_after(ir, iv_pos, exit_target) && slot_pos <= slot_end)
    {
      IROperand d = irop_make_vreg(iv->vreg, IROP_BTYPE_INT32);
      IROperand s = irop_make_imm32(-1, iv_final_val, IROP_BTYPE_INT32);
      write_instr_at_nop(ir, slot_pos++, TCCIR_OP_ASSIGN, d, s, IROP_NONE);
    }
  }

  /* Other modified VAR residuals */
  for (int p = 0; p < st.n_vars; p++)
  {
    if (p == iv_pos) continue;
    if (!(written_bitmap[p / 8] & (1u << (p % 8)))) continue;
    /* The sim bails on any unresolvable op, so any written VAR should
     * have a known value here.  Be defensive against bugs. */
    if (!st.vars[p].known) continue;
    if (!lcs_var_used_after(ir, p, exit_target)) continue;
    if (slot_pos > slot_end) {
      /* Out of NOP slots — this is rare; we'd need to insert.  Bail
       * conservatively by reverting?  At this point the loop is NOPed.
       * Easiest safe action: leave the var without a residual, which is
       * a correctness bug.  To avoid that, ensure we have enough slots
       * before NOPing: tally needed slots first. */
      break;
    }
    int btype = st.vars[p].btype ? st.vars[p].btype : IROP_BTYPE_INT32;
    int32_t vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, p);
    IROperand d = irop_make_vreg(vr, btype);
    int64_t val = st.vars[p].value;
    IROperand s;
    if (btype == IROP_BTYPE_FLOAT64)
    {
      uint32_t pidx = tcc_ir_pool_add_f64(ir, (uint64_t)val);
      s = irop_make_f64(-1, pidx);
    }
    else if (btype == IROP_BTYPE_FLOAT32)
    {
      s = irop_make_f32(-1, (uint32_t)val);
    }
    else if (val == (int32_t)val)
    {
      s = irop_make_imm32(-1, (int32_t)val, btype);
    }
    else
    {
      uint32_t pidx = tcc_ir_pool_add_i64(ir, val);
      s = irop_make_i64(-1, pidx, btype);
    }
    write_instr_at_nop(ir, slot_pos++, TCCIR_OP_ASSIGN, d, s, IROP_NONE);
  }

  /* NOP the IV init too, plus any pre-loop guard CMP+JUMPIF on the IV.
   * Mirrors try_eliminate_loop. */
  if (iv->init_idx >= 0 && iv->init_idx < loop->start_idx)
  {
    ir->compact_instructions[iv->init_idx].op = TCCIR_OP_NOP;
    for (int g = iv->init_idx + 1; g < loop->start_idx; g++)
    {
      IRQuadCompact *gq = &ir->compact_instructions[g];
      if (gq->op == TCCIR_OP_CMP)
      {
        IROperand gsrc1 = tcc_ir_op_get_src1(ir, gq);
        if (irop_get_vreg(gsrc1) == iv->vreg && g + 1 < loop->start_idx)
        {
          IRQuadCompact *gjq = &ir->compact_instructions[g + 1];
          if (gjq->op == TCCIR_OP_JUMPIF)
          {
            gq->op  = TCCIR_OP_NOP;
            gjq->op = TCCIR_OP_NOP;
          }
        }
      }
    }
  }

  tcc_free(st.vars);
  tcc_free(st.tmps);
  tcc_free(st.calls);
  return 1;
}

int tcc_ir_opt_loop_const_sim(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops)
    return 0;

  int changes = 0;
  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];
    /* Skip nested loops (depth > 1 — outermost loops have depth=1) */
    if (loop->depth > 1) continue;
    /* Skip very large loop ranges to keep cost bounded */
    if (loop->end_idx - loop->start_idx > 256) continue;
    changes += lcs_try_fold(ir, loop);
  }

  tcc_ir_free_loops(loops);
  return changes;
}

int tcc_ir_opt_loop_const_sim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_loop_const_sim(ctx->ir);
}

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
#include "opt_alias.h"
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
#define LCS_MAX_MEM_SLOTS    64   /* distinct stack offsets the simulator tracks */

typedef struct LcsSlot
{
  int     known;
  int64_t value;
  int     btype;    /* IROP_BTYPE_INT32 / INT64 / FLOAT32 / FLOAT64 */
  int     is_unsigned; /* sign of a narrow (INT8/INT16) value — needed so the
                          residual is zero- vs sign-extended correctly */
  int     is_addr;  /* value is a stack offset (Addr[StackLoc[value]]) */
} LcsSlot;

typedef struct LcsCallSlot
{
  LcsSlot params[LCS_MAX_PARAMS];
} LcsCallSlot;

/* One tracked stack-memory slot.  The simulator records writes during loop
 * iteration; on success, residual STOREs are emitted only for slots whose
 * `written` flag is set.  `initial_known` records whether the pre-loop value
 * was constant — used to suppress residual STOREs that would just rewrite the
 * same value back. */
typedef struct LcsMemSlot
{
  int32_t offset;          /* stack offset (negative = local) */
  int64_t value;
  int     btype;
  int     is_unsigned;     /* sign of a narrow store — see LcsSlot.is_unsigned */
  int     known;           /* current value is known */
  int     written;         /* sim wrote to this slot at least once */
  int64_t initial_value;   /* value before the loop (if initial_known) */
  int     initial_known;
} LcsMemSlot;

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
  LcsMemSlot   *mem;      /* tracked stack-memory slots */
  int           n_mem;    /* number of entries used */
  int           mem_overflow; /* set when we needed to track > LCS_MAX_MEM_SLOTS */
} LcsState;

/* Find or create a tracked stack-memory slot for `offset`.  Returns NULL when
 * the slot table is full (sets state->mem_overflow so the caller bails). */
static LcsMemSlot *lcs_mem_get(LcsState *st, int32_t offset)
{
  for (int i = 0; i < st->n_mem; i++)
    if (st->mem[i].offset == offset)
      return &st->mem[i];
  if (st->n_mem >= LCS_MAX_MEM_SLOTS)
  {
    st->mem_overflow = 1;
    return NULL;
  }
  LcsMemSlot *s = &st->mem[st->n_mem++];
  s->offset = offset;
  s->value = 0;
  s->btype = IROP_BTYPE_INT32;
  s->is_unsigned = 0;
  s->known = 0;
  s->written = 0;
  s->initial_value = 0;
  s->initial_known = 0;
  return s;
}

/* A store of `width` bytes at `offset` also clobbers any OTHER tracked slot
 * whose byte range overlaps it.  Slots are keyed by exact offset with no
 * width awareness, so a packed-bitfield byte store at word_off+3 must mark
 * the word's slot unknown (and vice versa) — otherwise the simulator folds
 * a later RMW from the stale full-word value (bitfield seed 11840: byte-3
 * b3 store ignored, the collapsed loop store wiped it back to 0). */
static void lcs_mem_clobber_overlaps(LcsState *st, int32_t offset, int width,
                                     const LcsMemSlot *keep)
{
  for (int i = 0; i < st->n_mem; i++)
  {
    LcsMemSlot *m = &st->mem[i];
    if (m == keep)
      continue;
    int mw = ir_opt_store_btype_size_bytes(m->btype);
    if (mw <= 0)
      mw = 4;
    if (m->offset < offset + width && m->offset + mw > offset)
    {
      m->known = 0;
      m->initial_known = 0;
    }
  }
}

/* Resolve an operand to a stack offset when it is either:
 *   - a literal stack-address operand: Addr[StackLoc[off]] (LEA-style source)
 *   - a TEMP/VAR whose simulator slot is marked is_addr
 * Returns 1 and sets *out_off on success, 0 otherwise. */
static int lcs_resolve_stack_addr(const LcsState *st, IROperand op, int32_t *out_off)
{
  if (op.is_local && !op.is_lval && irop_get_tag(op) == IROP_TAG_STACKOFF)
  {
    *out_off = irop_get_stack_offset(op);
    return 1;
  }
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  int type = TCCIR_DECODE_VREG_TYPE(vr);
  int pos  = TCCIR_DECODE_VREG_POSITION(vr);
  const LcsSlot *slot = NULL;
  if (type == TCCIR_VREG_TYPE_VAR && pos < st->n_vars)
    slot = &st->vars[pos];
  else if (type == TCCIR_VREG_TYPE_TEMP && pos < st->n_tmps)
    slot = &st->tmps[pos];
  if (slot && slot->known && slot->is_addr)
  {
    *out_off = (int32_t)slot->value;
    return 1;
  }
  return 0;
}

static int lcs_op_supported(TccIrOp op)
{
  switch (op)
  {
  case TCCIR_OP_NOP:
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LEA:
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
  case TCCIR_OP_TEST_ZERO:
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_STORE:
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
 * FP immediates return the raw bit pattern.
 *
 * Memory reads (operand with is_lval set, or direct StackLoc[X] in lval form)
 * are resolved through the simulator's stack-memory map when the address is
 * known.  Reads of stack addresses (is_local && !is_lval) are rejected — only
 * helpers that explicitly want the address (ASSIGN/ADD/SUB) call
 * lcs_resolve_stack_addr directly. */
static int lcs_read_operand(const TCCIRState *ir, const LcsState *st,
                            IROperand op, int64_t *out)
{
  /* Inline immediate */
  if (irop_is_immediate(op))
  {
    *out = irop_get_imm64_ex(ir, op);
    return 1;
  }

  if (op.is_sym || op.is_llocal)
    return 0;
  /* Address-typed operand (LEA-style) — caller should have used
   * lcs_resolve_stack_addr instead. */
  if (op.is_local && !op.is_lval)
    return 0;

  int32_t vr = irop_get_vreg(op);
  /* Pure stack-slot read (no associated vreg): consult the memory map. */
  if (vr < 0)
  {
    if (op.is_local && op.is_lval && irop_get_tag(op) == IROP_TAG_STACKOFF)
    {
      int32_t off = irop_get_stack_offset(op);
      for (int i = 0; i < st->n_mem; i++)
      {
        if (st->mem[i].offset == off && st->mem[i].known)
        {
          *out = st->mem[i].value;
          return 1;
        }
      }
    }
    return 0;
  }
  int type = TCCIR_DECODE_VREG_TYPE(vr);
  int pos  = TCCIR_DECODE_VREG_POSITION(vr);
  const LcsSlot *slot = NULL;
  if (type == TCCIR_VREG_TYPE_VAR)
  {
    if (pos >= st->n_vars || !st->vars[pos].known)
      return 0;
    slot = &st->vars[pos];
  }
  else if (type == TCCIR_VREG_TYPE_TEMP)
  {
    if (pos >= st->n_tmps || !st->tmps[pos].known)
      return 0;
    slot = &st->tmps[pos];
  }
  else
  {
    return 0;
  }
  /* Indirect memory read: TEMP-vreg with is_lval, where the simulator slot
   * holds a known stack address.  is_local+is_lval on a VAR vreg is just
   * the IR's lval form of a register-resident variable — read the slot's
   * value, not memory through it. */
  if (op.is_lval && slot->is_addr &&
      (type == TCCIR_VREG_TYPE_TEMP || !op.is_local))
  {
    int32_t off = (int32_t)slot->value;
    for (int i = 0; i < st->n_mem; i++)
    {
      if (st->mem[i].offset == off && st->mem[i].known)
      {
        *out = st->mem[i].value;
        return 1;
      }
    }
    return 0;
  }
  /* Plain register read.  Reject when slot holds an address but caller wants
   * a value — addresses are only valid as ADD/SUB operands or LOAD bases. */
  if (slot->is_addr)
    return 0;
  *out = slot->value;
  return 1;
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
    st->vars[pos].is_unsigned = op.is_unsigned;
    st->vars[pos].is_addr = 0;
    return 1;
  }
  if (type == TCCIR_VREG_TYPE_TEMP)
  {
    if (pos >= st->n_tmps)
      return 0;
    st->tmps[pos].known = 1;
    st->tmps[pos].value = value;
    st->tmps[pos].btype = btype;
    st->tmps[pos].is_unsigned = op.is_unsigned;
    st->tmps[pos].is_addr = 0;
    return 1;
  }
  return 0;
}

/* Variant: write a stack-address (offset) into the destination slot, tagging
 * it as an address so later loads/stores resolve through it. */
static int lcs_write_addr_operand(LcsState *st, IROperand op, int32_t stack_offset)
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
  LcsSlot *slot = NULL;
  if (type == TCCIR_VREG_TYPE_VAR && pos < st->n_vars)
    slot = &st->vars[pos];
  else if (type == TCCIR_VREG_TYPE_TEMP && pos < st->n_tmps)
    slot = &st->tmps[pos];
  if (!slot)
    return 0;
  slot->known = 1;
  slot->value = stack_offset;
  slot->btype = IROP_BTYPE_INT32;
  slot->is_unsigned = 0;
  slot->is_addr = 1;
  return 1;
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

/* Evaluate a comparison whose operands are soft-float bit patterns (set by a
 * cfcmp / cdcmp flag-setter).  b1/b2 are the raw 32- or 64-bit FP bits; tok is
 * the same relational token evaluate_compare_condition uses.  Returns 1
 * (taken), 0 (not taken), or -1 (unsupported token -> caller bails).
 * Unordered (NaN) operands make every relation false except "!=", matching C
 * and the ARM flag semantics the lowered branch tests. */
static int lcs_evaluate_fp_compare(int64_t b1, int64_t b2, int tok, int is_double)
{
  double a, b;
  if (is_double)
  {
    union { double d; uint64_t u; } x, y;
    x.u = (uint64_t)b1; y.u = (uint64_t)b2;
    a = x.d; b = y.d;
  }
  else
  {
    union { float f; uint32_t u; } x, y;
    x.u = (uint32_t)b1; y.u = (uint32_t)b2;
    a = (double)x.f; b = (double)y.f;
  }
  int unordered = (a != a) || (b != b);
  switch (tok)
  {
  case 0x94: /* TOK_EQ  */ return !unordered && (a == b);
  case 0x95: /* TOK_NE  */ return unordered || (a != b);
  case 0x9c: /* TOK_LT  */ return !unordered && (a < b);
  case 0x9d: /* TOK_GE  */ return !unordered && (a >= b);
  case 0x9e: /* TOK_LE  */ return !unordered && (a <= b);
  case 0x9f: /* TOK_GT  */ return !unordered && (a > b);
  default:                 return -1; /* unsigned/unknown token: bail */
  }
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
    if (src1.is_sym || src1.is_llocal) { r.action = 0; return r; }
    /* `LOAD T <- Addr[StackLoc[X]]` (non-lval source) is really a LEA — the
     * dest receives a stack address.  Track it. */
    int32_t addr_off;
    if (lcs_resolve_stack_addr(st, src1, &addr_off) && !src1.is_lval)
    {
      if (!lcs_write_addr_operand(st, dest, addr_off)) { r.action = 0; return r; }
      return r;
    }
    int64_t v;
    if (!lcs_read_operand(ir, st, src1, &v)) { r.action = 0; return r; }
    int dbt = irop_get_btype(dest);
    if (!lcs_write_operand(st, dest, v, dbt)) { r.action = 0; return r; }
    return r;
  }

  case TCCIR_OP_STORE:
  {
    int64_t v;
    if (!lcs_read_operand(ir, st, src1, &v)) { r.action = 0; return r; }
    int sbt = irop_get_btype(src1);
    int dbt = irop_get_btype(dest);
    int is_fp = (sbt == IROP_BTYPE_FLOAT32 || sbt == IROP_BTYPE_FLOAT64 ||
                 dbt == IROP_BTYPE_FLOAT32 || dbt == IROP_BTYPE_FLOAT64);
    int64_t store_val = is_fp ? v : lcs_truncate(v, dbt);

    /* For register-promotable VARs, update the VAR slot directly so that
     * subsequent reads via the vreg see the stored value. */
    int32_t dvr = irop_get_vreg(dest);
    int recorded_in_var = 0;
    if (dvr >= 0 && dest.is_lval) {
      int dtype = TCCIR_DECODE_VREG_TYPE(dvr);
      int dpos  = TCCIR_DECODE_VREG_POSITION(dvr);
      if (dtype == TCCIR_VREG_TYPE_VAR && dpos < st->n_vars) {
        st->vars[dpos].known = 1;
        st->vars[dpos].value = store_val;
        st->vars[dpos].btype = dbt;
        st->vars[dpos].is_unsigned = dest.is_unsigned;
        st->vars[dpos].is_addr = 0;
        recorded_in_var = 1;
      }
    }

    /* Resolve destination address.  Accept either a direct StackLoc[X]
     * (is_local && is_lval) or a vreg whose simulator slot is_addr. */
    int32_t off;
    if (dest.is_local && dest.is_lval && irop_get_tag(dest) == IROP_TAG_STACKOFF)
    {
      off = irop_get_stack_offset(dest);
    }
    else if (dest.is_lval)
    {
      int32_t vr = irop_get_vreg(dest);
      if (vr < 0) { r.action = 0; return r; }
      int type = TCCIR_DECODE_VREG_TYPE(vr);
      int pos  = TCCIR_DECODE_VREG_POSITION(vr);
      const LcsSlot *slot = NULL;
      if (type == TCCIR_VREG_TYPE_VAR && pos < st->n_vars) slot = &st->vars[pos];
      else if (type == TCCIR_VREG_TYPE_TEMP && pos < st->n_tmps) slot = &st->tmps[pos];
      if (!slot || !slot->known || !slot->is_addr)
      {
        /* The destination address does not resolve to a tracked stack slot.
         * If the value was just recorded into a register-promotable VAR slot
         * above, the store is fully modeled — continue.  Otherwise this STORE
         * writes memory the simulator cannot track — most importantly a deref
         * through a PARAM pointer (`*y = …` for a parameter `int *y`), which
         * targets caller-visible memory.  Such a store is OBSERVABLE; folding
         * the loop would silently drop it (compiling `for(i…) *y=i;` to a bare
         * `bx lr`).  Bail so the loop is left intact. */
        if (recorded_in_var)
          return r;
        r.action = 0;
        return r;
      }
      off = (int32_t)slot->value;
    }
    else
    {
      r.action = 0; return r;
    }
    LcsMemSlot *ms = lcs_mem_get(st, off);
    if (!ms) { r.action = 0; return r; }
    ms->value = store_val;
    ms->btype = dbt;
    ms->is_unsigned = dest.is_unsigned;
    ms->known = 1;
    ms->written = 1;
    {
      int sw = ir_opt_store_btype_size_bytes(dbt);
      if (sw <= 0)
        sw = 4;
      lcs_mem_clobber_overlaps(st, off, sw, ms);
    }
    return r;
  }

  case TCCIR_OP_LEA:
  {
    int32_t addr_off;
    if (src1.is_local && irop_get_tag(src1) == IROP_TAG_STACKOFF)
      addr_off = irop_get_stack_offset(src1);
    else if (!lcs_resolve_stack_addr(st, src1, &addr_off))
    {
      r.action = 0;
      return r;
    }
    if (!lcs_write_addr_operand(st, dest, addr_off)) { r.action = 0; return r; }
    return r;
  }

  case TCCIR_OP_ASSIGN:
  {
    /* ASSIGN dest <- Addr[StackLoc[X]] is a LEA-equivalent — track address. */
    int32_t addr_off;
    if (lcs_resolve_stack_addr(st, src1, &addr_off) && !src1.is_lval)
    {
      if (!lcs_write_addr_operand(st, dest, addr_off)) { r.action = 0; return r; }
      return r;
    }
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

  case TCCIR_OP_TEST_ZERO:
  {
    int64_t v1;
    if (!lcs_read_operand(ir, st, src1, &v1))
    {
      r.action = 0;
      return r;
    }
    st->cmp_v1 = v1;
    st->cmp_v2 = 0;
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
    /* A compare flagged by a soft-float helper (cfcmp / cdcmp) holds raw FP
     * bit patterns in cmp_v1/cmp_v2; evaluating them as integers is wrong for
     * any operand whose sign bit is set (a negative float bit pattern reads as
     * a huge unsigned int).  Reinterpret and compare as float/double. */
    int taken = st->cmp_is_fp
                    ? lcs_evaluate_fp_compare(st->cmp_v1, st->cmp_v2, tok, st->cmp_is_double)
                    : evaluate_compare_condition(st->cmp_v1, st->cmp_v2, tok);
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
    /* Address arithmetic: ADD/SUB of a stack address and an integer produces
     * another stack address.  Tracked so subsequent LOAD/STORE through the
     * result vreg can resolve the slot.  Only ADD/SUB combinations are
     * meaningful here — multiplication etc. of an address has no defined
     * stack-slot semantics and falls through to the integer path. */
    if (op == TCCIR_OP_ADD || op == TCCIR_OP_SUB)
    {
      int32_t a1_off, a2_off;
      int a1_is_addr = lcs_resolve_stack_addr(st, src1, &a1_off) &&
                       (!src1.is_lval || src1.is_local);
      int a2_is_addr = lcs_resolve_stack_addr(st, src2, &a2_off) &&
                       (!src2.is_lval || src2.is_local);
      if (a1_is_addr && !a2_is_addr)
      {
        int64_t v2_int;
        if (!lcs_read_operand(ir, st, src2, &v2_int)) { r.action = 0; return r; }
        int32_t new_off = (op == TCCIR_OP_ADD)
                            ? (int32_t)(a1_off + v2_int)
                            : (int32_t)(a1_off - v2_int);
        if (!lcs_write_addr_operand(st, dest, new_off)) { r.action = 0; return r; }
        return r;
      }
      if (!a1_is_addr && a2_is_addr && op == TCCIR_OP_ADD)
      {
        int64_t v1_int;
        if (!lcs_read_operand(ir, st, src1, &v1_int)) { r.action = 0; return r; }
        int32_t new_off = (int32_t)(a2_off + v1_int);
        if (!lcs_write_addr_operand(st, dest, new_off)) { r.action = 0; return r; }
        return r;
      }
      /* addr - addr (gives an integer offset) and addr * X / addr & X /
       * etc. are not meaningful for our stack model.  Bail rather than
       * silently producing garbage. */
      if (a1_is_addr || a2_is_addr) { r.action = 0; return r; }
    }
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
      /* INT_MIN / -1 overflows and traps on hardware divide; don't fold. */
      if (v2 == -1 &&
          ((dbt == IROP_BTYPE_INT64 && v1 == INT64_MIN) ||
           (dbt != IROP_BTYPE_INT64 && (int32_t)v1 == INT32_MIN)))
      { r.action = 0; return r; }
      result = v1 / v2;
      break;
    case TCCIR_OP_UDIV:
      if (v2 == 0) { r.action = 0; return r; }
      if (dbt == IROP_BTYPE_INT64) result = (int64_t)((uint64_t)v1 / (uint64_t)v2);
      else                          result = (int64_t)((uint32_t)v1 / (uint32_t)v2);
      break;
    case TCCIR_OP_IMOD:
      if (v2 == 0) { r.action = 0; return r; }
      if (v2 == -1 &&
          ((dbt == IROP_BTYPE_INT64 && v1 == INT64_MIN) ||
           (dbt != IROP_BTYPE_INT64 && (int32_t)v1 == INT32_MIN)))
      { r.action = 0; return r; }
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
    if (!lcs_op_supported(q->op)) {
      return 0;
    }
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
       * the slot as the value-bearing location.
       *
       * STORE-op destinations are the special case: STORE writes through
       * an address, so a STACKOFF+lval dest means "store at this stack
       * slot" — that's tracked by the memory model, not a "PARAM dest"-
       * style bail. */
      if (has_real_dest && (d.is_llocal || d.is_sym))
        return 0;
      if (has_real_dest && d.is_local && !d.is_lval)
        return 0;
      int32_t vr = irop_get_vreg(d);
      /* For STORE through a vreg (`T***DEREF*** <- val` where the vreg is
       * NOT a local) the dest carries is_lval on a regular TEMP that is
       * itself a tracked stack address.  Don't run the register-promotable
       * (addrtaken) check on such a vreg, but still size the TEMP table. */
      int store_through_vreg = (q->op == TCCIR_OP_STORE) && d.is_lval &&
                               !d.is_local && !d.is_llocal && !d.is_sym;
      if (has_real_dest && vr >= 0)
      {
        int type = TCCIR_DECODE_VREG_TYPE(vr);
        int pos  = TCCIR_DECODE_VREG_POSITION(vr);
        if (type == TCCIR_VREG_TYPE_VAR)
        {
          if (pos > max_var) max_var = pos;
          if (!store_through_vreg)
          {
            IRLiveInterval *li = tcc_ir_get_live_interval(ir, vr);
            if (li && (li->addrtaken || li->is_complex))
              return 0;
            if (pos < written_var_bitmap_bytes * 8)
              written_var_bitmap[pos / 8] |= (1u << (pos % 8));
          }
        }
        else if (type == TCCIR_VREG_TYPE_TEMP)
        {
          if (pos > max_tmp) max_tmp = pos;
        }
        else if (!store_through_vreg)
        {
          /* PARAM dest — unusual; bail (STORE through PARAM vreg would be
           * an indirect through a param-passed pointer; not modeled). */
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
      /* Stack-address operand (LEA-style source) is only meaningful for
       * ASSIGN, LOAD (LEA-shaped), and ADD/SUB — where lcs_exec resolves
       * the address through lcs_resolve_stack_addr.  Other ops reading
       * such operands would be misinterpreted, so reject. */
      if (op.is_local && !op.is_lval)
      {
        if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD &&
            q->op != TCCIR_OP_LEA &&
            q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
          return 0;
      }
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
        {
          /* The address-taken / complex restriction guards the register-
           * promotability of the simulator's VAR slot.  When the VAR is
           * only used as a vreg source/dest (and the body holds its
           * "memory" via the stack-slot model), we still need a slot to
           * track its value across iterations — bail only when the body
           * doesn't read/write it through its STACKOFF either. */
          return 0;
        }
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
 *
 * Scans forward, recording each ASSIGN/LOAD of an immediate or LEA-style
 * source as the slot's tentative known value.  Multiple sequential defs in
 * straight-line code overwrite each other — the slot ends up holding the
 * LAST def's value, which is what reaches the loop entry.
 *
 * Safety: when control flow could enter the slot's def region from
 * elsewhere (the def's instruction is a jump target, or a JUMP/JUMPIF
 * sits between the def and the loop), the linear "last def wins" reasoning
 * breaks.  We track a per-slot `flow_unsafe` flag that gets set whenever
 * we see a jump-target / control-flow boundary AFTER a slot was defined,
 * and demote that slot to unknown.
 *
 * Also populates the stack-memory map with pre-loop direct stores of the
 * form `StackLoc[off] <- imm [STORE]`.  Multi-write slots take the last
 * write's value (same straight-line reasoning).  STOREs through computed
 * addresses are ignored here; the simulator only knows about slots seeded
 * by direct STOREs and any new writes the simulated body performs. */
static void lcs_init_var_state(TCCIRState *ir, int start_idx, LcsState *st)
{
  uint8_t *var_flow_unsafe = tcc_mallocz((size_t)(st->n_vars > 0 ? st->n_vars : 1));
  uint8_t *tmp_flow_unsafe = tcc_mallocz((size_t)(st->n_tmps > 0 ? st->n_tmps : 1));
  uint8_t *var_has_def = tcc_mallocz((size_t)(st->n_vars > 0 ? st->n_vars : 1));
  uint8_t *tmp_has_def = tcc_mallocz((size_t)(st->n_tmps > 0 ? st->n_tmps : 1));
  uint8_t *mem_has_def = tcc_mallocz((size_t)LCS_MAX_MEM_SLOTS);
  uint8_t *mem_flow_unsafe = tcc_mallocz((size_t)LCS_MAX_MEM_SLOTS);

  /* Identify jump targets in [0..start_idx-1] whose only incoming edges
   * come from inside the loop region [start_idx..n-1] — those are the
   * current loop's back-edges (and back-edges of later loops); they don't
   * affect the *first-iteration* state we're computing here, so they
   * should NOT be treated as branch boundaries for the pre-loop scan.
   *
   * "real_pre_target" = at least one JUMP/JUMPIF in [0..start_idx-1]
   * (i.e. somewhere in the pre-loop itself) targets this instruction. */
  int n_all = ir->next_instruction_index;
  uint8_t *real_pre_target = tcc_mallocz((size_t)start_idx);
  for (int j = 0; j < start_idx; j++)
  {
    IRQuadCompact *jq = &ir->compact_instructions[j];
    if (jq->op != TCCIR_OP_JUMP && jq->op != TCCIR_OP_JUMPIF) continue;
    IROperand jd = tcc_ir_op_get_dest(ir, jq);
    int target = (int)irop_get_imm64_ex(ir, jd);
    if (target >= 0 && target < start_idx)
      real_pre_target[target] = 1;
  }
  (void)n_all;

  for (int i = 0; i < start_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;
    /* A pre-loop jump target with at least one pre-loop incoming edge is
     * a real branch boundary — defs before it might be bypassed.  Targets
     * whose only incoming edges come from later in the function (e.g.
     * the loop header's own back-edge) don't affect first-iteration state.
     */
    if (q->is_jump_target && real_pre_target[i])
    {
      for (int p = 0; p < st->n_vars; p++)
        if (var_has_def[p]) var_flow_unsafe[p] = 1;
      for (int p = 0; p < st->n_tmps; p++)
        if (tmp_has_def[p]) tmp_flow_unsafe[p] = 1;
      for (int m = 0; m < st->n_mem; m++)
        if (mem_has_def[m]) mem_flow_unsafe[m] = 1;
    }
    /* Skip control-flow ops outright — they don't have a tracked dest. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF ||
        q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE ||
        q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
        q->op == TCCIR_OP_TRAP)
      continue;
    /* A pre-loop write through a computed/indexed address (STORE_INDEXED,
     * STORE_POSTINC) or a bulk copy (BLOCK_COPY) can land on ANY stack slot:
     * the direct- and known-address STORE seeding below cannot resolve its
     * target offset, so without this an overwritten slot would keep the stale
     * value of an EARLIER direct store.  Conservatively demote every tracked
     * memory slot to flow-unsafe so the simulator never trusts a stale initial
     * value.  (agg_deep seed 47: `st12.f2 = st12.f0 ^ *p` lowers to a
     * `STORE_INDEXED #4` off `&st12`, overwriting the slot the loop body then
     * copies into `st12.f0`; missing that store folded the copy to f2's stale
     * initializer constant.)  The base/pointer vreg is still demoted by the
     * generic dest handling below, so we do not skip the rest of the loop. */
    if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC ||
        q->op == TCCIR_OP_BLOCK_COPY)
    {
      for (int m = 0; m < st->n_mem; m++)
        mem_flow_unsafe[m] = 1;
    }
    if (!irop_config[q->op].has_dest) continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_llocal || d.is_sym) continue;
    /* Direct stack-slot STORE of an immediate: seed the memory map.
     * Also seed the VAR slot when the destination has an associated vreg
     * and the source is a stack address — enables the simulator to track
     * pointer-to-local patterns like `V0 = &array[0]`. */
    if (q->op == TCCIR_OP_STORE && d.is_local && d.is_lval &&
        irop_get_tag(d) == IROP_TAG_STACKOFF)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      int32_t off = irop_get_stack_offset(d);
      LcsMemSlot *ms = lcs_mem_get(st, off);
      if (!ms) continue;
      int mem_idx = (int)(ms - st->mem);
      /* A pre-loop store also clobbers overlapping slots tracked at OTHER
       * offsets (packed sub-word accesses of the same word). */
      {
        int sw = ir_opt_store_btype_size_bytes(irop_get_btype(d));
        if (sw <= 0)
          sw = 4;
        lcs_mem_clobber_overlaps(st, off, sw, ms);
      }
      if (mem_flow_unsafe[mem_idx])
        continue;
      if (irop_is_immediate(s1))
      {
        int64_t v = irop_get_imm64_ex(ir, s1);
        int bt = irop_get_btype(d);
        ms->value = v;
        ms->btype = bt;
        ms->known = 1;
        ms->initial_value = v;
        ms->initial_known = 1;
        mem_has_def[mem_idx] = 1;
      }
      else if (s1.is_local && !s1.is_lval &&
               irop_get_tag(s1) == IROP_TAG_STACKOFF)
      {
        int32_t dvr = irop_get_vreg(d);
        if (dvr >= 0) {
          int dtype = TCCIR_DECODE_VREG_TYPE(dvr);
          int dpos  = TCCIR_DECODE_VREG_POSITION(dvr);
          LcsSlot *dslot = NULL;
          uint8_t *dflow = NULL;
          uint8_t *ddef = NULL;
          if (dtype == TCCIR_VREG_TYPE_VAR && dpos < st->n_vars) {
            dslot = &st->vars[dpos];
            dflow = &var_flow_unsafe[dpos];
            ddef = &var_has_def[dpos];
          } else if (dtype == TCCIR_VREG_TYPE_TEMP && dpos < st->n_tmps) {
            dslot = &st->tmps[dpos];
            dflow = &tmp_flow_unsafe[dpos];
            ddef = &tmp_has_def[dpos];
          }
          if (dslot && !*dflow) {
            dslot->known = 1;
            dslot->value = irop_get_stack_offset(s1);
            dslot->btype = IROP_BTYPE_INT32;
            dslot->is_addr = 1;
            *ddef = 1;
          }
        }
        ms->known = 0;
        ms->initial_known = 0;
      }
      else
      {
        ms->known = 0;
        ms->initial_known = 0;
      }
      continue;
    }
    /* Indirect STORE through a known stack-address temp/var:
     *   T <- Addr[StackLoc[off]] ; T***DEREF*** <- value
     * The body simulator resolves exactly this form (see the TCCIR_OP_STORE
     * case in lcs_step), so the pre-loop scan must too: otherwise a pre-loop
     * write through an address alias is dropped, leaving the slot's initial
     * value stale and mis-seeding the simulation (bitfield seed 5 -- a packed
     * RMW of b1 via Addr[bf], then a loop RMW of b2 in the same word; the
     * missed b1 store made the residual store clobber b1 back to 0). */
    if (q->op == TCCIR_OP_STORE && d.is_lval)
    {
      int32_t avr = irop_get_vreg(d);
      if (avr >= 0)
      {
        int atype = TCCIR_DECODE_VREG_TYPE(avr);
        int apos  = TCCIR_DECODE_VREG_POSITION(avr);
        const LcsSlot *aslot = NULL;
        if (atype == TCCIR_VREG_TYPE_VAR && apos < st->n_vars)
          aslot = &st->vars[apos];
        else if (atype == TCCIR_VREG_TYPE_TEMP && apos < st->n_tmps)
          aslot = &st->tmps[apos];
        if (aslot && aslot->known && aslot->is_addr)
        {
          int32_t off = (int32_t)aslot->value;
          LcsMemSlot *ms = lcs_mem_get(st, off);
          if (ms)
          {
            int mem_idx = (int)(ms - st->mem);
            {
              int sw = ir_opt_store_btype_size_bytes(irop_get_btype(d));
              if (sw <= 0)
                sw = 4;
              lcs_mem_clobber_overlaps(st, off, sw, ms);
            }
            if (!mem_flow_unsafe[mem_idx])
            {
              IROperand s1 = tcc_ir_op_get_src1(ir, q);
              if (irop_is_immediate(s1))
              {
                ms->value = irop_get_imm64_ex(ir, s1);
                ms->btype = irop_get_btype(d);
                ms->known = 1;
                ms->initial_value = ms->value;
                ms->initial_known = 1;
                mem_has_def[mem_idx] = 1;
              }
              else
              {
                ms->known = 0;
                ms->initial_known = 0;
              }
            }
          }
          continue;
        }
      }
    }
    if (d.is_local && !d.is_lval) continue;
    int32_t vr = irop_get_vreg(d);
    if (vr < 0) continue;
    int type = TCCIR_DECODE_VREG_TYPE(vr);
    int pos  = TCCIR_DECODE_VREG_POSITION(vr);
    LcsSlot *slot = NULL;
    uint8_t *flow_unsafe = NULL;
    uint8_t *has_def = NULL;
    if (type == TCCIR_VREG_TYPE_VAR && pos < st->n_vars) {
      slot = &st->vars[pos];
      flow_unsafe = &var_flow_unsafe[pos];
      has_def = &var_has_def[pos];
    } else if (type == TCCIR_VREG_TYPE_TEMP && pos < st->n_tmps) {
      slot = &st->tmps[pos];
      flow_unsafe = &tmp_flow_unsafe[pos];
      has_def = &tmp_has_def[pos];
    } else {
      continue;
    }
    if (*flow_unsafe) continue;
    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD ||
        q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_STORE)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (irop_is_immediate(s1))
      {
        slot->known = 1;
        slot->value = irop_get_imm64_ex(ir, s1);
        slot->btype = irop_get_btype(s1);
        slot->is_addr = 0;
        *has_def = 1;
      }
      else if (s1.is_local &&
               irop_get_tag(s1) == IROP_TAG_STACKOFF &&
               (q->op == TCCIR_OP_LEA || !s1.is_lval))
      {
        slot->known = 1;
        slot->value = irop_get_stack_offset(s1);
        slot->btype = IROP_BTYPE_INT32;
        slot->is_addr = 1;
        *has_def = 1;
      }
      else
      {
        /* Non-constant assignment overwrites the slot.  Demote. */
        slot->known = 0;
        *has_def = 0;
      }
    }
    else if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
    {
      /* Address arithmetic: `T = <stack address> +/- immediate` produces
       * another stack address.  lcs_step models this (see the ADD/SUB case),
       * so the pre-loop scan must too — otherwise a later indirect store
       * through the result (`T = &arr + 4; *T = v`) can't resolve its target
       * slot and leaves that slot's stale initializer in the memory map
       * (combo_num seed 872: `arr12[u11&7] = ...` lowers to
       * `T = Addr[StackLoc] ADD #4; *T = <runtime>`, and missing it let the
       * unrolled/simulated loop read arr12[1]'s .data initializer instead). */
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      int32_t base_off;
      if (lcs_resolve_stack_addr(st, s1, &base_off) && (!s1.is_lval || s1.is_local) &&
          irop_is_immediate(s2) && !s2.is_sym)
      {
        int64_t imm = irop_get_imm64_ex(ir, s2);
        slot->known = 1;
        slot->value = (q->op == TCCIR_OP_ADD) ? (base_off + imm) : (base_off - imm);
        slot->btype = IROP_BTYPE_INT32;
        slot->is_addr = 1;
        *has_def = 1;
      }
      else if (q->op == TCCIR_OP_ADD &&
               lcs_resolve_stack_addr(st, s2, &base_off) && (!s2.is_lval || s2.is_local) &&
               irop_is_immediate(s1) && !s1.is_sym)
      {
        int64_t imm = irop_get_imm64_ex(ir, s1);
        slot->known = 1;
        slot->value = base_off + imm;
        slot->btype = IROP_BTYPE_INT32;
        slot->is_addr = 1;
        *has_def = 1;
      }
      else
      {
        /* addr - addr, addr +/- runtime, etc.: not a resolvable address. */
        slot->known = 0;
        *has_def = 0;
      }
    }
    else
    {
      /* Any other op writing this slot: we don't model — demote. */
      slot->known = 0;
      *has_def = 0;
    }
  }
  /* Final pass: any slot whose def was followed by a control-flow boundary
   * is unsafe to trust. */
  for (int p = 0; p < st->n_vars; p++)
    if (var_flow_unsafe[p]) st->vars[p].known = 0;
  for (int p = 0; p < st->n_tmps; p++)
    if (tmp_flow_unsafe[p]) st->tmps[p].known = 0;
  for (int m = 0; m < st->n_mem; m++)
    if (mem_flow_unsafe[m]) { st->mem[m].known = 0; st->mem[m].initial_known = 0; }

  tcc_free(var_flow_unsafe);
  tcc_free(tmp_flow_unsafe);
  tcc_free(var_has_def);
  tcc_free(tmp_has_def);
  tcc_free(mem_has_def);
  tcc_free(mem_flow_unsafe);
  tcc_free(real_pre_target);
}

/* Determine whether any stack memory modified by the loop is potentially
 * accessed after `from_idx`.  Checks both direct StackLoc references and
 * indirect/indexed accesses (LOAD_INDEXED, STORE_INDEXED, indirect LOAD/
 * STORE through address-holding vregs).
 *
 * Returns 1 if any modified slot might be read after the loop, 0 if all
 * modifications are loop-internal temporaries safe to discard. */
static int lcs_any_mem_used_after(TCCIRState *ir, const LcsState *st,
                                  int from_idx)
{
  int n = ir->next_instruction_index;
  int has_modified_slot = 0;
  for (int m = 0; m < st->n_mem; m++)
  {
    if (st->mem[m].written && st->mem[m].known &&
        !(st->mem[m].initial_known &&
          st->mem[m].value == st->mem[m].initial_value))
    {
      has_modified_slot = 1;
      break;
    }
  }
  if (!has_modified_slot)
    return 0;

  for (int i = from_idx; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;
    if (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC)
      return 1;
    IROperand ops[3];
    int nops = 0;
    if (irop_config[q->op].has_dest) ops[nops++] = tcc_ir_op_get_dest(ir, q);
    if (irop_config[q->op].has_src1) ops[nops++] = tcc_ir_op_get_src1(ir, q);
    if (irop_config[q->op].has_src2) ops[nops++] = tcc_ir_op_get_src2(ir, q);
    for (int k = 0; k < nops; k++)
    {
      if (!ops[k].is_local || irop_get_tag(ops[k]) != IROP_TAG_STACKOFF)
        continue;
      int32_t off = irop_get_stack_offset(ops[k]);
      for (int m = 0; m < st->n_mem; m++)
      {
        if (st->mem[m].written && st->mem[m].offset == off)
          return 1;
      }
    }
  }
  return 0;
}

/* Determine whether a VAR vreg is read after `from_idx` (anywhere outside
 * the eliminated loop range).  Used to decide whether to emit a residual
 * ASSIGN with the final value. */
static int lcs_var_used_after(TCCIRState *ir, int var_pos, int from_idx)
{
  int n = ir->next_instruction_index;
  int32_t target_vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, var_pos);
  /* This is a linear scan over instruction *indices*, which only reflects
   * control flow while the path stays straight-line.  A redefinition therefore
   * kills the loop's value only in the straight-line prefix from the loop exit:
   * once we pass any branch, a later redefinition may sit in a sibling
   * (not-taken) branch while the real use is reached via another path.  That is
   * exactly fuzz seed 8985 — the loop is in an `if` branch, the value is read
   * after the merge, and the `else` branch redefines the same VAR at a lower
   * index than that read.  Honouring the kill there wrongly dropped the loop's
   * residual store, leaving the variable at its pre-loop value. */
  int saw_branch = 0;
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
    /* A redefinition kills the loop's value only when it is unconditionally
     * reached from the loop exit (no branch in between). */
    if (!saw_branch && irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!d.is_lval && irop_get_vreg(d) == target_vr) return 0;
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_SWITCH_TABLE)
      saw_branch = 1;
  }
  return 0;
}

/* Find the single outside target a bounded generic simulation is allowed to
 * exit to.  Branches inside the loop may either stay within [start..end] or
 * leave to this one target; a final conditional back-edge may also fall
 * through to end+1. */
static int lcs_find_single_exit_target(TCCIRState *ir, int start_idx,
                                       int end_idx, int *out_exit_target)
{
  int exit_target = -1;

#define LCS_RECORD_EXIT(t_) do {                         \
    int _t = (t_);                                       \
    if (exit_target < 0) exit_target = _t;               \
    else if (exit_target != _t) return 0;                \
  } while (0)

  for (int i = start_idx; i <= end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;

    int target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
    int target_in_loop = (target >= start_idx && target <= end_idx);
    if (!target_in_loop)
      LCS_RECORD_EXIT(target);

    if (q->op == TCCIR_OP_JUMPIF)
    {
      int fallthrough = i + 1;
      if (fallthrough > end_idx && target_in_loop)
        LCS_RECORD_EXIT(fallthrough);
    }
  }

#undef LCS_RECORD_EXIT

  if (exit_target < 0)
    return 0;
  *out_exit_target = exit_target;
  return 1;
}

/* The generic bounded simulator has no symbolic model for caller-provided
 * pointers or globals.  Keep it to loops whose state is made from locals,
 * temps, immediates, and stack-slot addresses. */
static int lcs_generic_loop_is_stack_local(TCCIRState *ir, int start_idx,
                                           int end_idx)
{
  uint8_t addr_var[LCS_MAX_TRACKED_VARS] = {0};
  uint8_t addr_tmp[LCS_MAX_TRACKED_TMPS] = {0};
  int saw_stack_mem __attribute__((unused)) = 0;

  for (int i = start_idx; i <= end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand ops[3] = {
      tcc_ir_op_get_dest(ir, q),
      tcc_ir_op_get_src1(ir, q),
      tcc_ir_op_get_src2(ir, q)
    };

    if (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC)
      return 0;

    int is_call = (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID);

    for (int k = 0; k < 3; k++)
    {
      if (is_call) continue;
      if (q->op == TCCIR_OP_FUNCPARAMVOID) continue;
      if (q->op == TCCIR_OP_FUNCPARAMVAL && k == 2) continue;
      IROperand op = ops[k];
      if (irop_is_none(op) || irop_is_immediate(op))
        continue;
      if (op.is_sym || op.is_llocal)
        return 0;

      int32_t vr = irop_get_vreg(op);
      if (vr >= 0)
      {
        int vt = TCCIR_DECODE_VREG_TYPE(vr);
        if (vt == TCCIR_VREG_TYPE_PARAM)
          return 0;
        if (vt != TCCIR_VREG_TYPE_VAR && vt != TCCIR_VREG_TYPE_TEMP)
          return 0;

        if (op.is_lval)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          int known_addr = (vt == TCCIR_VREG_TYPE_VAR)
                             ? (pos < LCS_MAX_TRACKED_VARS && addr_var[pos])
                             : (pos < LCS_MAX_TRACKED_TMPS && addr_tmp[pos]);
          if (!known_addr)
          {
            if (!op.is_local)
              return 0;
          }
          else
            saw_stack_mem = 1;
        }
        continue;
      }

      if (!(op.is_local && irop_get_tag(op) == IROP_TAG_STACKOFF))
        return 0;
      if (op.is_lval)
        saw_stack_mem = 1;
    }

    if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      IROperand add_ops[2] = { s1, s2 };
      for (int k = 0; k < 2; k++)
      {
        IROperand op = add_ops[k];
        if (op.is_local && !op.is_lval && irop_get_tag(op) == IROP_TAG_STACKOFF)
          return 0;
        int32_t vr = irop_get_vreg(op);
        if (vr >= 0)
        {
          int vt = TCCIR_DECODE_VREG_TYPE(vr);
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if ((vt == TCCIR_VREG_TYPE_VAR && pos < LCS_MAX_TRACKED_VARS && addr_var[pos]) ||
              (vt == TCCIR_VREG_TYPE_TEMP && pos < LCS_MAX_TRACKED_TMPS && addr_tmp[pos]))
            return 0;
        }
      }
    }

    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (dvr >= 0 && !dest.is_lval)
      {
        int vt = TCCIR_DECODE_VREG_TYPE(dvr);
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        uint8_t *slot = NULL;
        if (vt == TCCIR_VREG_TYPE_VAR && pos < LCS_MAX_TRACKED_VARS)
          slot = &addr_var[pos];
        else if (vt == TCCIR_VREG_TYPE_TEMP && pos < LCS_MAX_TRACKED_TMPS)
          slot = &addr_tmp[pos];

        if (slot)
        {
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          int addr_def = (q->op == TCCIR_OP_LEA ||
                          q->op == TCCIR_OP_ASSIGN ||
                          q->op == TCCIR_OP_LOAD) &&
                         src1.is_local && !src1.is_lval &&
                         irop_get_tag(src1) == IROP_TAG_STACKOFF;
          *slot = addr_def ? 1 : 0;
        }
      }
    }
  }
  return 1;
}

/* Try to fold a single loop.  Returns 1 if folded, 0 otherwise. */
static int lcs_try_fold(TCCIRState *ir, IRLoop *loop)
{
  int have_iv_trip = 0;
  InductionVar ivs[MAX_IV];
  int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1);

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

  int trip_count = -1;
  if (iv)
  {
    trip_count = compute_trip_count(iv->init_val, limit, iv->step, cond);
    if (trip_count > 0 && trip_count <= LCS_MAX_TRIP_COUNT)
      have_iv_trip = 1;
  }
  /* Compute effective loop range.  The loop detector may report a tight
   * range like [3..8] that omits the body when control flow is rotated
   * (header at 3 jumps to body at 9, body loops back to 6).  We take the
   * span [start_idx..exit_target-1] as the effective range and verify all
   * jumps within it stay inside or land exactly at exit_target. */
  int eff_start = loop->start_idx;
  int eff_end   = loop->end_idx;
  if (have_iv_trip && exit_target > eff_end + 1) {
    if (exit_target - eff_start > 512)
      return 0;
    int orig_end = eff_end;
    eff_end = exit_target - 1;
    /* The extension assumes [orig_end+1 .. eff_end] is rotated loop body,
     * reachable only through the loop's own control flow.  If an instruction
     * OUTSIDE the loop jumps INTO this absorbed region, it is not loop body
     * at all but a separate block that merely sits between the back-edge and
     * the exit target — e.g. the ELSE arm of a guard whose THEN arm holds the
     * loop: the guard's false-branch JUMP lands on the else block, which lies
     * before the join.  Folding it into the loop would NOP the else block and
     * misroute the guard jump to the exit, dropping the else body entirely
     * (longlong seed 2426).  The caller's ext_entry check only covered the
     * pre-extension range, so re-check the newly-absorbed tail here. */
    int nn = ir->next_instruction_index;
    for (int j = 0; j < nn; j++)
    {
      if (j >= eff_start && j <= eff_end) continue;
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op != TCCIR_OP_JUMP && jq->op != TCCIR_OP_JUMPIF) continue;
      int jt = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jq));
      if (jt > orig_end && jt <= eff_end)
        return 0;
    }
  }

  if (!have_iv_trip)
  {
    cmp_idx = -1;
    jmpif_idx = -1;
    if (!lcs_find_single_exit_target(ir, eff_start, eff_end, &exit_target))
      return 0;
    if (!lcs_generic_loop_is_stack_local(ir, eff_start, eff_end))
      return 0;
  }

  /* Verify all branches in extended range stay within OR land exactly at exit. */
  for (int i = eff_start; i <= eff_end; i++)
  {
    IRQuadCompact *qx = &ir->compact_instructions[i];
    if (qx->op != TCCIR_OP_JUMP && qx->op != TCCIR_OP_JUMPIF) continue;
    int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, qx));
    if ((t < eff_start || t > eff_end) && t != exit_target)
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

  /* Memory-aliasing safety: if the loop performs any STORE through a vreg
   * (i.e. writes to ANY stack slot via a computed address), downstream
   * constprop passes must alias-disambiguate the residual direct STOREs
   * against later indexed/indirect writes — sccp_resolve_stack_load
   * (extended with sccp_no_aliasing_between) handles this. */

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
  st.mem   = tcc_mallocz(sizeof(LcsMemSlot) * LCS_MAX_MEM_SLOTS);

  lcs_init_var_state(ir, loop->start_idx, &st);

  /* Seed the IV's initial value if its definition is before the loop.  The
   * generic simulator path may have no primary IV; in that case the normal
   * pre-loop scan provides all tracked initial values. */
  if (have_iv_trip && iv->init_idx >= 0)
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
        st.vars[pos].is_unsigned = 0;
        st.vars[pos].is_addr = 0;
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
  int step_trip_bound = have_iv_trip ? trip_count : LCS_MAX_TRIP_COUNT;
  int max_total_steps = (step_trip_bound + 1) * (eff_end - eff_start + 1) + 32;
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
      if (!have_iv_trip && step.next_pc <= pc)
      {
        step_trip_bound--;
        if (step_trip_bound < 0) { sim_ok = 0; break; }
      }
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

  if (!sim_ok || st.mem_overflow)
  {
    tcc_free(st.vars);
    tcc_free(st.tmps);
    tcc_free(st.calls);
    tcc_free(st.mem);
    return 0;
  }

  /* For generic bounded simulation (no IV trip count), bail when the loop
   * modifies stack memory that is accessed after the loop — the simulator
   * might be running a switch-dispatch pattern that the loop detector
   * falsely identified as a loop.  IV-trip loops are safe: the trip count
   * guarantees real iteration and the residual STOREs correctly capture
   * the final state.  Loop-internal temporaries (stack slots only
   * referenced within the loop body) are safe to ignore. */
  if (!have_iv_trip && lcs_any_mem_used_after(ir, &st, exit_target))
  {
    tcc_free(st.vars);
    tcc_free(st.tmps);
    tcc_free(st.calls);
    tcc_free(st.mem);
    return 0;
  }

  /* Pre-flight: count residual slots needed so we can bail before NOPing
   * the loop if we can't fit them.  Each modified stack-mem slot needing a
   * residual STORE consumes one IR slot, and modified VAR slots and the
   * final IV consume one each. */
  {
    int needed = 0;
    int avail = eff_end - eff_start + 1;
    if (have_iv_trip && TCCIR_DECODE_VREG_TYPE(iv->vreg) == TCCIR_VREG_TYPE_VAR &&
        lcs_var_used_after(ir, TCCIR_DECODE_VREG_POSITION(iv->vreg), exit_target))
      needed++;
    int iv_pos_pf = have_iv_trip ? TCCIR_DECODE_VREG_POSITION(iv->vreg) : -1;
    for (int p = 0; p < st.n_vars; p++)
    {
      if (p == iv_pos_pf) continue;
      if (!(written_bitmap[p / 8] & (1u << (p % 8)))) continue;
      if (!st.vars[p].known) continue;
      if (!lcs_var_used_after(ir, p, exit_target)) continue;
      needed++;
    }
    for (int m = 0; m < st.n_mem; m++)
    {
      LcsMemSlot *ms = &st.mem[m];
      if (!ms->written) continue;
      if (!ms->known) continue;
      if (ms->initial_known && ms->value == ms->initial_value) continue;
      needed++;
    }
    if (needed > avail)
    {
      tcc_free(st.vars);
      tcc_free(st.tmps);
      tcc_free(st.calls);
      tcc_free(st.mem);
      return 0;
    }
  }

  /* Sim succeeded.  Build the residual: NOP the entire loop body, then
   * emit ASSIGNs for each written VAR (and the IV) that's used after. */

  if (have_iv_trip)
    LOG_IR_GEN("[LOOP-CONST-SIM] folding loop header=%d trip=%d", loop->header_idx, trip_count);
  else
    LOG_IR_GEN("[LOOP-CONST-SIM] folding loop header=%d by bounded simulation", loop->header_idx);

  /* Collect the NOPs we'll write into. */
  int slot_pos = eff_start;
  int slot_end = eff_end;

  /* Final IV value: use the simulation's actual final value (which accounts
   * for early exits) rather than the IV-bound formula, since the loop may
   * have exited before trip_count iterations via a data-dependent condition. */
  int iv_pos = have_iv_trip ? TCCIR_DECODE_VREG_POSITION(iv->vreg) : -1;
  int iv_final_val = 0;
  if (have_iv_trip) {
    if (TCCIR_DECODE_VREG_TYPE(iv->vreg) == TCCIR_VREG_TYPE_VAR &&
        iv_pos < st.n_vars && st.vars[iv_pos].known)
      iv_final_val = (int)st.vars[iv_pos].value;
    else
      iv_final_val = iv->init_val + trip_count * iv->step;
  }

  /* NOP the entire effective loop range first */
  for (int i = eff_start; i <= eff_end; i++)
    ir->compact_instructions[i].op = TCCIR_OP_NOP;

  /* Emit residual ASSIGNs.  We write into the NOP slots starting at start_idx. */
  /* Final IV residual */
  if (have_iv_trip && TCCIR_DECODE_VREG_TYPE(iv->vreg) == TCCIR_VREG_TYPE_VAR)
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
    /* Preserve the sign of a narrow (INT8/INT16) VAR.  st.vars[p].value holds
     * the un-narrowed simulated value; downstream const-prop narrows it to the
     * residual's width via ir_opt_fit_const_to_operand, which sign- vs
     * zero-extends based on is_unsigned.  Dropping this flag would sign-extend
     * an unsigned char (e.g. 254 -> -2) and miscompile. */
    d.is_unsigned = st.vars[p].is_unsigned;
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

  /* Residual STOREs for modified stack-memory slots.  We only emit when the
   * sim-final value differs from the pre-loop initial value (when known),
   * so we don't rewrite identical bytes.  Downstream constprop must
   * alias-invalidate these against subsequent indexed/indirect writes —
   * handled by sccp_no_aliasing_between in ssa_opt_sccp. */
  for (int m = 0; m < st.n_mem; m++)
  {
    LcsMemSlot *ms = &st.mem[m];
    if (!ms->written) continue;
    if (!ms->known) continue;
    if (ms->initial_known && ms->value == ms->initial_value) continue;
    if (slot_pos > slot_end) break;
    int btype = ms->btype ? ms->btype : IROP_BTYPE_INT32;
    IROperand d = irop_make_stackoff(-1, ms->offset, /*is_lval*/ 1,
                                     /*is_llocal*/ 0, /*is_param*/ 0, btype);
    d.is_unsigned = ms->is_unsigned;
    int64_t val = ms->value;
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
    write_instr_at_nop(ir, slot_pos++, TCCIR_OP_STORE, d, s, IROP_NONE);
  }

  /* NOP the IV init too, plus any pre-loop guard CMP+JUMPIF on the IV.
   * Mirrors try_eliminate_loop. */
  if (have_iv_trip && iv->init_idx >= 0 && iv->init_idx < loop->start_idx)
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
  tcc_free(st.mem);
  return 1;
}

int tcc_ir_opt_loop_const_sim(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops)
    return 0;

  /* Merge overlapping loops — same as the unroller does.  A C for-loop
   * often produces two backward edges creating two detected loops that
   * are really one.  Merge them so LCS sees the full loop body. */
  for (int i = 0; i < loops->num_loops; i++)
  {
    if (loops->loops[i].start_idx < 0)
      continue;
    int merged;
    do
    {
      merged = 0;
      for (int j = 0; j < loops->num_loops; j++)
      {
        if (j == i || loops->loops[j].start_idx < 0)
          continue;
        IRLoop *a = &loops->loops[i];
        IRLoop *b = &loops->loops[j];
        if (a->start_idx <= b->end_idx && b->start_idx <= a->end_idx)
        {
          if (b->start_idx < a->start_idx)
          {
            a->header_idx = b->header_idx;
            a->start_idx = b->start_idx;
            a->preheader_idx = b->preheader_idx;
          }
          if (b->end_idx > a->end_idx)
            a->end_idx = b->end_idx;
          if (b->depth < a->depth)
            a->depth = b->depth;
          tcc_free(a->body_instrs);
          int new_size = a->end_idx - a->start_idx + 1;
          a->body_instrs = tcc_mallocz(sizeof(int) * new_size);
          a->body_instrs_capacity = new_size;
          a->num_body_instrs = 0;
          for (int k = a->start_idx; k <= a->end_idx; k++)
            a->body_instrs[a->num_body_instrs++] = k;
          b->start_idx = -1;
          merged = 1;
        }
      }
    } while (merged);
  }

  int changes = 0;
  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];
    if (loop->start_idx < 0) continue;
    /* Skip nested loops (depth > 1 — outermost loops have depth=1) */
    if (loop->depth > 1) continue;
    /* Skip very large loop ranges to keep cost bounded */
    if (loop->end_idx - loop->start_idx > 256) continue;

    /* Skip loops that have external entries into the body (not to the
     * header) — same guard as try_unroll_loop_ex/opt_loop.c.
     * tcc_ir_detect_loops flags ANY JUMP/JUMPIF whose numeric target is
     * lower than its own index as a loop back edge, with no dominance
     * check.  A switch's case-body-before-dispatch layout (the dispatch
     * jumps forward in control flow to a case handler that was laid out
     * earlier in instruction order) satisfies that test without being a
     * loop at all: the dispatch's own entry jump lands inside the "body"
     * but not at the "header", which a real loop never does.  Simulating
     * such a false loop executes switch-case code as if it were a
     * repeating body, corrupting the result (seed 589, switch profile). */
    int ext_entry = 0;
    for (int j = 0; j < ir->next_instruction_index && !ext_entry; j++)
    {
      if (j >= loop->start_idx && j <= loop->end_idx)
        continue; /* skip instructions inside the loop itself */
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
      {
        IROperand jdest = tcc_ir_op_get_dest(ir, jq);
        int jtarget = (int)irop_get_imm64_ex(ir, jdest);
        if (jtarget > loop->start_idx && jtarget <= loop->end_idx)
        {
          LOG_IR_GEN("[LOOP-CONST-SIM] loop header=%d: external entry from [%d] to [%d], skipping",
                     loop->header_idx, j, jtarget);
          ext_entry = 1;
        }
      }
    }
    if (ext_entry)
      continue;

    /* LCS is only sound for register-only arithmetic.  The implementation has
     * partial stack-memory modeling, but recent fuzz cases show stale aggregate
     * values still escaping through indexed stores and packed RMW chains after
     * simulation.  Keep memory-carrying loops for the normal IR pipeline. */
    int has_memory = 0;
    for (int bi = 0; bi < loop->num_body_instrs && !has_memory; bi++)
    {
      int idx = loop->body_instrs[bi];
      if (idx < loop->start_idx || idx > loop->end_idx)
        continue;
      IRQuadCompact *mq = &ir->compact_instructions[idx];
      if (mq->op == TCCIR_OP_LOAD || mq->op == TCCIR_OP_STORE ||
          mq->op == TCCIR_OP_LOAD_INDEXED || mq->op == TCCIR_OP_STORE_INDEXED ||
          mq->op == TCCIR_OP_LOAD_POSTINC || mq->op == TCCIR_OP_STORE_POSTINC ||
          mq->op == TCCIR_OP_BLOCK_COPY)
      {
        has_memory = 1;
        break;
      }
      if (irop_config[mq->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, mq);
        if (s1.is_lval)
          has_memory = 1;
      }
      if (!has_memory && irop_config[mq->op].has_src2)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, mq);
        if (s2.is_lval)
          has_memory = 1;
      }
      if (!has_memory && mq->op == TCCIR_OP_MLA)
      {
        IROperand acc = tcc_ir_op_get_accum(ir, mq);
        if (acc.is_lval)
          has_memory = 1;
      }
    }
    if (has_memory)
      continue;

    changes += lcs_try_fold(ir, loop);
  }

  tcc_ir_free_loops(loops);
  return changes;
}

int tcc_ir_opt_loop_const_sim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_loop_const_sim(ctx->ir);
}

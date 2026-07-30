/*
 *  TCC IR - Loop constant simulation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
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

#define LCS_MAX_TRIP_COUNT   64
/* Bounds the work a candidate can burn before being declined.  A 64-trip loop
 * (LCS_MAX_TRIP_COUNT) with a 30-instruction body needs ~1980, so this covers
 * every shape that actually folds while halving what a doomed one costs. */
#define LCS_MAX_ITER_STEPS   2048
#define LCS_MAX_TRACKED_VARS 256
#define LCS_MAX_TRACKED_TMPS 256
/* The per-TEMP state array is heap-allocated from the region's own maximum, so
 * only a sanity bound applies; LCS_MAX_TRACKED_TMPS bounds the fixed-size
 * address bitmaps in lcs_generic_loop_is_stack_local, not the simulator.  A
 * heavily-inlined caller reaches TEMP positions in the thousands. */
#define LCS_MAX_TMP_POS      65536
#define LCS_MAX_VAR_POS      65536
#define LCS_MAX_PARAMS       4
#define LCS_MAX_CALLS        32   /* distinct call_ids tracked per iteration */
#define LCS_MAX_MEM_SLOTS    64   /* distinct stack offsets the simulator tracks */

typedef struct LcsSlot
{
  int     known;
  int64_t value;
  int     btype;    /* IROP_BTYPE_INT32 / INT64 / FLOAT32 / FLOAT64 */
  int     is_unsigned; /* narrow (INT8/INT16) sign — residual must extend right */
  int     is_addr;  /* value is a stack offset (Addr[StackLoc[value]]) */
} LcsSlot;

typedef struct LcsCallSlot
{
  LcsSlot params[LCS_MAX_PARAMS];
} LcsCallSlot;

typedef struct LcsMemSlot
{
  int32_t offset;          /* stack offset (negative = local) */
  int64_t value;
  int     btype;
  int     is_unsigned;     /* sign of a narrow store */
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

/* Slots are keyed by exact offset: a sub-word store must invalidate the overlapping word slot (bitfield seed 11840). */
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

/* Accepts a literal Addr[StackLoc[off]] operand or a TEMP/VAR slot marked is_addr. */
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

/* Kind: 1-4 arith, 5-14 conversions, 20-23 VOID flag-setters, 0 = unknown helper. */
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
  /* 64-bit integer helpers: the IR passes the whole long long as one operand,
   * so these are ordinary two-operand evaluations. */
  if (!strcmp(name, "__aeabi_llsl")) { return 30; }
  if (!strcmp(name, "__aeabi_llsr")) { return 31; }
  if (!strcmp(name, "__aeabi_lasr")) { return 32; }
  if (!strcmp(name, "__aeabi_lmul")) { return 33; }
  return 0;
}

/* FP values are raw bit patterns; lval reads resolve through the stack-memory map. */
static int lcs_read_operand(const TCCIRState *ir, const LcsState *st,
                            IROperand op, int64_t *out)
{
  if (irop_is_immediate(op))
  {
    *out = irop_get_imm64_ex(ir, op);
    return 1;
  }

  if (op.is_sym || op.is_llocal)
    return 0;
  /* Address-typed operand: the caller must use lcs_resolve_stack_addr. */
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
  /* is_local+is_lval on a VAR vreg is a register-resident variable, not memory. */
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
  /* Addresses are only valid as ADD/SUB operands or LOAD bases. */
  if (slot->is_addr)
    return 0;
  *out = slot->value;
  return 1;
}

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

/* Tags the destination slot as an address so later loads/stores resolve through it. */
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

/* Mask a result to its operand width so 32-bit overflow wraps. */
static int64_t lcs_truncate(int64_t v, int btype)
{
  if (btype == IROP_BTYPE_INT64)
    return v;
  return (int64_t)(int32_t)(uint32_t)v;
}

typedef struct LcsStep
{
  int  action;    /* 1=advance, 0=bail, 2=set-pc, -1=exit-iter */
  int  next_pc;   /* used when action==2 */
} LcsStep;

/* VOID kinds set the caller's cmp state via st and leave *out untouched. */
static int lcs_eval_softcall(int kind, int is_double, LcsState *st,
                             const int64_t *params, int nparams,
                             int64_t *out, int *out_btype)
{
  int64_t a0 = nparams >= 1 ? params[0] : 0;
  int64_t a1 = nparams >= 2 ? params[1] : 0;
  int64_t result = 0;

  if (kind >= 30)
  {
    /* Shift counts >= 64 are undefined in C; the helpers return 0 for them,
     * but a fold must not bake in behaviour the program never relies on. */
    if (kind != 33 && ((uint64_t)a1 >= 64))
      return 0;
    switch (kind) {
    case 30: result = (int64_t)((uint64_t)a0 << (unsigned)a1); break;
    case 31: result = (int64_t)((uint64_t)a0 >> (unsigned)a1); break;
    case 32: result = (int64_t)(a0 >> (unsigned)a1); break;
    case 33: result = (int64_t)((uint64_t)a0 * (uint64_t)a1); break;
    default: return 0;
    }
    *out = result;
    *out_btype = IROP_BTYPE_INT64;
    return 1;
  }

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

/* tcc_ir_barrel_shift_fusion folds a single-use 32-bit shift into src2 of an
 * ADD/SUB/AND/OR/XOR/CMP and records it in ir->barrel_shifts[] rather than in
 * the operand — and it runs BEFORE this pass (run_post_ra_optimizations is
 * called ahead of tcc_ir_ssa_regalloc, despite the name).  So the real RHS is
 * `(src2 SHIFT #n)` and every src2 value read must come through here, or the
 * simulator computes `h + v` where the code computes `h + (v << 6)`.
 *
 * The fusion is 32-bit only (it declines any INT64 operand) and rejects a zero
 * amount for everything but LSL, so this is a plain 32-bit barrel shift whose
 * result is held sign-extended, matching lcs_truncate's convention.  Returns 0
 * for an encoding it does not model, which fails the simulation. */
static int lcs_apply_barrel_shift(const TCCIRState *ir, const IRQuadCompact *q,
                                  int64_t *v)
{
  uint8_t enc = tcc_ir_barrel_shift_at(ir, q);
  if (!enc)
    return 1;
  unsigned amount = enc & 31u;
  uint32_t x = (uint32_t)*v;
  uint32_t res;
  switch (enc >> 5)
  {
  case 1: res = x << amount; break;                       /* LSL */
  case 2: res = x >> amount; break;                       /* LSR */
  case 3: res = (uint32_t)((int32_t)x >> amount); break;  /* ASR */
  case 4: res = amount ? ((x >> amount) | (x << (32 - amount))) : x; break; /* ROR */
  default: return 0;
  }
  *v = (int64_t)(int32_t)res;
  return 1;
}

/* src2 as the instruction really consumes it: value first, barrel shift after. */
static int lcs_read_src2(const TCCIRState *ir, const LcsState *st,
                         const IRQuadCompact *q, IROperand op, int64_t *out)
{
  return lcs_read_operand(ir, st, op, out) && lcs_apply_barrel_shift(ir, q, out);
}

/* A break and the latch fall-through often land on either side of a dead NOP;
 * both are the same exit edge, so branch targets are compared NOP-normalized. */
static int lcs_skip_nops(const TCCIRState *ir, int idx)
{
  int n = ir->next_instruction_index;
  while (idx >= 0 && idx < n && ir->compact_instructions[idx].op == TCCIR_OP_NOP)
    idx++;
  return idx;
}

/* Returns taken/not-taken, or -1 for an unsupported token; NaN makes every relation false except "!=". */
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
    /* A non-lval Addr[StackLoc[X]] source is really a LEA. */
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

    /* Register-promotable VAR: update the slot so later vreg reads see the store. */
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
        /* Untracked address: a deref through a PARAM pointer is observable, so bail. */
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
    /* ASSIGN dest <- Addr[StackLoc[X]] is LEA-equivalent. */
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
    /* FP values keep their raw bit pattern — no truncation. */
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
    return r;

  case TCCIR_OP_FUNCPARAMVAL:
  {
    /* src2 carries the encoded (call_id, param_idx). */
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
    int is_flag_setter = (kind >= 20 && kind <= 23);
    if (op == TCCIR_OP_FUNCCALLVOID && !is_flag_setter) { r.action = 0; return r; }
    if (op == TCCIR_OP_FUNCCALLVAL && is_flag_setter) { r.action = 0; return r; }

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
        !lcs_read_src2(ir, st, q, src2, &v2))
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
      r.action  = 2;
      r.next_pc = target;
      return r;
    }
    if (target == exit_target || lcs_skip_nops(ir, target) == exit_target)
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
    /* Soft-float cmp state holds raw FP bits; an integer compare mis-reads negatives. */
    int taken = st->cmp_is_fp
                    ? lcs_evaluate_fp_compare(st->cmp_v1, st->cmp_v2, tok, st->cmp_is_double)
                    : evaluate_compare_condition(st->cmp_v1, st->cmp_v2, tok);
    if (taken < 0)
    {
      r.action = 0;
      return r;
    }
    int target = (int)irop_get_imm64_ex(ir, dest);

    int target_in_loop = (target >= start_idx && target <= end_idx);
    int target_is_exit = (target == exit_target ||
                          lcs_skip_nops(ir, target) == exit_target);

    if (taken)
    {
      if (target_in_loop) { r.action = 2; r.next_pc = target; return r; }
      if (target_is_exit) { r.action = -1; return r; }
      r.action = 0; return r;
    }
    if (pc == jmpif_idx && !target_in_loop) {
      /* Top-tested: fall-through continues the body. */
      return r;
    }
    if (pc == jmpif_idx && target_in_loop) {
      /* Bottom-tested: target is the back-edge, fall-through exits. */
      r.action = -1; return r;
    }
    /* Internal JUMPIF: fall-through continues. */
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
    /* ADD/SUB of a stack address and an integer yields another stack address.
     * A shifted address operand has no stack-model meaning, so an annotated
     * ADD/SUB never takes this path. */
    if ((op == TCCIR_OP_ADD || op == TCCIR_OP_SUB) &&
        !tcc_ir_barrel_shift_at(ir, q))
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
      /* addr - addr and other address combinations have no stack-model meaning. */
      if (a1_is_addr || a2_is_addr) { r.action = 0; return r; }
    }
    int64_t v1, v2;
    if (!lcs_read_operand(ir, st, src1, &v1) ||
        !lcs_read_src2(ir, st, q, src2, &v2))
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

/* Sizes the VAR/TEMP tables, rejects unsupported ops, and records written VARs. */
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
    /* lcs_apply_barrel_shift only covers the ops tcc_ir_barrel_shift_fusion
     * annotates today.  Should that set ever widen, an unmodelled consumer
     * would read src2 unshifted, so decline rather than mis-simulate it. */
    uint8_t bsh = tcc_ir_barrel_shift_at(ir, q);
    if (bsh)
    {
      switch (q->op)
      {
      case TCCIR_OP_ADD: case TCCIR_OP_SUB: case TCCIR_OP_AND:
      case TCCIR_OP_OR:  case TCCIR_OP_XOR: case TCCIR_OP_CMP:
        break;
      default:
        return 0;
      }
      if ((bsh >> 5) < 1 || (bsh >> 5) > 4)
        return 0;
    }
    /* FUNCCALL src1 is a callee SYMREF, not a value source. */
    int is_call = (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID);

    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      /* FUNCCALLVOID/FUNCPARAMVOID have no real dest. */
      int has_real_dest = !is_call || q->op == TCCIR_OP_FUNCCALLVAL;
      /* is_lval+is_local names the local's stack slot: tracked, not a memory write. */
      if (has_real_dest && (d.is_llocal || d.is_sym))
        return 0;
      if (has_real_dest && d.is_local && !d.is_lval)
        return 0;
      int32_t vr = irop_get_vreg(d);
      /* STORE through a non-local vreg: skip the addrtaken check, still size the table. */
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
          /* PARAM dest: an indirect write through a param pointer isn't modeled. */
          return 0;
        }
      }
    }
    for (int s = 0; s < 2; s++)
    {
      if (s == 0 && !irop_config[q->op].has_src1) continue;
      if (s == 1 && !irop_config[q->op].has_src2) continue;
      IROperand op = (s == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      /* Skip sources that aren't value reads: cond tokens, callee syms, call-id encodings. */
      if (q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_JUMP)
        continue;
      if (is_call) continue;
      if (q->op == TCCIR_OP_FUNCPARAMVAL && s == 1) continue;
      if (q->op == TCCIR_OP_FUNCPARAMVOID) continue;
      if (op.is_sym || op.is_llocal)
        return 0;
      /* Only ASSIGN/LOAD/LEA/ADD/SUB resolve a stack-address source. */
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
          /* Address-taken VAR slots are not register-promotable. */
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

/* Linear last-def-wins pre-loop scan; defs crossed by control flow are demoted. */
static void lcs_init_var_state(TCCIRState *ir, int start_idx, LcsState *st)
{
  uint8_t *var_flow_unsafe = tcc_mallocz((size_t)(st->n_vars > 0 ? st->n_vars : 1));
  uint8_t *tmp_flow_unsafe = tcc_mallocz((size_t)(st->n_tmps > 0 ? st->n_tmps : 1));
  uint8_t *var_has_def = tcc_mallocz((size_t)(st->n_vars > 0 ? st->n_vars : 1));
  uint8_t *tmp_has_def = tcc_mallocz((size_t)(st->n_tmps > 0 ? st->n_tmps : 1));
  uint8_t *mem_has_def = tcc_mallocz((size_t)LCS_MAX_MEM_SLOTS);
  uint8_t *mem_flow_unsafe = tcc_mallocz((size_t)LCS_MAX_MEM_SLOTS);

  /* Back-edge targets don't affect first-iteration state; only pre-loop edges count. */
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
    /* A real branch boundary: defs before it might be bypassed. */
    if (q->is_jump_target && real_pre_target[i])
    {
      for (int p = 0; p < st->n_vars; p++)
        if (var_has_def[p]) var_flow_unsafe[p] = 1;
      for (int p = 0; p < st->n_tmps; p++)
        if (tmp_has_def[p]) tmp_flow_unsafe[p] = 1;
      for (int m = 0; m < st->n_mem; m++)
        if (mem_has_def[m]) mem_flow_unsafe[m] = 1;
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF ||
        q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE ||
        q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
        q->op == TCCIR_OP_TRAP)
      continue;
    /* An indexed or bulk pre-loop write can land on any slot — demote all (agg_deep seed 47). */
    if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC ||
        q->op == TCCIR_OP_BLOCK_COPY)
    {
      for (int m = 0; m < st->n_mem; m++)
        mem_flow_unsafe[m] = 1;
    }
    if (!irop_config[q->op].has_dest) continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_llocal || d.is_sym) continue;
    /* Seed the memory map, plus the VAR slot for pointer-to-local patterns. */
    if (q->op == TCCIR_OP_STORE && d.is_local && d.is_lval &&
        irop_get_tag(d) == IROP_TAG_STACKOFF)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      int32_t off = irop_get_stack_offset(d);
      LcsMemSlot *ms = lcs_mem_get(st, off);
      if (!ms) continue;
      int mem_idx = (int)(ms - st->mem);
      /* Also clobbers packed sub-word slots of the same word. */
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
    /* Indirect STORE through a known stack address, else the slot stays stale (bitfield seed 5). */
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
        slot->known = 0;
        *has_def = 0;
      }
    }
    else if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
    {
      /* Address arithmetic, else a later indirect store can't resolve its slot (combo_num seed 872). */
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
        /* addr - addr, addr +/- runtime: not a resolvable address. */
        slot->known = 0;
        *has_def = 0;
      }
    }
    else
    {
      /* Any other op writing this slot is unmodelled. */
      slot->known = 0;
      *has_def = 0;
    }
  }
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

/* Returns 1 if any loop-modified stack slot might be accessed after from_idx. */
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

/* Decides whether a residual ASSIGN carrying the final value is needed. */
static int lcs_var_used_after(TCCIRState *ir, int var_pos, int from_idx)
{
  int n = ir->next_instruction_index;
  int32_t target_vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, var_pos);
  /* Past a branch, a redefinition may sit in a sibling arm (fuzz seed 8985). */
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
    /* A redefinition kills only when unconditionally reached from the exit. */
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

/* Bounded simulation allows exactly one outside target (possibly end+1). */
static int lcs_find_single_exit_target(TCCIRState *ir, int start_idx,
                                       int end_idx, int *out_exit_target)
{
  int exit_target = -1;

#define LCS_RECORD_EXIT(t_) do {                         \
    int _t = lcs_skip_nops(ir, (t_));                    \
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

/* No symbolic model for caller pointers or globals: locals, temps and stack addrs only. */
static int lcs_generic_loop_is_stack_local(TCCIRState *ir, int start_idx,
                                           int end_idx)
{
  uint8_t addr_var[LCS_MAX_TRACKED_VARS] = {0};
  uint8_t addr_tmp[LCS_MAX_TRACKED_TMPS] = {0};
  int saw_stack_mem __attribute__((unused)) = 0;

  /* The address bitmaps are fixed-size; a position past them would silently read
   * as "not an address", so decline the region instead. */
  for (int i = start_idx; i <= end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand ops[3] = { tcc_ir_op_get_dest(ir, q), tcc_ir_op_get_src1(ir, q),
                         tcc_ir_op_get_src2(ir, q) };
    for (int k = 0; k < 3; k++)
    {
      int32_t vr = irop_get_vreg(ops[k]);
      if (vr < 0)
        continue;
      int vt = TCCIR_DECODE_VREG_TYPE(vr);
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (vt == TCCIR_VREG_TYPE_VAR && pos >= LCS_MAX_TRACKED_VARS)
        return 0;
      if (vt == TCCIR_VREG_TYPE_TEMP && pos >= LCS_MAX_TRACKED_TMPS)
        return 0;
    }
  }

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

/* allow_extension enables the rotated-range tail extension for callers that report tight ranges. */
int lcs_fold_region(TCCIRState *ir, int start_idx, int end_idx,
                    int header_idx, int preheader_idx, int allow_extension)
{
  IRLoop lp;
  memset(&lp, 0, sizeof(lp));
  lp.start_idx = start_idx;
  lp.end_idx = end_idx;
  lp.header_idx = header_idx;
  lp.preheader_idx = preheader_idx;
  IRLoop *loop = &lp;

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
  /* Rotated control flow makes the detector report a range that omits the body. */
  int eff_start = loop->start_idx;
  int eff_end   = loop->end_idx;
  if (allow_extension && have_iv_trip && exit_target > eff_end + 1) {
    if (exit_target - eff_start > 512)
      return 0;
    int orig_end = eff_end;
    eff_end = exit_target - 1;
    /* An outside jump into the absorbed tail means it isn't loop body (longlong seed 2426). */
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

  /* Compare exits NOP-normalized: a `break` and the latch fall-through are the
   * same edge even when a dead NOP sits between their two targets. */
  exit_target = lcs_skip_nops(ir, exit_target);

  /* Every branch must stay inside the range or land exactly at exit. */
  for (int i = eff_start; i <= eff_end; i++)
  {
    IRQuadCompact *qx = &ir->compact_instructions[i];
    if (qx->op != TCCIR_OP_JUMP && qx->op != TCCIR_OP_JUMPIF) continue;
    int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, qx));
    if ((t < eff_start || t > eff_end) && lcs_skip_nops(ir, t) != exit_target)
      return 0;
  }

  /* n_vars must also cover VARs used outside the loop, for initial-state reads,
   * so the tables are sized from the whole function up front. */
  int n = ir->next_instruction_index;
  int outer_max_var = -1;
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
  if (outer_max_var >= LCS_MAX_VAR_POS)
    return 0;

  int max_var = -1, max_tmp = -1;
  int written_bitmap_bytes = outer_max_var / 8 + 1;
  uint8_t *written_bitmap = tcc_mallocz((size_t)written_bitmap_bytes);
  if (!lcs_scan_body(ir, eff_start, eff_end, &max_var, &max_tmp,
                     written_bitmap, written_bitmap_bytes) ||
      max_tmp >= LCS_MAX_TMP_POS)
  {
    tcc_free(written_bitmap);
    return 0;
  }
  if (max_var > outer_max_var)
    outer_max_var = max_var;

  LcsState st = {0};
  st.n_vars = outer_max_var + 1;
  st.n_tmps = max_tmp + 1;
  st.vars  = tcc_mallocz(sizeof(LcsSlot) * (st.n_vars > 0 ? st.n_vars : 1));
  st.tmps  = tcc_mallocz(sizeof(LcsSlot) * (st.n_tmps > 0 ? st.n_tmps : 1));
  st.calls = tcc_mallocz(sizeof(LcsCallSlot) * LCS_MAX_CALLS);
  st.mem   = tcc_mallocz(sizeof(LcsMemSlot) * LCS_MAX_MEM_SLOTS);

  lcs_init_var_state(ir, loop->start_idx, &st);
  /* Seeding walks every pre-loop store, so a caller with many stack slots fills
   * the table before simulation even starts.  That overflow is harmless: an
   * untracked slot is simply unknown, so an in-loop read of it fails and an
   * in-loop write to it hits the same full table and fails too (both paths bail
   * on their own).  Only an overflow raised during simulation is unsafe, so the
   * flag is reset here and re-armed for the run. */
  st.mem_overflow = 0;

  /* The generic path has no primary IV; the pre-loop scan supplies its values. */
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

  /* Entry is the header, which is not always the lowest instruction in the
   * region: a rotated loop puts the latch increment ahead of the header, and
   * the preheader jumps over it.  Simulating (and later, placing residuals)
   * from eff_start would run that increment on the first iteration too and
   * write the residual into a slot the preheader jumps past (980619-1). */
  int region_entry = (header_idx >= eff_start && header_idx <= eff_end)
                         ? header_idx : eff_start;

  /* Run until the exit edge is taken, bounded by trip_count * body size. */
  int sim_ok = 1;
  int pc = region_entry;
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
      /* Per-iteration scratchpads reset here; TEMPs persist as they may be loop-carried. */
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
    tcc_free(written_bitmap);
    tcc_free(st.vars);
    tcc_free(st.tmps);
    tcc_free(st.calls);
    tcc_free(st.mem);
    return 0;
  }

  /* Without a trip count the region may be a switch dispatch mis-detected as a loop. */
  if (!have_iv_trip && lcs_any_mem_used_after(ir, &st, exit_target))
  {
    tcc_free(written_bitmap);
    tcc_free(st.vars);
    tcc_free(st.tmps);
    tcc_free(st.calls);
    tcc_free(st.mem);
    return 0;
  }

  /* NOPing the region makes it fall through to eff_end+1.  When that is not
   * where the loop actually exited (an `if`-arm loop leaves via a jump over the
   * else arm), the residual must end with an explicit branch or control drops
   * into the wrong block.  NOPs inside the region are irrelevant here: only
   * slots past eff_end decide where the fall-through lands. */
  int need_exit_jump = (lcs_skip_nops(ir, eff_end + 1) != exit_target);
  if (need_exit_jump &&
      (exit_target < 0 || exit_target >= ir->next_instruction_index))
  {
    tcc_free(written_bitmap);
    tcc_free(st.vars);
    tcc_free(st.tmps);
    tcc_free(st.calls);
    tcc_free(st.mem);
    return 0;
  }

  /* Residuals can only reuse NOP slots, so count them before NOPing the loop. */
  {
    int needed = need_exit_jump ? 1 : 0;
    int avail = eff_end - region_entry + 1;
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
      tcc_free(written_bitmap);
      tcc_free(st.vars);
      tcc_free(st.tmps);
      tcc_free(st.calls);
      tcc_free(st.mem);
      return 0;
    }
  }

  if (have_iv_trip)
    LOG_IR_GEN("[LOOP-CONST-SIM] folding loop header=%d trip=%d", loop->header_idx, trip_count);
  else
    LOG_IR_GEN("[LOOP-CONST-SIM] folding loop header=%d by bounded simulation", loop->header_idx);

  int slot_pos = region_entry;
  int slot_end = eff_end;

  /* Prefer the simulated final value: the loop may exit early on a data-dependent condition. */
  int iv_pos = have_iv_trip ? TCCIR_DECODE_VREG_POSITION(iv->vreg) : -1;
  int iv_final_val = 0;
  if (have_iv_trip) {
    if (TCCIR_DECODE_VREG_TYPE(iv->vreg) == TCCIR_VREG_TYPE_VAR &&
        iv_pos < st.n_vars && st.vars[iv_pos].known)
      iv_final_val = (int)st.vars[iv_pos].value;
    else
      iv_final_val = iv->init_val + trip_count * iv->step;
  }

  for (int i = eff_start; i <= eff_end; i++)
    ir->compact_instructions[i].op = TCCIR_OP_NOP;

  if (have_iv_trip && TCCIR_DECODE_VREG_TYPE(iv->vreg) == TCCIR_VREG_TYPE_VAR)
  {
    if (lcs_var_used_after(ir, iv_pos, exit_target) && slot_pos <= slot_end)
    {
      IROperand d = irop_make_vreg(iv->vreg, IROP_BTYPE_INT32);
      IROperand s = irop_make_imm32(-1, iv_final_val, IROP_BTYPE_INT32);
      write_instr_at_nop(ir, slot_pos++, TCCIR_OP_ASSIGN, d, s, IROP_NONE);
    }
  }

  for (int p = 0; p < st.n_vars; p++)
  {
    if (p == iv_pos) continue;
    if (!(written_bitmap[p / 8] & (1u << (p % 8)))) continue;
    /* Defensive: the sim bails on unresolvable ops, so this should be known. */
    if (!st.vars[p].known) continue;
    if (!lcs_var_used_after(ir, p, exit_target)) continue;
    if (slot_pos > slot_end) {
      break;
    }
    int btype = st.vars[p].btype ? st.vars[p].btype : IROP_BTYPE_INT32;
    int32_t vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, p);
    IROperand d = irop_make_vreg(vr, btype);
    /* Preserve narrow sign: dropping it sign-extends an unsigned char (254 -> -2). */
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

  /* Residual STOREs only where the final value differs from the pre-loop one. */
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

  /* Restore the exit edge the NOPed region used to take, after the residuals so
   * they are still executed.  The slot was reserved in the needed/avail count. */
  if (need_exit_jump)
  {
    IROperand exit_dest = irop_make_imm32(-1, exit_target, IROP_BTYPE_INT32);
    write_instr_at_nop(ir, slot_pos++, TCCIR_OP_JUMP, exit_dest, IROP_NONE, IROP_NONE);
    ir->compact_instructions[exit_target].is_jump_target = 1;
  }

  /* NOP the IV init and any pre-loop guard CMP+JUMPIF on the IV. */
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

  tcc_free(written_bitmap);
  tcc_free(st.vars);
  tcc_free(st.tmps);
  tcc_free(st.calls);
  tcc_free(st.mem);
  return 1;
}

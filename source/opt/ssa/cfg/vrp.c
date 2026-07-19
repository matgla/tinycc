/*
 *  TCC IR - SSA Value Range Propagation (ssa:vrp)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Dominator-tree walk carrying a per-SSA-value [min,max] range map, scoped per
 * subtree by the domwalk mark/reset watermark; folds CMP/TEST_ZERO+JUMPIF/SETIF/SELECT. */

#define USING_GLOBALS

#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"
#include "opt_ssa_domwalk.h"
#include "opt/ssa/vrp.h"
#include "memory/small_sequence.h"
#include "memory/vector.h"
#include <limits.h>

#define VRP_TOK_ULT 0x92
#define VRP_TOK_UGE 0x93
#define VRP_TOK_EQ  0x94
#define VRP_TOK_NE  0x95
#define VRP_TOK_ULE 0x96
#define VRP_TOK_UGT 0x97
#define VRP_TOK_LT  0x9c
#define VRP_TOK_GE  0x9d
#define VRP_TOK_LE  0x9e
#define VRP_TOK_GT  0x9f

/* Endpoints are int32: every seeding path clamps to the int32 domain before storing, and sv_set
 * drops anything that would not fit.  Only the intermediate math below runs in int64. */
typedef struct { int32_t lo, hi; uint8_t valid; } SVRange;
typedef struct { int slot; SVRange prev; } SVUndo;

/* Inline-first per-function scratch (heap only past the inline cap).  Caps measured over
 * gcc.c-torture/execute (28,407 calls): 64 slots keeps 97.9% of range maps off the heap
 * (the 26/28-slot mode is 73% of calls), and 32 flags covers 100% of params, 99.8% of vars. */
TCC_SMALL_SEQUENCE_DEFINE(SVRangeSeq, SVRange, 64)
TCC_SMALL_SEQUENCE_DEFINE(SVFlagSeq, uint8_t, 32)
TCC_VECTOR_DEFINE(SVUndoVec, SVUndo)

typedef struct
{
  IRSSAOptCtx *ctx;
  SVRange *ranges;      /* [cap]; TEMP [0,temp_cap), stable PARAM [temp_cap,+param_cap), stable VAR [+var_cap) */
  int cap;
  int temp_cap;         /* TEMP-region size = vinfo_cap; PARAM then VAR slots follow it */
  uint8_t *param_stable; /* [param_cap]: PARAM p never written + address never taken */
  int param_cap;
  uint8_t *var_stable;  /* [var_cap]: VAR v written exactly once + address never taken + not volatile */
  int var_cap;
  SVUndoVec *log;       /* dominator-scope undo trail: (slot, prior range); owned by ssa_opt_vrp */
} SVState;

/* ------------------------------------------------------------------ pure range math */

/* Read a constant into the sign-extended int32 domain; reject 64-bit-typed operands. */
static int sv_read_const32(const TCCIRState *ir, IROperand op, int64_t *out)
{
  if (op.btype == IROP_BTYPE_INT64)
    return 0;
  *out = (int64_t)(int32_t)irop_get_imm64_ex(ir, op);
  return 1;
}

/* Fold a compare over [lo,hi]: 1 always, 0 never, -1 unknown.  Unsigned only when both endpoints share sign. */
static int sv_fold_cmp(int64_t lo, int64_t hi, int64_t c, int tok)
{
  if ((tok == VRP_TOK_ULT || tok == VRP_TOK_UGE || tok == VRP_TOK_ULE || tok == VRP_TOK_UGT) &&
      (lo < 0) != (hi < 0))
    return -1;
  int a = evaluate_compare_condition(lo, c, tok);
  int b = evaluate_compare_condition(hi, c, tok);
  if (a < 0 || b < 0 || a != b)
    return -1;
  return a;
}

/* Whether [lo,hi] proves `x <tok> c`: 1 always, 0 never, -1 unknown. */
static int sv_range_verdict(int64_t lo, int64_t hi, int64_t c, int tok)
{
  if (tok == VRP_TOK_EQ || tok == VRP_TOK_NE)
  {
    if (c < lo || c > hi)
      return (tok == VRP_TOK_NE) ? 1 : 0;
    if (lo == hi)
      return (tok == VRP_TOK_EQ) ? 1 : 0;
    return -1;
  }
  return sv_fold_cmp(lo, hi, c, tok);
}

/* Interval in signed int32 where `x <tok> c` holds; 0 if not a single signed interval. */
static int sv_interval_for(int tok, int64_t c, int64_t *lo, int64_t *hi)
{
  switch (tok)
  {
  case VRP_TOK_LT: if (c <= (int64_t)INT32_MIN) return 0; *lo = INT32_MIN; *hi = c - 1; return 1;
  case VRP_TOK_LE: *lo = INT32_MIN; *hi = c; return 1;
  case VRP_TOK_GT: if (c >= (int64_t)INT32_MAX) return 0; *lo = c + 1; *hi = INT32_MAX; return 1;
  case VRP_TOK_GE: *lo = c; *hi = INT32_MAX; return 1;
  case VRP_TOK_EQ: *lo = *hi = c; return 1;
  /* c in (0,2^31): the unsigned interval below c stays under 2^31, so it is also the signed one. */
  case VRP_TOK_ULT: if (c <= 0) return 0; *lo = 0; *hi = c - 1; return 1;
  case VRP_TOK_ULE: if (c < 0) return 0; *lo = 0; *hi = c; return 1;
  default: return 0;
  }
}

/* Whether `a <tok> b` is decided for every a in [alo,ahi], b in [blo,bhi]: 1/0/-1 unknown. */
static int sv_range_verdict2(int64_t alo, int64_t ahi, int64_t blo, int64_t bhi, int tok)
{
  switch (tok)   /* unsigned order matches signed order only when both ranges are non-negative */
  {
  case VRP_TOK_ULT: case VRP_TOK_ULE: case VRP_TOK_UGT: case VRP_TOK_UGE:
    if (alo < 0 || blo < 0)
      return -1;
    tok = (tok == VRP_TOK_ULT) ? VRP_TOK_LT : (tok == VRP_TOK_ULE) ? VRP_TOK_LE
        : (tok == VRP_TOK_UGT) ? VRP_TOK_GT : VRP_TOK_GE;
    break;
  }
  switch (tok)
  {
  case VRP_TOK_LT: return (ahi < blo) ? 1 : (alo >= bhi) ? 0 : -1;
  case VRP_TOK_LE: return (ahi <= blo) ? 1 : (alo > bhi) ? 0 : -1;
  case VRP_TOK_GT: return (alo > bhi) ? 1 : (ahi <= blo) ? 0 : -1;
  case VRP_TOK_GE: return (alo >= bhi) ? 1 : (ahi < blo) ? 0 : -1;
  case VRP_TOK_EQ: return (ahi < blo || alo > bhi) ? 0 : (alo == ahi && blo == bhi) ? 1 : -1;
  case VRP_TOK_NE: return (ahi < blo || alo > bhi) ? 1 : (alo == ahi && blo == bhi) ? 0 : -1;
  }
  return -1;
}

/* --------------------------------------------------------------- scoped range map */

static int sv_slot(SVState *s, int32_t vr)
{
  if (vr < 0)
    return -1;
  int ty = TCCIR_DECODE_VREG_TYPE(vr);
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (ty == TCCIR_VREG_TYPE_TEMP)
  {
    if (pos >= s->temp_cap)
      return -1;
    /* A multi-def TEMP (regalloc fallback path) would fold a range refined on a different value. */
    IRSSAVregInfo *vi = ssa_opt_vinfo(s->ctx, vr);
    if (vi && vi->def_count > 1)
      return -1;
    return pos;
  }
  /* An unwritten, never-addressed PARAM is a constant input: its range is function-wide stable. */
  if (ty == TCCIR_VREG_TYPE_PARAM)
  {
    if (pos >= s->param_cap || !s->param_stable[pos])
      return -1;
    return s->temp_cap + pos;
  }
  /* A stable VAR holds one value its store dominates, so its range is sound in dominator scope. */
  if (ty == TCCIR_VREG_TYPE_VAR)
  {
    if (pos >= s->var_cap || !s->var_stable[pos])
      return -1;
    return s->temp_cap + s->param_cap + pos;
  }
  return -1;
}

/* Tracking slot for an operand READ.  An is_lval operand is trackable only as a direct
 * VAR-slot read (is_local && !is_llocal); any deref reads a pointee, not the slot. */
static int sv_operand_slot(SVState *s, IROperand op)
{
  if (op.is_sym)
    return -1;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return -1;
  if (op.is_lval &&
      (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR || !op.is_local || op.is_llocal))
    return -1;
  return sv_slot(s, vr);
}

/* [lo,hi] for an operand: singleton for a plain int32 immediate, else its slot's range. */
static int sv_operand_range(SVState *s, IROperand op, int64_t *lo, int64_t *hi)
{
  if (op.btype != IROP_BTYPE_INT32 && op.btype != IROP_BTYPE_INT8 && op.btype != IROP_BTYPE_INT16)
    return 0;
  if (irop_is_immediate(op))
  {
    int64_t c;
    if (!irop_is_plain_imm(op) || !sv_read_const32(s->ctx->ir, op, &c))
      return 0;
    *lo = *hi = c;
    return 1;
  }
  int sl = sv_operand_slot(s, op);
  if (sl < 0 || !s->ranges[sl].valid)
    return 0;
  *lo = s->ranges[sl].lo;
  *hi = s->ranges[sl].hi;
  return 1;
}

/* A decoded `CMP x,#c`.  c_ok is 0 when the immediate leaves the int32 domain: the range folds
 * need it, but the tautology-vs-0 fold does not, so it is reported rather than rejected. */
typedef struct
{
  IROperand x;      /* the compared value (src1) */
  IROperand c_op;   /* the immediate (src2), for readers that need it raw */
  int32_t xvr;
  int slot;         /* x's tracking slot, or -1 */
  int64_t c;
  int c_ok;
} SVCmp;

/* Decode `CMP x,#c`, or `TEST_ZERO x` as x vs 0; 0 unless the rhs is an immediate. */
static int sv_read_cmp(SVState *s, int cmp_idx, SVCmp *out)
{
  TCCIRState *ir = s->ctx->ir;
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
  if (cq->op == TCCIR_OP_TEST_ZERO)
  {
    out->x = tcc_ir_op_get_src1(ir, cq);
    /* A 64-bit or float test is not a 32-bit compare against 0. */
    if (out->x.btype != IROP_BTYPE_INT32 && out->x.btype != IROP_BTYPE_INT8 &&
        out->x.btype != IROP_BTYPE_INT16)
      return 0;
    out->c_op = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
    out->xvr = irop_get_vreg(out->x);
    out->slot = sv_operand_slot(s, out->x);
    out->c = 0;
    out->c_ok = 1;
    return 1;
  }
  IROperand cs2 = tcc_ir_op_get_src2(ir, cq);
  if (!irop_is_immediate(cs2))
    return 0;
  out->x = tcc_ir_op_get_src1(ir, cq);
  out->c_op = cs2;
  out->xvr = irop_get_vreg(out->x);
  out->slot = sv_operand_slot(s, out->x);
  out->c = 0;
  out->c_ok = sv_read_const32(ir, cs2, &out->c);
  return 1;
}

static void sv_set(SVState *s, int slot, int64_t lo, int64_t hi);

/* The phi that defines vr, or NULL if vr is not a phi result. */
static IRPhiNode *sv_phi_for(SVState *s, int32_t vr)
{
  if (vr < 0)
    return NULL;
  IRSSAVregInfo *vi = ssa_opt_vinfo(s->ctx, vr);
  if (!vi || vi->def_phi_block < 0 || !s->ctx->ssa || !s->ctx->ssa->block_phis)
    return NULL;
  for (IRPhiNode *p = s->ctx->ssa->block_phis[vi->def_phi_block]; p; p = p->next)
    if (p->dest_vreg == vr)
      return p;
  return NULL;
}

/* Classify a phi operand as an immediate or a stable source slot (direct, or one
 * ASSIGN/LOAD hop).  Returns 0 for anything else; callers must treat that as abort. */
static int sv_classify_operand(SVState *s, int32_t vr, int *is_const, int64_t *c, int *slot)
{
  *is_const = 0;
  *slot = -1;
  if (vr < 0)
    return 0;
  IRSSAVregInfo *vi = ssa_opt_vinfo(s->ctx, vr);
  if (vi && vi->def_instr >= 0)
  {
    IRQuadCompact *dq = &s->ctx->ir->compact_instructions[vi->def_instr];
    if (dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_LOAD)
      return 0;
    if (tcc_ir_barrel_shift_at(s->ctx->ir, dq))
      return 0;
    IROperand src = tcc_ir_op_get_src1(s->ctx->ir, dq);
    int64_t cv;
    if (irop_is_immediate(src) && sv_read_const32(s->ctx->ir, src, &cv))
    {
      *is_const = 1;
      *c = cv;
      return 1;
    }
    int ssl = sv_operand_slot(s, src);
    if (ssl >= 0)
    {
      *slot = ssl;
      return 1;
    }
    return 0;
  }
  /* No defining instruction: a direct PARAM/VAR entry value. */
  int sl = sv_slot(s, vr);
  if (sl >= 0)
  {
    *slot = sl;
    return 1;
  }
  return 0;
}

/* Best-effort [lo,hi] for a vreg: its own scoped range, a constant def, or a stable source's range. */
static int sv_resolve_range(SVState *s, int32_t vr, int64_t *lo, int64_t *hi)
{
  int sl = sv_slot(s, vr);
  if (sl >= 0 && s->ranges[sl].valid)
  {
    *lo = s->ranges[sl].lo;
    *hi = s->ranges[sl].hi;
    return 1;
  }
  int is_const = 0, slot = -1;
  int64_t c;
  if (!sv_classify_operand(s, vr, &is_const, &c, &slot))
    return 0;
  if (is_const)
  {
    *lo = *hi = c;
    return 1;
  }
  if (slot >= 0 && s->ranges[slot].valid)
  {
    *lo = s->ranges[slot].lo;
    *hi = s->ranges[slot].hi;
    return 1;
  }
  return 0;
}

/* Whether no operand of `phi` can equal K — stronger than the interval hull, which may include K. */
static int sv_phi_excludes_const(SVState *s, IRPhiNode *phi, int64_t K)
{
  if (phi->num_operands == 0)
    return 0;
  for (int i = 0; i < phi->num_operands; i++)
  {
    int64_t lo, hi;
    if (!sv_resolve_range(s, phi->operands[i].vreg, &lo, &hi))
      return 0;
    if (K >= lo && K <= hi)
      return 0;
  }
  return 1;
}

/* On an edge pinning a phi to K, refine its one non-constant stable source S to [K,K].
 * Requires a unique such S and no operand constant == K, else S is not the taken edge. */
static void sv_backprop_phi_eq(SVState *s, IRPhiNode *phi, int64_t K)
{
  int uniq_slot = -1;
  for (int i = 0; i < phi->num_operands; i++)
  {
    int is_const = 0, slot = -1;
    int64_t c;
    if (!sv_classify_operand(s, phi->operands[i].vreg, &is_const, &c, &slot))
      return;
    if (is_const)
    {
      if (c == K)
        return;   /* this edge can also produce K: source not unique */
      continue;   /* constant != K: infeasible edge */
    }
    if (slot < 0 || (uniq_slot >= 0 && uniq_slot != slot))
      return;
    uniq_slot = slot;
  }
  if (uniq_slot < 0)
    return;
  if (s->ranges[uniq_slot].valid &&
      (K < s->ranges[uniq_slot].lo || K > s->ranges[uniq_slot].hi))
    return;   /* contradicts an in-scope range: leave it, DCE handles dead edge */
  sv_set(s, uniq_slot, K, K);
}

/* The sole writer, so it enforces SVRange's int32 domain: a fact that would not fit is dropped
 * rather than truncated (unreachable today — every caller already clamps). */
static void sv_set(SVState *s, int slot, int64_t lo, int64_t hi)
{
  if (lo < (int64_t)INT32_MIN || hi > (int64_t)INT32_MAX)
    return;
  SVUndo entry = { slot, s->ranges[slot] };
  SVUndoVec_push_back(s->log, entry);
  s->ranges[slot].valid = 1;
  s->ranges[slot].lo = (int32_t)lo;
  s->ranges[slot].hi = (int32_t)hi;
}

/* Meet a new fact with any range already in scope; an empty meet marks a dead path — skip. */
static void sv_set_intersect(SVState *s, int slot, int64_t lo, int64_t hi)
{
  if (s->ranges[slot].valid)
  {
    if (s->ranges[slot].lo > lo) lo = s->ranges[slot].lo;
    if (s->ranges[slot].hi < hi) hi = s->ranges[slot].hi;
    if (lo > hi)
      return;
  }
  sv_set(s, slot, lo, hi);
}

static int sv_mark(void *state) { return (int)((SVState *)state)->log->size; }

static void sv_reset(void *state, int wm)
{
  SVState *s = state;
  SVUndo entry;
  while (s->log->size > (size_t)wm)
  {
    SVUndoVec_pop_back(s->log, &entry);
    s->ranges[entry.slot] = entry.prev;
  }
}

static int sv_last_real(TCCIRState *ir, int start, int end)
{
  for (int j = end - 1; j >= start; j--)
    if (ir->compact_instructions[j].op != TCCIR_OP_NOP)
      return j;
  return -1;
}

static int sv_first_real(TCCIRState *ir, int start, int end)
{
  for (int j = start; j < end; j++)
    if (ir->compact_instructions[j].op != TCCIR_OP_NOP)
      return j;
  return -1;
}

/* Blocks a JUMPIF's two edges land in, or -1 each when there is none: the target may index past
 * instr_to_block (which is sized num_instrs), and the fall-through may run off the last block. */
static void sv_jumpif_edges(IRSSAOptCtx *ctx, int jmp_idx, int *target_block, int *ft_block)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  int n = cfg->num_instrs;
  int target = (int)tcc_ir_op_get_dest(ir, &ir->compact_instructions[jmp_idx]).u.imm32;
  int ft = ir_skip_nops_forward(ir, jmp_idx + 1, n);
  *target_block = (target >= 0 && target < n) ? cfg->instr_to_block[target] : -1;
  *ft_block = (ft < n) ? cfg->instr_to_block[ft] : -1;
}

/* ----------------------------------------------------------------- range seeding */

/* dest <- #c, or dest <- x (inheriting x's range). */
static void sv_seed_assign(SVState *s, int dslot, IROperand src1)
{
  int64_t c;
  if (irop_is_immediate(src1))
  {
    if (sv_read_const32(s->ctx->ir, src1, &c))
      sv_set(s, dslot, c, c);
    return;
  }
  int sslot = sv_operand_slot(s, src1);
  if (sslot >= 0 && s->ranges[sslot].valid)
    sv_set(s, dslot, s->ranges[sslot].lo, s->ranges[sslot].hi);
}

/* A narrow memory read zero/sign-extends, so the loaded width bounds dest by itself. */
static void sv_seed_width(SVState *s, int dslot, IROperand src1)
{
  if (!src1.is_lval || src1.is_llocal)
    return;
  if (src1.btype == IROP_BTYPE_INT8)
    sv_set_intersect(s, dslot, src1.is_unsigned ? 0 : -128, src1.is_unsigned ? 255 : 127);
  else if (src1.btype == IROP_BTYPE_INT16)
    sv_set_intersect(s, dslot, src1.is_unsigned ? 0 : -32768, src1.is_unsigned ? 65535 : 32767);
}

/* dest <- a +/- b over any mix of immediates and ranged slots; dropped if it leaves int32. */
static void sv_seed_addsub(SVState *s, int dslot, int op, IROperand src1, IROperand src2)
{
  int64_t alo, ahi, blo, bhi;
  if (!sv_operand_range(s, src1, &alo, &ahi) || !sv_operand_range(s, src2, &blo, &bhi))
    return;
  int add = (op == TCCIR_OP_ADD);
  int64_t lo = add ? alo + blo : alo - bhi;
  int64_t hi = add ? ahi + bhi : ahi - blo;
  if (lo >= (int64_t)INT32_MIN && hi <= (int64_t)INT32_MAX)
    sv_set(s, dslot, lo, hi);
}

/* dest <- x MUL/DIV/UDIV/UMOD/IMOD #k: scale or bound x's range. */
static void sv_seed_muldiv(SVState *s, int dslot, int op, IROperand src1, IROperand src2)
{
  int64_t k, xlo, xhi;
  if (!irop_is_plain_imm(src2) || !sv_read_const32(s->ctx->ir, src2, &k))
    return;
  if (op == TCCIR_OP_UMOD)
  {
    if (k > 0)   /* any unsigned value mod k lands in [0,k-1] */
      sv_set_intersect(s, dslot, 0, k - 1);
    return;
  }
  if (op == TCCIR_OP_IMOD)
  {
    int64_t a = (k < 0 ? -k : k) - 1;
    if (k == 0)
      return;
    int has_x = sv_operand_range(s, src1, &xlo, &xhi);   /* C remainder keeps the dividend's sign */
    sv_set_intersect(s, dslot, (has_x && xlo >= 0) ? 0 : -a, (has_x && xhi <= 0) ? 0 : a);
    return;
  }
  if (!sv_operand_range(s, src1, &xlo, &xhi))
    return;
  if (op == TCCIR_OP_MUL)
  {
    int64_t lo = (k < 0 ? xhi : xlo) * k, hi = (k < 0 ? xlo : xhi) * k;
    if (lo >= (int64_t)INT32_MIN && hi <= (int64_t)INT32_MAX)
      sv_set(s, dslot, lo, hi);
    return;
  }
  /* DIV/UDIV #k (k>0) truncate toward zero, which is monotonic; UDIV additionally needs a
   * non-negative x so the unsigned quotient matches the signed one. */
  if (k <= 0 || (op == TCCIR_OP_UDIV && xlo < 0))
    return;
  sv_set(s, dslot, xlo / k, xhi / k);
}

/* dest <- x OR/XOR #k for non-negative x and k: the result stays inside the joint bit hull. */
static void sv_seed_orxor(SVState *s, int dslot, int op, IROperand src1, IROperand src2)
{
  int64_t k, xlo, xhi;
  if (!irop_is_plain_imm(src2) || !sv_read_const32(s->ctx->ir, src2, &k) || k < 0)
    return;
  if (!sv_operand_range(s, src1, &xlo, &xhi) || xlo < 0)
    return;
  int64_t mask = 1;
  while (mask <= (xhi | k))
    mask <<= 1;
  mask -= 1;
  if (op == TCCIR_OP_OR)
    sv_set_intersect(s, dslot, xlo > k ? xlo : k, mask);   /* x|k >= max(x,k) */
  else
    sv_set_intersect(s, dslot, 0, mask);
}

/* dest <- SELECT a,b: either arm may be taken, so dest lies in the hull of both. */
static void sv_seed_select(SVState *s, int dslot, IROperand src1, IROperand src2)
{
  int64_t alo, ahi, blo, bhi;
  if (!sv_operand_range(s, src1, &alo, &ahi) || !sv_operand_range(s, src2, &blo, &bhi))
    return;
  sv_set_intersect(s, dslot, alo < blo ? alo : blo, ahi > bhi ? ahi : bhi);
}

/* dest <- UBFX/SBFX x,#(lsb|width<<5): the extracted field is definitionally bounded by its
 * width regardless of x — UBFX to [0,2^w-1], SBFX to [-2^(w-1),2^(w-1)-1].  src2 decodes as in
 * fold_bfx_value (width 0 means 8); a full-width extract leaves the int32 domain, so skip it. */
static void sv_seed_bfx(SVState *s, int dslot, int op, IROperand src2)
{
  if (!irop_is_immediate(src2))
    return;
  uint32_t enc = (uint32_t)irop_get_imm64_ex(s->ctx->ir, src2);
  int width = (enc >> 5) & 0x1F;
  if (width == 0)
    width = 8;
  if (width >= 32)
    return;
  if (op == TCCIR_OP_UBFX)
    sv_set(s, dslot, 0, ((int64_t)1 << width) - 1);
  else
    sv_set(s, dslot, -((int64_t)1 << (width - 1)), ((int64_t)1 << (width - 1)) - 1);
}

/* dest <- x AND/SHL/SAR/SHR #k. */
static void sv_seed_bitop(SVState *s, IRQuadCompact *q, int dslot, int op,
                          IROperand src1, IROperand src2)
{
  TCCIRState *ir = s->ctx->ir;
  int64_t k;
  if (!irop_is_immediate(src2) || !sv_read_const32(ir, src2, &k))
    return;
  int sslot = sv_operand_slot(s, src1);
  int have_src = (sslot >= 0 && s->ranges[sslot].valid);
  if (op == TCCIR_OP_AND)
  {
    if (k < 0)
      return;
    int64_t hi = k;   /* for non-negative x, x&k <= min(x,k) */
    if (have_src && s->ranges[sslot].lo >= 0 && s->ranges[sslot].hi < hi)
      hi = s->ranges[sslot].hi;
    sv_set(s, dslot, 0, hi);
    return;
  }
  if (k < 0 || k > 31)
    return;

  if (op == TCCIR_OP_SHR)
  {
    /* SHR #k (k>=1) zeroes the top k bits, bounding a 32-bit value to [0, 0xFFFFFFFF>>k]
     * regardless of input; tighten with the source range only when x is non-negative. */
    int is32 = tcc_ir_op_get_dest(ir, q).btype != IROP_BTYPE_INT64 && src1.btype != IROP_BTYPE_INT64;
    int64_t lo = 0, hi;
    if (k >= 1 && is32)
      hi = (int64_t)(uint32_t)(0xFFFFFFFFu >> k);
    else if (have_src && s->ranges[sslot].lo >= 0)
      hi = s->ranges[sslot].hi >> k;
    else
      return;
    if (have_src && s->ranges[sslot].lo >= 0)   /* intersect with the (tighter) source-derived range */
    {
      int64_t slo = s->ranges[sslot].lo >> k, shi = s->ranges[sslot].hi >> k;
      if (slo > lo) lo = slo;
      if (shi < hi) hi = shi;
    }
    if (lo <= hi)
      sv_set(s, dslot, lo, hi);
    return;
  }

  if (!have_src)
    return;
  int64_t xlo = s->ranges[sslot].lo, xhi = s->ranges[sslot].hi;
  if (op == TCCIR_OP_SHL)
  {
    int64_t lo = xlo * (1LL << k), hi = xhi * (1LL << k);
    if (lo >= (int64_t)INT32_MIN && hi <= (int64_t)INT32_MAX)
      sv_set(s, dslot, lo, hi);
  }
  else if (op == TCCIR_OP_SAR)
    sv_set(s, dslot, xlo >> k, xhi >> k);
}

/* Seed dest's range from a value-producing def; SSA is single-def so no kill is needed. */
static void sv_seed_def(SVState *s, IRQuadCompact *q)
{
  TCCIRState *ir = s->ctx->ir;
  int op = q->op;
  if (!irop_config[op].has_dest)
    return;
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  if (dest.is_lval || dest.is_sym)
    return;
  int dslot = sv_slot(s, irop_get_vreg(dest));
  if (dslot < 0)
    return;
  /* A hidden barrel shift on src2 means the visible operands don't describe the value. */
  if (tcc_ir_barrel_shift_at(ir, q))
    return;

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);

  if (op == TCCIR_OP_ASSIGN || op == TCCIR_OP_LOAD)
  {
    sv_seed_assign(s, dslot, src1);
    sv_seed_width(s, dslot, src1);
  }
  else if (op == TCCIR_OP_SETIF || op == TCCIR_OP_BOOL_OR || op == TCCIR_OP_BOOL_AND)
    sv_set(s, dslot, 0, 1);   /* all produce a definitional 0/1 */
  else if (op == TCCIR_OP_UBFX || op == TCCIR_OP_SBFX)
    sv_seed_bfx(s, dslot, op, src2);
  else if (op == TCCIR_OP_ADD || op == TCCIR_OP_SUB)
    sv_seed_addsub(s, dslot, op, src1, src2);
  else if (op == TCCIR_OP_AND || op == TCCIR_OP_SHL || op == TCCIR_OP_SAR || op == TCCIR_OP_SHR)
    sv_seed_bitop(s, q, dslot, op, src1, src2);
  else if (op == TCCIR_OP_MUL || op == TCCIR_OP_DIV || op == TCCIR_OP_UDIV ||
           op == TCCIR_OP_UMOD || op == TCCIR_OP_IMOD)
    sv_seed_muldiv(s, dslot, op, src1, src2);
  else if (op == TCCIR_OP_OR || op == TCCIR_OP_XOR)
    sv_seed_orxor(s, dslot, op, src1, src2);
  else if (op == TCCIR_OP_SELECT)
    sv_seed_select(s, dslot, src1, src2);
}

/* ------------------------------------------------------------------- branch folds */

/* Commit a verdict to a CMP+JUMPIF: 1 -> JUMP, 0 -> both NOP; drops the dead edge's phi operands. */
static void sv_commit_jumpif(IRSSAOptCtx *ctx, int cmp_idx, int jmp_idx, int verdict)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
  IRQuadCompact *jq = &ir->compact_instructions[jmp_idx];
  IROperand jdst = tcc_ir_op_get_dest(ir, jq);
  int target_block, ft_block;
  sv_jumpif_edges(ctx, jmp_idx, &target_block, &ft_block);
  int src_block = cfg->instr_to_block[jmp_idx];

  cq->op = TCCIR_OP_NOP;
  if (verdict == 1)
  {
    jq->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, jmp_idx, jdst);
    if (ft_block >= 0 && ft_block != target_block)   /* drop the phi edge on the dead fall-through */
      ssa_drop_phi_edge(ctx, src_block, ft_block);
  }
  else
  {
    jq->op = TCCIR_OP_NOP;
    if (target_block >= 0)   /* drop the phi edge on the dead target */
      ssa_drop_phi_edge(ctx, src_block, target_block);
  }
}

/* Drop a folded CMP's use of one operand vreg from that vreg's use list. */
static void sv_drop_cmp_use(IRSSAOptCtx *ctx, int32_t vr, int cmp_idx)
{
  if (vr < 0)
    return;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (vi)
    ssa_opt_remove_use_instr(vi, cmp_idx);
}

/* Whether the instruction after idx (skipping NOPs) reads the same flag state: folding the
 * compare away would leave it consuming garbage flags (see ssa_fold_test_zero's chain fold). */
static int sv_next_reads_flags(TCCIRState *ir, int idx)
{
  int n = ir->next_instruction_index;
  int k = ir_skip_nops_forward(ir, idx + 1, n);
  if (k >= n)
    return 0;
  int op = ir->compact_instructions[k].op;
  return op == TCCIR_OP_SETIF || op == TCCIR_OP_SELECT || op == TCCIR_OP_JUMPIF;
}

/* Verdict for the CMP/TEST_ZERO at cmp_idx under tok: 1 always, 0 never, -1 unknown.
 * Fills *cmp for use-list upkeep; *b_vr is the rhs vreg when the rhs is a register. */
static int sv_cmp_verdict(SVState *s, int cmp_idx, int tok, SVCmp *cmp, int32_t *b_vr)
{
  TCCIRState *ir = s->ctx->ir;
  *b_vr = -1;
  cmp->xvr = -1;
  if (tok < 0)
    return -1;
  /* A deref/sym x yields slot = -1 (the base vreg's range describes the wrong value), which
   * disables only the range fold; the tautology-vs-0 path below holds for any value. */
  if (sv_read_cmp(s, cmp_idx, cmp))
  {
    int verdict = -1;
    if (irop_get_imm64_ex(ir, cmp->c_op) == 0)   /* tautology vs 0, no range needed */
    {
      if (tok == VRP_TOK_UGE) verdict = 1;
      else if (tok == VRP_TOK_ULT) verdict = 0;
    }
    if (verdict < 0 && cmp->slot >= 0 && s->ranges[cmp->slot].valid && cmp->c_ok)
      verdict = sv_range_verdict(s->ranges[cmp->slot].lo, s->ranges[cmp->slot].hi, cmp->c, tok);
    /* A phi whose operands all exclude c decides `x == c` even when the interval hull cannot. */
    if (verdict < 0 && cmp->c_ok && (tok == VRP_TOK_EQ || tok == VRP_TOK_NE))
    {
      IRPhiNode *phi = sv_phi_for(s, cmp->xvr);
      if (phi && sv_phi_excludes_const(s, phi, cmp->c))
        verdict = (tok == VRP_TOK_NE) ? 1 : 0;
    }
    return verdict;
  }
  /* Register rhs: decide from the two operands' intervals. */
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
  if (cq->op != TCCIR_OP_CMP)
    return -1;
  IROperand a = tcc_ir_op_get_src1(ir, cq), b = tcc_ir_op_get_src2(ir, cq);
  int64_t alo, ahi, blo, bhi;
  if (!sv_operand_range(s, a, &alo, &ahi) || !sv_operand_range(s, b, &blo, &bhi))
    return -1;
  int verdict = sv_range_verdict2(alo, ahi, blo, bhi, tok);
  if (verdict >= 0)
  {
    cmp->xvr = irop_get_vreg(a);
    *b_vr = irop_get_vreg(b);
  }
  return verdict;
}

/* Fold CMP x,rhs / TEST_ZERO x ; JUMPIF: -> unconditional JUMP (taken) or NOP,NOP (never). */
static int sv_try_fold_jumpif(IRSSAOptCtx *ctx, SVState *s, int cmp_idx, int jmp_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *jq = &ir->compact_instructions[jmp_idx];
  int tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jq));

  SVCmp cmp;
  int32_t b_vr;
  int verdict = sv_cmp_verdict(s, cmp_idx, tok, &cmp, &b_vr);
  if (verdict < 0 || sv_next_reads_flags(ir, jmp_idx))
    return 0;

  sv_commit_jumpif(ctx, cmp_idx, jmp_idx, verdict);
  sv_drop_cmp_use(ctx, cmp.xvr, cmp_idx);
  sv_drop_cmp_use(ctx, b_vr, cmp_idx);
  return 1;
}

/* Two CMP operands denote the identical value: same vreg and same addressing flags. */
static int sv_same_operand(IROperand x, IROperand y)
{
  int32_t vx = irop_get_vreg(x);
  return vx >= 0 && vx == irop_get_vreg(y) && x.is_lval == y.is_lval && x.is_sym == y.is_sym &&
         x.is_llocal == y.is_llocal && x.is_local == y.is_local;
}

/* Fold B's leading `CMP a,b ; JUMPIF c2` when its idom-pred ends in `CMP a,b ; JUMPIF c1` and
 * !c1 decides c2.  Leading-CMP only, so the operands cannot have been redefined since entry. */
static int sv_try_fold_regreg(IRSSAOptCtx *ctx, int block, int cmp_idx, int jmp_idx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  IRBasicBlock *bb = &cfg->blocks[block];
  if (bb->num_preds != 1 || bb->preds[0] != bb->idom)
    return 0;
  if (sv_first_real(ir, bb->start_idx, bb->end_idx) != cmp_idx)
    return 0;

  /* Only the pred's JUMPIF runs between the two CMPs — no store — so even lval operands hold
   * identical values at both, provided the full operand identity (vreg + flags) matches. */
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
  IROperand a_op = tcc_ir_op_get_src1(ir, cq), b_op = tcc_ir_op_get_src2(ir, cq);
  int32_t a = irop_get_vreg(a_op);
  int32_t b = irop_get_vreg(b_op);
  if (a < 0 || b < 0)
    return 0;

  IRBasicBlock *pb = &cfg->blocks[bb->preds[0]];
  int pterm = sv_last_real(ir, pb->start_idx, pb->end_idx);
  if (pterm < 0 || ir->compact_instructions[pterm].op != TCCIR_OP_JUMPIF)
    return 0;
  int pcmp = sv_last_real(ir, pb->start_idx, pterm);
  if (pcmp < 0 || ir->compact_instructions[pcmp].op != TCCIR_OP_CMP)
    return 0;
  IRQuadCompact *pcq = &ir->compact_instructions[pcmp];
  IROperand pa_op = tcc_ir_op_get_src1(ir, pcq), pb_op = tcc_ir_op_get_src2(ir, pcq);

  int eff = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, &ir->compact_instructions[jmp_idx]));
  if (sv_same_operand(a_op, pa_op) && sv_same_operand(b_op, pb_op))
    ;                               /* same operand order: eff = c2 unchanged */
  else if (sv_same_operand(a_op, pb_op) && sv_same_operand(b_op, pa_op))
    eff = vrp_swap_cmp_tok(eff);     /* operands swapped */
  else
    return 0;
  if (eff < 0)
    return 0;

  /* Only the fall-through edge carries !c1; bail on the taken edge or if it is ambiguous. */
  IRQuadCompact *pjq = &ir->compact_instructions[pterm];
  int target_block, ft_block;
  sv_jumpif_edges(ctx, pterm, &target_block, &ft_block);
  if (block != ft_block || block == target_block)
    return 0;

  int known = vrp_negate_cmp_tok((int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, pjq)));
  if (known < 0)
    return 0;

  int verdict = -1;
  if (vrp_cmp_implies(known, eff))
    verdict = 1;
  else if (vrp_cmp_implies(known, vrp_negate_cmp_tok(eff)))
    verdict = 0;
  if (verdict < 0)
    return 0;

  sv_commit_jumpif(ctx, cmp_idx, jmp_idx, verdict);
  sv_drop_cmp_use(ctx, a, cmp_idx);
  sv_drop_cmp_use(ctx, b, cmp_idx);
  return 1;
}

/* Fold CMP/TEST_ZERO ; SETIF: rewrite SETIF to ASSIGN of the constant verdict. */
static int sv_try_fold_setif(IRSSAOptCtx *ctx, SVState *s, int cmp_idx, int set_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
  IRQuadCompact *sq = &ir->compact_instructions[set_idx];
  int tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, sq));

  SVCmp cmp;
  int32_t b_vr;
  int v = sv_cmp_verdict(s, cmp_idx, tok, &cmp, &b_vr);
  if (v < 0 || sv_next_reads_flags(ir, set_idx))
    return 0;

  IROperand set_dest = tcc_ir_op_get_dest(ir, sq);
  cq->op = TCCIR_OP_NOP;
  sq->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src1(ir, set_idx, irop_make_imm32(-1, v, IROP_BTYPE_INT32));
  tcc_ir_op_set_dest(ir, sq, set_dest);

  sv_drop_cmp_use(ctx, cmp.xvr, cmp_idx);
  sv_drop_cmp_use(ctx, b_vr, cmp_idx);
  return 1;
}

/* Fold CMP/TEST_ZERO ; SELECT then,else [cond]: rewrite to `dest <- chosen arm`. */
static int sv_try_fold_select(IRSSAOptCtx *ctx, SVState *s, int cmp_idx, int sel_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
  IRQuadCompact *sq = &ir->compact_instructions[sel_idx];
  int tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_cond(ir, sq));

  SVCmp cmp;
  int32_t b_vr;
  int v = sv_cmp_verdict(s, cmp_idx, tok, &cmp, &b_vr);
  if (v < 0 || sv_next_reads_flags(ir, sel_idx))
    return 0;

  IROperand chosen = v ? tcc_ir_op_get_src1(ir, sq) : tcc_ir_op_get_src2(ir, sq);
  IROperand dropped = v ? tcc_ir_op_get_src2(ir, sq) : tcc_ir_op_get_src1(ir, sq);
  IROperand sel_dest = tcc_ir_op_get_dest(ir, sq);
  cq->op = TCCIR_OP_NOP;
  sq->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src1(ir, sel_idx, chosen);
  tcc_ir_op_set_dest(ir, sq, sel_dest);

  sv_drop_cmp_use(ctx, cmp.xvr, cmp_idx);
  sv_drop_cmp_use(ctx, b_vr, cmp_idx);
  sv_drop_cmp_use(ctx, irop_get_vreg(dropped), sel_idx);   /* the unchosen arm is no longer read */
  return 1;
}

/* --------------------------------------------------------------- domwalk hooks */

/* On entering a block whose sole predecessor is its immediate dominator, refine the
 * compared value's range by the edge condition (taken edge if this is the jump target). */
static int sv_enter(IRSSAOptCtx *ctx, int block, void *state)
{
  SVState *s = state;
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  IRBasicBlock *bb = &cfg->blocks[block];
  /* A self-loop pred is circular: the block is also reached from entry, so its own
   * back-edge condition never holds unconditionally on arrival. */
  if (bb->num_preds != 1 || bb->preds[0] != bb->idom || bb->preds[0] == block)
    return 0;

  IRBasicBlock *pb = &cfg->blocks[bb->preds[0]];
  int term = sv_last_real(ir, pb->start_idx, pb->end_idx);
  if (term < 0)
    return 0;
  IRQuadCompact *jq = &ir->compact_instructions[term];
  if (jq->op != TCCIR_OP_JUMPIF)
    return 0;
  int cmp_idx = sv_last_real(ir, pb->start_idx, term);
  if (cmp_idx < 0)
    return 0;
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
  if (cq->op != TCCIR_OP_CMP && cq->op != TCCIR_OP_TEST_ZERO)
    return 0;

  SVCmp cmp;
  if (!sv_read_cmp(s, cmp_idx, &cmp) || !cmp.c_ok)
    return 0;
  int tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jq));

  /* A target past the last instruction is the function exit: this block is then unambiguously
   * the fall-through, which is what we refine on. */
  int target_block, ft_block;
  sv_jumpif_edges(ctx, term, &target_block, &ft_block);

  int is_target = (block == target_block);
  int is_ft = (block == ft_block);
  if (is_target == is_ft)   /* ambiguous (both/neither): no sound edge fact */
    return 0;

  int eff_tok = is_target ? tok : vrp_negate_cmp_tok(tok);
  if (eff_tok < 0)
    return 0;
  int64_t lo, hi;
  if (!sv_interval_for(eff_tok, cmp.c, &lo, &hi))
  {
    /* No representable interval: still trim an in-scope range at its boundary. */
    if (cmp.slot < 0 || !s->ranges[cmp.slot].valid)
      return 0;
    int64_t rlo = s->ranges[cmp.slot].lo, rhi = s->ranges[cmp.slot].hi;
    if (eff_tok == VRP_TOK_NE && rlo < rhi)
    {
      if (cmp.c == rlo)
        sv_set(s, cmp.slot, rlo + 1, rhi);
      else if (cmp.c == rhi)
        sv_set(s, cmp.slot, rlo, rhi - 1);
    }
    else if ((eff_tok == VRP_TOK_UGE || eff_tok == VRP_TOK_UGT) && cmp.c >= 0 && rlo >= 0)
    {
      int64_t nlo = cmp.c + (eff_tok == VRP_TOK_UGT);   /* non-negative range: unsigned = signed */
      if (nlo > rlo && nlo <= rhi)
        sv_set(s, cmp.slot, nlo, rhi);
    }
    return 0;
  }

  if (cmp.slot >= 0)
  {
    int64_t rlo = lo, rhi = hi;
    if (s->ranges[cmp.slot].valid)   /* intersect with the range already in scope */
    {
      if (s->ranges[cmp.slot].lo > rlo) rlo = s->ranges[cmp.slot].lo;
      if (s->ranges[cmp.slot].hi < rhi) rhi = s->ranges[cmp.slot].hi;
    }
    if (rlo <= rhi)
      sv_set(s, cmp.slot, rlo, rhi);
  }

  /* A singleton edge fact pins the compared value; push that equality back through a phi. */
  if (lo == hi)
  {
    IRPhiNode *phi = sv_phi_for(s, cmp.xvr);
    if (phi)
      sv_backprop_phi_eq(s, phi, lo);
  }
  return 0;
}

/* Seed each phi dest with the hull of its operands' ranges.  Loop-carried operands fail
 * sv_resolve_range (their defs are not ASSIGN/LOAD of a stable source), aborting the hull. */
static void sv_seed_phis(SVState *s, int block)
{
  if (!s->ctx->ssa || !s->ctx->ssa->block_phis)
    return;
  for (IRPhiNode *p = s->ctx->ssa->block_phis[block]; p; p = p->next)
  {
    int dslot = sv_slot(s, p->dest_vreg);
    if (dslot < 0 || s->ranges[dslot].valid || p->num_operands <= 0)
      continue;
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    int k;
    for (k = 0; k < p->num_operands; k++)
    {
      int64_t olo, ohi;
      if (!sv_resolve_range(s, p->operands[k].vreg, &olo, &ohi))
        break;
      if (olo < lo) lo = olo;
      if (ohi > hi) hi = ohi;
    }
    if (k == p->num_operands)
      sv_set(s, dslot, lo, hi);
  }
}

static int sv_visit(IRSSAOptCtx *ctx, int block, void *state)
{
  SVState *s = state;
  TCCIRState *ir = ctx->ir;
  IRBasicBlock *bb = &ctx->cfg->blocks[block];
  int changes = 0;

  sv_seed_phis(s, block);
  for (int i = bb->start_idx; i < bb->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_TEST_ZERO)
    {
      int j = i + 1;
      while (j < bb->end_idx && ir->compact_instructions[j].op == TCCIR_OP_NOP)
        j++;
      if (j < bb->end_idx)
      {
        int op2 = ir->compact_instructions[j].op;
        if (op2 == TCCIR_OP_JUMPIF)
        {
          int fired = sv_try_fold_jumpif(ctx, s, i, j);
          if (!fired && q->op == TCCIR_OP_CMP && !irop_is_immediate(tcc_ir_op_get_src2(ir, q)))
            fired = sv_try_fold_regreg(ctx, block, i, j);
          changes += fired;
        }
        else if (op2 == TCCIR_OP_SETIF)
          changes += sv_try_fold_setif(ctx, s, i, j);
        else if (op2 == TCCIR_OP_SELECT)
          changes += sv_try_fold_select(ctx, s, i, j);
      }
      continue;
    }
    sv_seed_def(s, q);
  }
  return changes;
}

/* Whether any instruction directly writes vr (as a non-lval dest). */
static int sv_vreg_ever_written(TCCIRState *ir, int32_t vr)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (!d.is_lval && irop_get_vreg(d) == vr)
      return 1;
  }
  return 0;
}

/* Count writes to a VAR slot, including the is_lval slot STORE sv_vreg_ever_written skips.
 * Counting a deref STORE `*V<-x` too only over-marks V non-stable, which is sound.  Stops at 2. */
static int sv_var_write_count(TCCIRState *ir, int32_t vr)
{
  int n = ir->next_instruction_index, cnt = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    if (irop_get_vreg(tcc_ir_op_get_dest(ir, q)) == vr && ++cnt > 1)
      break;
  }
  return cnt;
}

int ssa_opt_vrp(IRSSAOptCtx *ctx)
{
  IRCFG *cfg = ctx->cfg;
  if (!cfg || cfg->num_blocks == 0 || ctx->vinfo_cap <= 0)
    return 0;

  TCCIRState *ir = ctx->ir;
  small_sequence(SVRangeSeq) ranges_owner = { 0 };
  small_sequence(SVFlagSeq) param_stable_owner = { 0 }, var_stable_owner = { 0 };
  scoped_named_vector(SVUndoVec) log_owner = { 0 };

  SVState s = { 0 };
  s.ctx = ctx;
  s.log = &log_owner;
  s.temp_cap = ctx->vinfo_cap;
  s.param_cap = ir->next_parameter > 0 ? ir->next_parameter : 0;
  s.var_cap = ir->next_local_variable > 0 ? ir->next_local_variable : 0;
  s.cap = s.temp_cap + s.param_cap + s.var_cap;
  SVRangeSeq_init(&ranges_owner, (size_t)s.cap);
  s.ranges = SVRangeSeq_data(&ranges_owner);

  /* Precomputed so sv_slot's per-lookup test is O(1) instead of an O(n) scan. */
  if (s.param_cap > 0)
  {
    int n = ir->next_instruction_index;
    SVFlagSeq_init(&param_stable_owner, (size_t)s.param_cap);
    s.param_stable = SVFlagSeq_data(&param_stable_owner);
    for (int p = 0; p < s.param_cap; p++)
    {
      int32_t pvr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, p);
      s.param_stable[p] =
          !sv_vreg_ever_written(ir, pvr) && !ir_opt_vreg_address_taken_between(ir, pvr, -1, n);
    }
  }

  /* Stable VAR: written once, address never taken, not volatile. */
  if (s.var_cap > 0)
  {
    int n = ir->next_instruction_index;
    SVFlagSeq_init(&var_stable_owner, (size_t)s.var_cap);
    s.var_stable = SVFlagSeq_data(&var_stable_owner);
    for (int v = 0; v < s.var_cap; v++)
    {
      int32_t vvr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, v);
      int is_vol = (v < ir->variables_live_intervals_size &&
                    ir->variables_live_intervals[v].is_volatile);
      s.var_stable[v] =
          !is_vol && sv_var_write_count(ir, vvr) == 1 &&
          !ir_opt_vreg_address_taken_between(ir, vvr, -1, n);
    }
  }

  OptSSADomWalk walk = {
      .state = &s,
      .mark = sv_mark,
      .reset = sv_reset,
      .enter = sv_enter,
      .visit = sv_visit,
  };
  int changes = opt_ssa_domwalk(ctx, &walk);
  /* Our branch folds strand blocks, whose phi operands must go the same way
   * ssa_opt_branch prunes its own (a stale operand's def is later NOP'd by
   * dce, leaving it unresolvable for good). */
  if (changes > 0)
    changes += ssa_opt_prune_unreachable_phis(ctx);
  return changes;
}

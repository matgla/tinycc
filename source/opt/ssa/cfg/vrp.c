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
#include "opt_range.h"
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
/* nz: the value is known non-zero where no single signed interval says so (`x >u 0`).
 * sign64: a 64-bit value has no range here, only a known sign (1 >= 0, -1 < 0, 0 unknown). */
typedef struct { int32_t lo, hi; uint8_t valid, nz; int8_t sign64; } SVRange;
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
  int *var_writes;      /* [var_cap * SV_VAR_WRITES]: a VAR's writes, all of them (-1 after the last), or
                           var_writes[v * SV_VAR_WRITES] == -2: not known (addressed, volatile, too many, partial) */
  uint8_t *var_addr;    /* [var_cap]: the VAR's address is taken */
  int chain_steps;      /* what is left of one proof's steps (sv_chain_step) */
} SVState;

/* the writes of a VAR the value folds may look through (inlined functions'
   return slots, which SSA leaves in memory: ssa.c's store-only rule) */
#define SV_VAR_WRITES 4

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
  if (irop_is_sym(op))
    return -1;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return -1;
  if (op.is_lval &&
      (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR || !irop_is_local(op) || irop_is_llocal(op)))
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
  int swapped;      /* `CMP #c,x`: the reader's condition token applies to (c, x) */
} SVCmp;

/* Decode `CMP x,#c`, or `TEST_ZERO x` as x vs 0; 0 unless the rhs is an immediate. */
static int sv_read_cmp(SVState *s, int cmp_idx, SVCmp *out)
{
  TCCIRState *ir = s->ctx->ir;
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
  /* A fused barrel shift means the real rhs is (src2 SHIFT #n) — the visible
   * operand does not describe the compared value (fuzz seed signed:8890,
   * test 393). */
  if (tcc_ir_barrel_shift_at(ir, cq))
    return 0;
  out->swapped = 0;
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
  IROperand cs1 = tcc_ir_op_get_src1(ir, cq);
  IROperand cs2 = tcc_ir_op_get_src2(ir, cq);
  /* `CMP #c,x` (a rotated loop's `0 < n` guard): read as x against c; the
   * callers swap the condition token. */
  if (!irop_is_immediate(cs2) && irop_is_immediate(cs1) && !irop_is_sym(cs1))
  {
    IROperand t = cs1;
    cs1 = cs2;
    cs2 = t;
    out->swapped = 1;
  }
  if (!irop_is_immediate(cs2))
    return 0;
  out->x = cs1;
  out->c_op = cs2;
  out->xvr = irop_get_vreg(out->x);
  out->slot = sv_operand_slot(s, out->x);
  out->c = 0;
  out->c_ok = sv_read_const32(ir, cs2, &out->c);
  return 1;
}

static void sv_set(SVState *s, int slot, int64_t lo, int64_t hi);

/* The phi that defines vr, or NULL if vr is not a phi result. */
/* A loop phi carries a value across a BACK EDGE: that operand holds the
 * PREVIOUS iteration's value, so any range in scope at the current program
 * point (this iteration's guards) says nothing about it.  Evaluating such an
 * operand with sv_resolve_range let `if (t >= TOK_IDENT) { if (prev_tok ==
 * TOK_LAND) ... }` fold the prev_tok check away (prescan_captured_vars lost
 * its &&-label block: every parent-__label__ address-of in a nested function
 * failed with "label used but not defined").  An edge P->H is a back edge
 * iff the phi's block H dominates P. */
static int sv_phi_is_loop_phi(SVState *s, IRPhiNode *phi, int32_t dest_vr)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(s->ctx, dest_vr);
  IRCFG *cfg = s->ctx->cfg;
  if (!vi || vi->def_phi_block < 0 || !cfg)
    return 1; /* unknown home block: assume the worst */
  for (int i = 0; i < phi->num_operands; i++)
  {
    int p = phi->operands[i].pred_block;
    if (p < 0 || p >= cfg->num_blocks ||
        tcc_ir_cfg_dominates(cfg, vi->def_phi_block, p))
      return 1;
  }
  return 0;
}

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

static void sv_set_sign64(SVState *s, int slot, int sign)
{
  SVUndo entry = { slot, s->ranges[slot] };
  SVUndoVec_push_back(s->log, entry);
  s->ranges[slot].sign64 = (int8_t)sign;
}

static void sv_set_nonzero(SVState *s, int slot)
{
  if (s->ranges[slot].nz)
    return;
  SVUndo entry = { slot, s->ranges[slot] };
  SVUndoVec_push_back(s->log, entry);
  s->ranges[slot].nz = 1;
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
  int target = (int)tcc_ir_op_dest_u_imm32(ir, &ir->compact_instructions[jmp_idx]);
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
  if (!src1.is_lval || irop_is_llocal(src1))
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

/* SAR #k bounds any 32-bit value to [INT32_MIN>>k, INT32_MAX>>k] — the shape `(intN_t)x`
 * lowers to (SHL #k; SAR #k) before it becomes an SBFX. */
static void sv_seed_sar_width(SVState *s, IRQuadCompact *q, int dslot, IROperand src1, IROperand src2)
{
  int64_t k;
  if (!irop_is_plain_imm(src2) || !sv_read_const32(s->ctx->ir, src2, &k) || k < 1 || k > 31)
    return;
  if (src1.btype != IROP_BTYPE_INT32 || tcc_ir_op_get_dest(s->ctx->ir, q).btype != IROP_BTYPE_INT32)
    return;
  sv_set_intersect(s, dslot, (int64_t)INT32_MIN >> k, (int64_t)INT32_MAX >> k);
}

/* Bits common to every value of [lo,hi]: the shared leading prefix of the two endpoints.
 * Only for a range that does not straddle zero, where unsigned order matches signed order. */
static int sv_range_known_bits(int64_t lo, int64_t hi, uint32_t *ones, uint32_t *zeros)
{
  if ((lo < 0) != (hi < 0))
    return 0;
  uint32_t ulo = (uint32_t)lo, diff = ulo ^ (uint32_t)hi;
  uint32_t prefix = 0xFFFFFFFFu;
  while (diff)
  {
    prefix <<= 1;
    diff >>= 1;
  }
  *ones = ulo & prefix;
  *zeros = ~ulo & prefix;
  return 1;
}

/* `d <- x AND #k` / `d <- x OR #k` whose mask cannot change any value of x's range is
 * `d <- x`.  zig.h's full-width signed truncate (x & SIGN) ? x | MIN : x & MAX is exactly
 * this once the branch edge bounds x to one sign (sv_enter_and_test). */
static int sv_try_fold_mask_identity(IRSSAOptCtx *ctx, SVState *s, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  int op = q->op;
  if (op != TCCIR_OP_AND && op != TCCIR_OP_OR && op != TCCIR_OP_SBFX && op != TCCIR_OP_UBFX)
    return 0;
  if (tcc_ir_barrel_shift_at(ir, q))
    return 0;
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  if (dest.is_lval || irop_is_sym(dest) || irop_is_immediate(src1) || !irop_is_plain_imm(src2))
    return 0;
  if (dest.btype == IROP_BTYPE_INT64 && src1.btype == IROP_BTYPE_INT64 && op != TCCIR_OP_SBFX &&
      op != TCCIR_OP_UBFX)
  {
    /* A known sign decides only the sign-bit masks: x >= 0 keeps x & (INT64_MAX | anything),
     * x < 0 keeps x | INT64_MIN. */
    int sl = sv_operand_slot(s, src1);
    uint64_t uk = (uint64_t)irop_get_imm64_ex(ir, src2), sign = (uint64_t)1 << 63;
    int known = sl >= 0 ? s->ranges[sl].sign64 : 0;
    if (op == TCCIR_OP_AND ? !(known > 0 && (uk | sign) == ~(uint64_t)0)
                           : !(known < 0 && uk == sign))
      return 0;
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src2_none(ir, idx);
    return 1;
  }
  int64_t k, lo, hi;
  if (!sv_read_const32(ir, src2, &k) || !sv_operand_range(s, src1, &lo, &hi))
    return 0;
  if (op == TCCIR_OP_SBFX || op == TCCIR_OP_UBFX)
  {
    /* A bit-0 field as wide as every value of x: (intN_t)x of an x that already fits.
     * The copy keeps the bits, so dest must read them the same way: same btype. */
    int width = (int)((k >> 5) & 0x1F);
    if ((k & 0x1F) != 0 || (k & UBFX_HI_HALF) || width == 0 || dest.btype != src1.btype ||
        dest.is_unsigned != src1.is_unsigned)
      return 0;
    int64_t flo = op == TCCIR_OP_SBFX ? -((int64_t)1 << (width - 1)) : 0;
    int64_t fhi = op == TCCIR_OP_SBFX ? ((int64_t)1 << (width - 1)) - 1 : ((int64_t)1 << width) - 1;
    if (lo < flo || hi > fhi)
      return 0;
  }
  else
  {
    uint32_t ones, zeros, uk = (uint32_t)k;
    if (dest.btype != IROP_BTYPE_INT32 || src1.btype != IROP_BTYPE_INT32 ||
        !sv_range_known_bits(lo, hi, &ones, &zeros))
      return 0;
    if (op == TCCIR_OP_AND ? (~uk & ~zeros) != 0 : (uk & ~ones) != 0)
      return 0;
  }
  q->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src2_none(ir, idx);
  return 1;
}

/* Seed dest's range from a value-producing def; SSA is single-def so no kill is needed. */
static void sv_seed_def(SVState *s, IRQuadCompact *q)
{
  TCCIRState *ir = s->ctx->ir;
  int op = q->op;
  if (!irop_config[op].has_dest)
    return;
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  if (dest.is_lval || irop_is_sym(dest))
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
  {
    sv_seed_bitop(s, q, dslot, op, src1, src2);
    if (op == TCCIR_OP_SAR)
      sv_seed_sar_width(s, q, dslot, src1, src2);
  }
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
  /* Deciding the compare must not delete the reads it is made of: a volatile access is
   * mandated however decidable the comparison is (`x >=u 0` still reads x). */
  if (tcc_ir_instr_access_is_volatile(ir, &ir->compact_instructions[cmp_idx]))
    return -1;
  /* A deref/sym x yields slot = -1 (the base vreg's range describes the wrong value), which
   * disables only the range fold; the tautology-vs-0 path below holds for any value. */
  if (sv_read_cmp(s, cmp_idx, cmp))
  {
    if (cmp->swapped && (tok = vrp_swap_cmp_tok(tok)) < 0)
      return -1;
    int verdict = -1;
    if (irop_get_imm64_ex(ir, cmp->c_op) == 0)   /* tautology vs 0, no range needed */
    {
      if (tok == VRP_TOK_UGE) verdict = 1;
      else if (tok == VRP_TOK_ULT) verdict = 0;
    }
    /* ... and vs the other ends of the 32-bit compare: nothing is above
     * 0xFFFFFFFF unsigned, below INT32_MIN or above INT32_MAX signed.  zig.h
     * checks every __builtin_*_overflow result against zig_minInt/zig_maxInt
     * of its width, which inlining makes exactly these at full width. */
    if (verdict < 0 && cmp->c_ok && cmp->x.btype != IROP_BTYPE_INT64)
    {
      if (cmp->c == -1 && (tok == VRP_TOK_UGT || tok == VRP_TOK_ULE))
        verdict = tok == VRP_TOK_ULE;
      else if (cmp->c == INT32_MIN && (tok == VRP_TOK_LT || tok == VRP_TOK_GE))
        verdict = tok == VRP_TOK_GE;
      else if (cmp->c == INT32_MAX && (tok == VRP_TOK_GT || tok == VRP_TOK_LE))
        verdict = tok == VRP_TOK_LE;
    }
    if (verdict < 0 && cmp->slot >= 0 && s->ranges[cmp->slot].nz && cmp->c_ok && cmp->c == 0 &&
        cmp->x.btype == IROP_BTYPE_INT32)
    {
      if (tok == VRP_TOK_EQ || tok == VRP_TOK_ULE)
        verdict = 0;
      else if (tok == VRP_TOK_NE || tok == VRP_TOK_UGT)
        verdict = 1;
    }
    if (verdict < 0 && cmp->slot >= 0 && s->ranges[cmp->slot].valid && cmp->c_ok)
      verdict = sv_range_verdict(s->ranges[cmp->slot].lo, s->ranges[cmp->slot].hi, cmp->c, tok);
    /* A phi whose operands all exclude c decides `x == c` even when the interval hull cannot. */
    if (verdict < 0 && cmp->c_ok && (tok == VRP_TOK_EQ || tok == VRP_TOK_NE))
    {
      IRPhiNode *phi = sv_phi_for(s, cmp->xvr);
      if (phi && !sv_phi_is_loop_phi(s, phi, cmp->xvr) &&
          sv_phi_excludes_const(s, phi, cmp->c))
        verdict = (tok == VRP_TOK_NE) ? 1 : 0;
    }
    return verdict;
  }
  /* Register rhs: decide from the two operands' intervals. */
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
  if (cq->op != TCCIR_OP_CMP)
    return -1;
  /* Same barrel-shift bail as sv_read_cmp: b's range would describe the
   * UNshifted register. */
  if (tcc_ir_barrel_shift_at(ir, cq))
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
  int tok = (int)tcc_ir_op_src1_imm(ir, jq);

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
  return vx >= 0 && vx == irop_get_vreg(y) && x.is_lval == y.is_lval && irop_is_sym(x) == irop_is_sym(y) &&
         irop_is_llocal(x) == irop_is_llocal(y) && irop_is_local(x) == irop_is_local(y);
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

  /* Only the pred's JUMPIF runs between the two CMPs — no store — so even non-volatile lval
   * operands hold identical values at both, provided the full operand identity (vreg + flags)
   * matches. */
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

  /* Each volatile read is a separate mandated access, so the second CMP does not repeat the
   * first one's value, and folding it would delete the second read. */
  if (tcc_ir_instr_access_is_volatile(ir, cq) || tcc_ir_instr_access_is_volatile(ir, pcq))
    return 0;

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

  int known = vrp_negate_cmp_tok((int)tcc_ir_op_src1_imm(ir, pjq));
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
  int tok = (int)tcc_ir_op_src1_imm(ir, sq);

  SVCmp cmp;
  int32_t b_vr;
  int v = sv_cmp_verdict(s, cmp_idx, tok, &cmp, &b_vr);
  if (v < 0 || sv_next_reads_flags(ir, set_idx))
    return 0;

  IROperand set_dest = tcc_ir_op_get_dest(ir, sq);
  cq->op = TCCIR_OP_NOP;
  sq->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src1_imm32(ir, set_idx, v, IROP_BTYPE_INT32);
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
  int tok = (int)tcc_ir_op_cond_imm(ir, sq);

  SVCmp cmp;
  int32_t b_vr;
  int v = sv_cmp_verdict(s, cmp_idx, tok, &cmp, &b_vr);
  if (v < 0 || sv_next_reads_flags(ir, sel_idx))
    return 0;

  IROperand chosen = tcc_ir_op_get_src1_or_2(ir, sq, !v);
  IROperand dropped = tcc_ir_op_get_src1_or_2(ir, sq, v);
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

/* An edge of `T <- x AND #k ; T ==/!= 0` (CMP T,#0 or TEST_ZERO T) bounds x.
 * 32-bit x: when k reaches x's sign bit m (the top bit of k, with x in [-2^m, 2^m-1];
 * m = 31 always qualifies), `== 0` clears the sign and every bit of k, so x is in
 * [0, ~k & (2^m-1)]; `!= 0` with k == 2^m sets it, x in [-2^m,-1].
 * 64-bit x has no range, only a sign: `== 0` with bit 63 in k is x >= 0, `!= 0` with
 * k == bit 63 alone is x < 0. */
static void sv_enter_and_test(SVState *s, int cmp_idx, int eff_tok)
{
  if (eff_tok != VRP_TOK_EQ && eff_tok != VRP_TOK_NE)
    return;
  TCCIRState *ir = s->ctx->ir;
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
  if (tcc_ir_barrel_shift_at(ir, cq))
    return;
  IROperand t = tcc_ir_op_get_src1(ir, cq);
  if (cq->op == TCCIR_OP_CMP)
  {
    IROperand c = tcc_ir_op_get_src2(ir, cq);
    if (!irop_is_plain_imm(c) || irop_get_imm64_ex(ir, c) != 0)
      return;
  }
  int32_t tvr = irop_get_vreg(t);
  int is64 = t.btype == IROP_BTYPE_INT64;
  if (t.is_lval || irop_is_sym(t) || (t.btype != IROP_BTYPE_INT32 && !is64) ||
      tvr < 0 || TCCIR_DECODE_VREG_TYPE(tvr) != TCCIR_VREG_TYPE_TEMP)
    return;
  IRSSAVregInfo *vi = ssa_opt_vinfo(s->ctx, tvr);
  if (!vi || vi->def_instr < 0 || ssa_opt_def_total(vi) != 1)
    return;
  IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
  if (dq->op != TCCIR_OP_AND || tcc_ir_barrel_shift_at(ir, dq))
    return;
  IROperand x = tcc_ir_op_get_src1(ir, dq), kop = tcc_ir_op_get_src2(ir, dq);
  if (x.btype != t.btype || tcc_ir_op_get_dest(ir, dq).btype != t.btype || !irop_is_plain_imm(kop))
    return;
  int xslot = sv_operand_slot(s, x);
  if (xslot < 0)
    return;
  if (is64)
  {
    uint64_t uk = (uint64_t)irop_get_imm64_ex(ir, kop), sign = (uint64_t)1 << 63;
    if (eff_tok == VRP_TOK_EQ && (uk & sign))
      sv_set_sign64(s, xslot, 1);
    else if (eff_tok == VRP_TOK_NE && uk == sign)
      sv_set_sign64(s, xslot, -1);
    return;
  }
  int64_t k;
  if (!sv_read_const32(ir, kop, &k) || k == 0)
    return;
  int64_t xlo = INT32_MIN, xhi = INT32_MAX;
  if (s->ranges[xslot].valid)
  {
    xlo = s->ranges[xslot].lo;
    xhi = s->ranges[xslot].hi;
  }
  uint32_t uk = (uint32_t)k;
  int m = 31;
  while (!(uk >> m))
    m--;
  int64_t sign = (int64_t)1 << m;
  if (xlo < -sign || xhi > sign - 1)
    return;   /* bit m is not x's sign: x & k says nothing interval-shaped */
  if (eff_tok == VRP_TOK_EQ)
    sv_set_intersect(s, xslot, 0, (int64_t)(~uk & (uint32_t)(sign - 1)));
  else if (uk == (uint32_t)sign)
    sv_set_intersect(s, xslot, -sign, -1);
}

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

  int tok = (int)tcc_ir_op_src1_imm(ir, jq);

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
  sv_enter_and_test(s, cmp_idx, eff_tok);

  SVCmp cmp;
  if (!sv_read_cmp(s, cmp_idx, &cmp) || !cmp.c_ok)
    return 0;
  /* `CMP #c,x`: the token reads (c, x); swapping commutes with the edge negation above. */
  if (cmp.swapped && (eff_tok = vrp_swap_cmp_tok(eff_tok)) < 0)
    return 0;
  int64_t lo, hi;
  if (!sv_interval_for(eff_tok, cmp.c, &lo, &hi))
  {
    /* `x != 0`, `x >u c`, `x >=u c` (c != 0): x is non-zero.  Word compares
     * only: a narrow read of the same value could still be zero. */
    if (cmp.slot >= 0 && cmp.x.btype == IROP_BTYPE_INT32 &&
        ((eff_tok == VRP_TOK_NE && cmp.c == 0) || eff_tok == VRP_TOK_UGT ||
         (eff_tok == VRP_TOK_UGE && cmp.c != 0)))
      sv_set_nonzero(s, cmp.slot);
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
    if (phi && !sv_phi_is_loop_phi(s, phi, cmp.xvr))
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
    if (sv_phi_is_loop_phi(s, p, p->dest_vreg))
      continue; /* backedge operand ranges describe the WRONG iteration */
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

/* ------------------------------------------------------- induction-variable bounds */

static int sv_same_value_d(SVState *s, IROperand x, IROperand y, int depth);
static int sv_def_past_copies(SVState *s, int32_t *vr);

/* Two TEMPs, each defined once by the same pure operation on operands with
 * the same values: one value (a yasc slice's `s.len` is `s >> 32` wherever
 * it is read, each read a TEMP of its own). */
static int sv_same_def(SVState *s, int32_t a, int32_t b, int depth)
{
  TCCIRState *ir = s->ctx->ir;
  if (depth > 4 || a < 0 || b < 0 || TCCIR_DECODE_VREG_TYPE(a) != TCCIR_VREG_TYPE_TEMP ||
      TCCIR_DECODE_VREG_TYPE(b) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  /* through copies first: `T1 <- T0` is T0 */
  sv_def_past_copies(s, &a);
  sv_def_past_copies(s, &b);
  if (a == b)
    return 1;
  if (a < 0 || b < 0 || TCCIR_DECODE_VREG_TYPE(a) != TCCIR_VREG_TYPE_TEMP || TCCIR_DECODE_VREG_TYPE(b) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  IRSSAVregInfo *va = ssa_opt_vinfo(s->ctx, a), *vb = ssa_opt_vinfo(s->ctx, b);
  if (!va || !vb || va->def_instr < 0 || vb->def_instr < 0 || ssa_opt_def_total(va) != 1 || ssa_opt_def_total(vb) != 1)
    return 0;
  IRQuadCompact *qa = &ir->compact_instructions[va->def_instr], *qb = &ir->compact_instructions[vb->def_instr];
  if (qa->op != qb->op || tcc_ir_barrel_shift_at(ir, qa) || tcc_ir_barrel_shift_at(ir, qb))
    return 0;
  switch (qa->op)
  {
  case TCCIR_OP_ADD: case TCCIR_OP_SUB: case TCCIR_OP_AND: case TCCIR_OP_OR: case TCCIR_OP_XOR:
  case TCCIR_OP_SHL: case TCCIR_OP_SHR: case TCCIR_OP_SAR: case TCCIR_OP_ASSIGN:
    break;
  default:
    return 0;
  }
  IROperand da = tcc_ir_op_get_dest(ir, qa), db = tcc_ir_op_get_dest(ir, qb);
  if (da.btype != db.btype || da.is_unsigned != db.is_unsigned)
    return 0;
  IROperand a1 = tcc_ir_op_get_src1(ir, qa), b1 = tcc_ir_op_get_src1(ir, qb);
  if (a1.is_lval || b1.is_lval || !sv_same_value_d(s, a1, b1, depth + 1))
    return 0;
  if (qa->op == TCCIR_OP_ASSIGN)
    return 1;
  IROperand a2 = tcc_ir_op_get_src2(ir, qa), b2 = tcc_ir_op_get_src2(ir, qb);
  return !a2.is_lval && !b2.is_lval && sv_same_value_d(s, a2, b2, depth + 1);
}

/* Two operands with the same value: one vreg, one constant, or one
   computation (sv_same_def). */
static int sv_same_value_d(SVState *s, IROperand x, IROperand y, int depth)
{
  int64_t xl, xh, yl, yh;
  if (x.btype == IROP_BTYPE_INT64 || y.btype == IROP_BTYPE_INT64)
  {
    /* a 64-bit operand: the same register or the same constant only */
    if (irop_is_immediate(x) && irop_is_immediate(y))
      return irop_get_imm64_ex(s->ctx->ir, x) == irop_get_imm64_ex(s->ctx->ir, y);
    return sv_same_operand(x, y) || (!x.is_lval && !y.is_lval && sv_same_def(s, irop_get_vreg(x), irop_get_vreg(y), depth));
  }
  if (sv_same_operand(x, y))
    return 1;
  if (sv_operand_range(s, x, &xl, &xh) && sv_operand_range(s, y, &yl, &yh) && xl == xh && yl == yh && xl == yl)
    return 1;
  return !x.is_lval && !y.is_lval && !irop_is_immediate(x) && !irop_is_immediate(y) &&
         sv_same_def(s, irop_get_vreg(x), irop_get_vreg(y), depth);
}

static int sv_same_value(SVState *s, IROperand x, IROperand y)
{
  return sv_same_value_d(s, x, y, 0);
}

/* A loop bound Y that is one value through the whole loop at header H: a
 * constant, a never-written PARAM, or a TEMP defined before H. */
static int sv_loop_invariant(SVState *s, IROperand y, int header)
{
  IRCFG *cfg = s->ctx->cfg;
  int32_t vr;
  if (irop_is_immediate(y))
    return irop_is_plain_imm(y);
  if (sv_operand_slot(s, y) < 0)
    return 0;
  vr = irop_get_vreg(y);
  /* a stable VAR's one store may sit inside the loop */
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
  {
    IRSSAVregInfo *vi = ssa_opt_vinfo(s->ctx, vr);
    int b = !vi ? -1 : vi->def_phi_block >= 0 ? vi->def_phi_block
                     : vi->def_instr >= 0 ? cfg->instr_to_block[vi->def_instr] : -1;
    if (b < 0 || b == header || !tcc_ir_cfg_dominates(cfg, b, header))
      return 0;
  }
  return 1;
}

/* The compare that an edge PRED -> BLOCK is taken under, read as `v TOK y`
 * for the value V: 1 and *y, *tok, or 0 when the edge is not conditional on
 * such a compare. */
static int sv_edge_cmp(SVState *s, int pred, int block, IROperand v, IROperand *y, int *tok)
{
  TCCIRState *ir = s->ctx->ir;
  IRBasicBlock *pb = &s->ctx->cfg->blocks[pred];
  int target_block, ft_block, t;
  int term = sv_last_real(ir, pb->start_idx, pb->end_idx);
  if (term < 0 || ir->compact_instructions[term].op != TCCIR_OP_JUMPIF)
    return 0;
  int cmp = sv_last_real(ir, pb->start_idx, term);
  IRQuadCompact *cq = cmp >= 0 ? &ir->compact_instructions[cmp] : NULL;
  if (!cq || cq->op != TCCIR_OP_CMP || tcc_ir_barrel_shift_at(ir, cq) ||
      tcc_ir_instr_access_is_volatile(ir, cq))
    return 0;
  sv_jumpif_edges(s->ctx, term, &target_block, &ft_block);
  if (target_block == ft_block)
    return 0;
  t = (int)tcc_ir_op_src1_imm(ir, &ir->compact_instructions[term]);
  if (block == ft_block)
    t = vrp_negate_cmp_tok(t);
  else if (block != target_block)
    return 0;
  IROperand a = tcc_ir_op_get_src1(ir, cq), b = tcc_ir_op_get_src2(ir, cq);
  if (a.btype != IROP_BTYPE_INT32 || b.btype != IROP_BTYPE_INT32)
    return 0;
  if (sv_same_value(s, a, v))
    *y = b;
  else if (sv_same_value(s, b, v))
  {
    *y = a;
    t = vrp_swap_cmp_tok(t);
  }
  else
    return 0;
  *tok = t;
  return t >= 0;
}

/* An induction variable against its loop's own bound.  x = phi(init, next)
 * at the header H, next = x + 1, and each edge into H is taken only under
 * `v < Y` for the value v it brings (init on the way in, next on the back
 * edge; both signed or both unsigned): every x then is below Y, so x + 1
 * never wraps and x only grows from init.  Signed with init >= 0, or
 * unsigned: x <u Y.  Decides the CMP x,Y / CMP x,#K (K >= Y) of a check like
 * -fsanitize=bounds' `i <u n` for `for (i = 0; i < n; i++) a[i]`, a[n]. */
static int sv_try_fold_iv_bound(IRSSAOptCtx *ctx, SVState *s, int cmp_idx, int jmp_idx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
  int tok = (int)tcc_ir_op_src1_imm(ir, &ir->compact_instructions[jmp_idx]);
  if (cq->op != TCCIR_OP_CMP || tok < 0 || tcc_ir_barrel_shift_at(ir, cq) ||
      tcc_ir_instr_access_is_volatile(ir, cq) || sv_next_reads_flags(ir, jmp_idx))
    return 0;
  IROperand x = tcc_ir_op_get_src1(ir, cq), k = tcc_ir_op_get_src2(ir, cq);
  IRPhiNode *phi = NULL;
  for (int side = 0; side < 2 && !phi; side++)
  {
    if (side)
    {
      IROperand t = x;
      x = k;
      k = t;
      tok = vrp_swap_cmp_tok(tok);
      if (tok < 0)
        return 0;
    }
    if (x.btype == IROP_BTYPE_INT32 && !x.is_lval && !irop_is_immediate(x) && irop_get_vreg(x) >= 0 &&
        TCCIR_DECODE_VREG_TYPE(irop_get_vreg(x)) == TCCIR_VREG_TYPE_TEMP)
      phi = sv_phi_for(s, irop_get_vreg(x));
  }
  if (!phi || phi->num_operands != 2 || k.btype != IROP_BTYPE_INT32)
    return 0;
  int32_t xvr = irop_get_vreg(x);
  IRSSAVregInfo *xvi = ssa_opt_vinfo(ctx, xvr);
  if (!xvi || ssa_opt_def_total(xvi) != 1)
    return 0;
  int h = xvi->def_phi_block;
  int back = -1, entry = -1;
  for (int i = 0; i < 2; i++)
  {
    int p = phi->operands[i].pred_block;
    if (p < 0 || p >= cfg->num_blocks)
      return 0;
    if (tcc_ir_cfg_dominates(cfg, h, p))
      back = i;
    else
      entry = i;
  }
  if (back < 0 || entry < 0)
    return 0;

  /* next = x + 1 */
  int32_t nvr = phi->operands[back].vreg;
  IRSSAVregInfo *nvi = nvr >= 0 ? ssa_opt_vinfo(ctx, nvr) : NULL;
  if (!nvi || nvi->def_instr < 0 || ssa_opt_def_total(nvi) != 1)
    return 0;
  IRQuadCompact *nq = &ir->compact_instructions[nvi->def_instr];
  if (nq->op != TCCIR_OP_ADD || tcc_ir_barrel_shift_at(ir, nq))
    return 0;
  IROperand n1 = tcc_ir_op_get_src1(ir, nq), n2 = tcc_ir_op_get_src2(ir, nq), one;
  if (sv_same_operand(n1, x))
    one = n2;
  else if (sv_same_operand(n2, x))
    one = n1;
  else
    return 0;
  int64_t c, lo, hi;
  if (!irop_is_immediate(one) || !irop_is_plain_imm(one) || !sv_read_const32(ir, one, &c) || c != 1)
    return 0;
  IROperand next = tcc_ir_op_get_dest(ir, nq), init;
  init = irop_make_vreg(phi->operands[entry].vreg, IROP_BTYPE_INT32);

  /* the back edge: next < Y */
  IROperand y, y2;
  int btok, etok;
  if (!sv_edge_cmp(s, phi->operands[back].pred_block, h, next, &y, &btok) || (btok != VRP_TOK_LT && btok != VRP_TOK_ULT))
    return 0;
  if (!sv_loop_invariant(s, y, h))
    return 0;
  /* the way in: init < Y, compared on the edge or both constants */
  if (sv_edge_cmp(s, phi->operands[entry].pred_block, h, init, &y2, &etok))
  {
    if (etok != btok || !sv_same_value(s, y, y2))
      return 0;
  }
  else
  {
    int64_t yl, yh;
    if (!sv_operand_range(s, init, &lo, &hi) || !sv_operand_range(s, y, &yl, &yh) || lo != hi || yl != yh)
      return 0;
    if (btok == VRP_TOK_LT ? !(lo < yl) : !((uint32_t)lo < (uint32_t)yl))
      return 0;
  }
  /* x <u Y: unsigned guards, or signed ones from a non-negative start */
  if (btok == VRP_TOK_LT && (!sv_operand_range(s, init, &lo, &hi) || lo < 0))
    return 0;

  /* the check: x against Y itself, or against a constant at or above a constant Y */
  int verdict = -1;
  int64_t kl, kh, yl, yh;
  int above = sv_same_value(s, k, y) ||
              (sv_operand_range(s, k, &kl, &kh) && sv_operand_range(s, y, &yl, &yh) && kl == kh && yl == yh &&
               (uint32_t)kl >= (uint32_t)yl && kl >= yl);
  if (!above)
    return 0;
  /* signed guards (from a start >= 0) give x <s Y and x <u Y; unsigned ones
     only x <u Y (x = 5, Y = 0x80000000) */
  switch (tok)
  {
  case VRP_TOK_ULT: case VRP_TOK_ULE: case VRP_TOK_NE:
    verdict = 1;
    break;
  case VRP_TOK_UGE: case VRP_TOK_UGT: case VRP_TOK_EQ:
    verdict = 0;
    break;
  case VRP_TOK_LT: case VRP_TOK_LE:
    verdict = btok == VRP_TOK_LT ? 1 : -1;
    break;
  case VRP_TOK_GE: case VRP_TOK_GT:
    verdict = btok == VRP_TOK_LT ? 0 : -1;
    break;
  default:
    return 0;
  }
  if (verdict < 0)
    return 0;
  sv_commit_jumpif(ctx, cmp_idx, jmp_idx, verdict);
  sv_drop_cmp_use(ctx, irop_get_vreg(tcc_ir_op_get_src1(ir, cq)), cmp_idx);
  sv_drop_cmp_use(ctx, irop_get_vreg(tcc_ir_op_get_src2(ir, cq)), cmp_idx);
  return 1;
}

/* ---------------------------------------------- decreasing loops: v <u Y by chains */

#define SV_PROVE_DEPTH 8
/* sv_prove_below / sv_nonzero / sv_dec_iv: how deep one proof goes (dirname's
   end <= len: three loops after each other, an inlined function's return
   slot, ~12 levels), and how many steps it may take in all -- a phi joining
   phis would otherwise make a failing search exponential in the depth */
#define SV_CHAIN_DEPTH 16
#define SV_CHAIN_STEPS 512

static int sv_chain_step(SVState *s, int depth)
{
  if (depth == 0)
    s->chain_steps = SV_CHAIN_STEPS;
  return depth <= SV_CHAIN_DEPTH && --s->chain_steps >= 0;
}

/* The instruction defining vr, past plain copies (*vr follows them), or -1. */
static int sv_def_past_copies(SVState *s, int32_t *vr)
{
  TCCIRState *ir = s->ctx->ir;
  for (int k = 0; k < SV_PROVE_DEPTH; k++)
  {
    if (*vr < 0 || TCCIR_DECODE_VREG_TYPE(*vr) != TCCIR_VREG_TYPE_TEMP)
      return -1;
    IRSSAVregInfo *vi = ssa_opt_vinfo(s->ctx, *vr);
    if (!vi || vi->def_instr < 0 || ssa_opt_def_total(vi) != 1)
      return -1;
    IRQuadCompact *q = &ir->compact_instructions[vi->def_instr];
    IROperand src = tcc_ir_op_get_src1(ir, q);
    if (q->op == TCCIR_OP_ASSIGN && src.tag == IROP_TAG_VREG && !src.is_lval && !irop_is_local(src) &&
        !irop_is_llocal(src) && src.btype == IROP_BTYPE_INT32 && irop_get_vreg(src) >= 0)
    {
      *vr = irop_get_vreg(src);
      continue;
    }
    return vi->def_instr;
  }
  return -1;
}

static int sv_dec_iv(SVState *s, int32_t xvr, IROperand *init, int depth);

/* v != 0: a constant, a fact in scope (one about the value, so true
   wherever it is defined), or a decreasing loop's variable (always >= 1) */
static int sv_nonzero(SVState *s, IROperand v, int depth)
{
  int64_t lo, hi;
  IROperand init;
  if (sv_operand_range(s, v, &lo, &hi) && (lo > 0 || hi < 0))
    return 1;
  int slot = sv_operand_slot(s, v);
  if (slot >= 0 && s->ranges[slot].nz)
    return 1;
  int32_t vr = irop_get_vreg(v);
  if (vr < 0 || v.is_lval || irop_is_immediate(v) || !sv_chain_step(s, depth))
    return 0;
  sv_def_past_copies(s, &vr);
  return sv_dec_iv(s, vr, &init, depth + 1);
}

/* Is the edge PRED -> BLOCK taken only with v > 0 (`v >u 0`, `v != 0`,
 * `v >s 0`)?  Its own compare, or one on the single-predecessor edge into a
 * block on PRED's dominator chain (every way to PRED took it), or v known
 * non-zero. */
static int sv_edge_positive(SVState *s, int pred, int block, IROperand v, int depth)
{
  IRCFG *cfg = s->ctx->cfg;
  IROperand y;
  int tok, from = pred, to = block;
  for (int k = 0; k < SV_PROVE_DEPTH && from >= 0; k++)
  {
    int64_t c;
    if (sv_edge_cmp(s, from, to, v, &y, &tok) && irop_is_immediate(y) && irop_is_plain_imm(y) &&
        sv_read_const32(s->ctx->ir, y, &c) && c == 0 &&
        (tok == VRP_TOK_UGT || tok == VRP_TOK_NE || tok == VRP_TOK_GT))
      return 1;
    /* up PRED's dominator chain: the edge into a block with one predecessor */
    to = k == 0 ? pred : cfg->blocks[to].idom;
    if (to < 0 || cfg->blocks[to].num_preds != 1)
    {
      if (to < 0 || to == cfg->blocks[to].idom)
        break;
      from = -1;
      for (int j = 0; j < SV_PROVE_DEPTH && to >= 0 && cfg->blocks[to].num_preds != 1; j++)
        to = to == cfg->blocks[to].idom ? -1 : cfg->blocks[to].idom;
      if (to < 0)
        break;
    }
    from = cfg->blocks[to].preds[0];
  }
  return sv_nonzero(s, v, depth + 1);
}

/* A decreasing loop's variable: x = phi(init, next) at H, next = x - 1, and
 * every edge into H is taken only with the value it brings above 0.  Then x
 * is >= 1 at each trip, x - 1 never wraps, and x <=u init. */
static int sv_dec_iv(SVState *s, int32_t xvr, IROperand *init, int depth)
{
  IRCFG *cfg = s->ctx->cfg;
  TCCIRState *ir = s->ctx->ir;
  if (xvr < 0 || TCCIR_DECODE_VREG_TYPE(xvr) != TCCIR_VREG_TYPE_TEMP || !sv_chain_step(s, depth))
    return 0;
  IRPhiNode *phi = sv_phi_for(s, xvr);
  IRSSAVregInfo *xvi = ssa_opt_vinfo(s->ctx, xvr);
  if (!phi || phi->num_operands != 2 || !xvi || ssa_opt_def_total(xvi) != 1)
    return 0;
  int h = xvi->def_phi_block, back = -1, entry = -1;
  for (int i = 0; i < 2; i++)
  {
    int p = phi->operands[i].pred_block;
    if (p < 0 || p >= cfg->num_blocks)
      return 0;
    if (tcc_ir_cfg_dominates(cfg, h, p))
      back = i;
    else
      entry = i;
  }
  if (back < 0 || entry < 0)
    return 0;
  int32_t nvr = phi->operands[back].vreg;
  IROperand next = irop_make_vreg(nvr, IROP_BTYPE_INT32);
  int def = sv_def_past_copies(s, &nvr);
  if (def < 0)
    return 0;
  IRQuadCompact *nq = &ir->compact_instructions[def];
  if ((nq->op != TCCIR_OP_ADD && nq->op != TCCIR_OP_SUB) || tcc_ir_barrel_shift_at(ir, nq))
    return 0;
  IROperand a = tcc_ir_op_get_src1(ir, nq), b = tcc_ir_op_get_src2(ir, nq);
  int64_t c;
  if (!irop_is_immediate(b) || !irop_is_plain_imm(b) || !sv_read_const32(ir, b, &c) ||
      c != (nq->op == TCCIR_OP_ADD ? -1 : 1) || a.tag != IROP_TAG_VREG || a.is_lval)
    return 0;
  int32_t avr = irop_get_vreg(a);
  sv_def_past_copies(s, &avr);
  if (avr != xvr)
    return 0;
  *init = irop_make_vreg(phi->operands[entry].vreg, IROP_BTYPE_INT32);
  return sv_edge_positive(s, phi->operands[back].pred_block, h, next, depth + 1) &&
         sv_edge_positive(s, phi->operands[entry].pred_block, h, *init, depth + 1);
}

/* The writes of the VAR the LOAD at DEF reads, every one (-1 ends the
   list), or NULL when they are not all known */
static const int *sv_load_var_writes(SVState *s, int def)
{
  TCCIRState *ir = s->ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[def];
  if (!s->var_writes || q->op != TCCIR_OP_LOAD || tcc_ir_instr_access_is_volatile(ir, q))
    return NULL;
  IROperand src = tcc_ir_op_get_src1(ir, q);
  int32_t vr = irop_get_vreg(src);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR || !src.is_lval || !irop_is_local(src) ||
      irop_is_llocal(src) || src.btype != IROP_BTYPE_INT32)
    return NULL;
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (pos < 0 || pos >= s->var_cap || s->var_writes[pos * SV_VAR_WRITES] < 0)
    return NULL;
  return &s->var_writes[pos * SV_VAR_WRITES];
}

static IROperand sv_write_value(SVState *s, int w)
{
  IROperand v = tcc_ir_op_get_src1(s->ctx->ir, &s->ctx->ir->compact_instructions[w]);
  if (!irop_is_immediate(v) && !v.is_lval && irop_get_vreg(v) >= 0)
    v.btype = IROP_BTYPE_INT32;
  return v;
}

/* v <u y (STRICT) or v <=u y, from v's definition: a decreasing loop's
 * variable is at most its start, `w - 1` with w != 0 is below w, a phi
 * joining values is below y when each one is. */
static int sv_prove_below(SVState *s, IROperand v, IROperand y, int strict, int depth)
{
  int64_t vl, vh, yl, yh;
  if (v.btype != IROP_BTYPE_INT32)
    return 0;
  /* what needs no search first: a chain's last step is often one of these,
     one level past where the search stops (dirname: three loops, then an
     inlined function's return slot) */
  if (!strict && sv_same_value(s, v, y))
    return 1;
  int64_t vc;
  if (!strict && irop_is_immediate(v) && irop_is_plain_imm(v) && sv_read_const32(s->ctx->ir, v, &vc) && vc == 0)
    return 1; /* 0 <=u anything */
  if (sv_operand_range(s, v, &vl, &vh) && sv_operand_range(s, y, &yl, &yh) && vl >= 0 && yl >= 0 &&
      (strict ? vh < yl : vh <= yl))
    return 1;
  if (!sv_chain_step(s, depth))
    return 0;
  int32_t vr = irop_get_vreg(v);
  if (vr < 0 || v.is_lval || irop_is_immediate(v))
    return 0;
  int def = sv_def_past_copies(s, &vr);
  IROperand init;
  if (def < 0)
  {
    IRPhiNode *phi = sv_phi_for(s, vr);
    if (!phi)
      return 0;
    if (sv_dec_iv(s, vr, &init, depth + 1))
      return sv_prove_below(s, init, y, strict, depth + 1);
    if (sv_phi_is_loop_phi(s, phi, vr) || phi->num_operands <= 0)
      return 0;
    for (int i = 0; i < phi->num_operands; i++)
      if (!sv_prove_below(s, irop_make_vreg(phi->operands[i].vreg, IROP_BTYPE_INT32), y, strict, depth + 1))
        return 0;
    return 1;
  }
  TCCIRState *ir = s->ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[def];
  /* a VAR's value: below y when everything ever stored there is (an inlined
     function's return slot: dirname's `end`) */
  const int *w = sv_load_var_writes(s, def);
  if (w)
  {
    for (int k = 0; k < SV_VAR_WRITES && w[k] >= 0; k++)
    {
      IROperand wv = sv_write_value(s, w[k]);
      if (wv.is_lval || !sv_prove_below(s, wv, y, strict, depth + 1))
        return 0;
    }
    return 1;
  }
  if ((q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB) || tcc_ir_barrel_shift_at(ir, q))
    return 0;
  IROperand a = tcc_ir_op_get_src1(ir, q), b = tcc_ir_op_get_src2(ir, q);
  int64_t c;
  if (!irop_is_immediate(b) || !irop_is_plain_imm(b) || !sv_read_const32(ir, b, &c) || a.is_lval ||
      a.btype != IROP_BTYPE_INT32)
    return 0;
  /* w + 1 <= y when w <u y: no wrap, y is at most UINT_MAX (dirname's
     `end = i + 1` against len, i below len) */
  if (!strict && c == (q->op == TCCIR_OP_ADD ? 1 : -1))
    return sv_prove_below(s, a, y, 1, depth + 1);
  if (c != (q->op == TCCIR_OP_ADD ? -1 : 1))
    return 0;
  /* w - 1 < w <= y */
  return sv_nonzero(s, a, depth + 1) && sv_prove_below(s, a, y, 0, depth + 1);
}

/* ---- the zero strnlen found ---------------------------------------------
 * r = strnlen(p, m) is at most m, and below m only at a zero: p[r] == 0.  A
 * byte load p[i] reads that zero when i is r on a path where `r != m` (or
 * `r <u m`) held and nothing wrote memory between the call and the load
 * (yasc's `p[0:n:0]` after n = strnlen(p, max): path lengths).  r = strlen(p)
 * is always at a zero: no test needed, only no write since (libgen). */
#define SV_WRITES (IR_HZ_MEM_WRITE | IR_HZ_CALL | IR_HZ_ASM | IR_HZ_NONLOCAL | IR_HZ_UPDATES_SRC | \
                   IR_HZ_DEST_LVAL | IR_HZ_DEST_STACKOFF | IR_HZ_CHAIN | IR_HZ_VLA)

/* A write into a VAR whose address is never taken: no pointer reaches it */
static int sv_private_var_write(SVState *s, IRQuadCompact *q)
{
  TCCIRState *ir = s->ctx->ir;
  if (!s->var_addr || (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_ASSIGN))
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  int32_t vr = irop_get_vreg(d);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR || irop_is_llocal(d) ||
      (d.is_lval && !irop_is_local(d)))
    return 0;
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  return pos >= 0 && pos < s->var_cap && !s->var_addr[pos];
}

/* Any instruction in [lo, hi) that may write memory a pointer reaches */
static int sv_writes_in(SVState *s, int lo, int hi)
{
  TCCIRState *ir = s->ctx->ir;
  for (int k = lo; k < hi; k++)
  {
    IRQuadCompact *q = &ir->compact_instructions[k];
    if (q->op != TCCIR_OP_NOP && ir_q_hazards(ir, q, SV_WRITES) && !sv_private_var_write(s, q))
      return 1;
  }
  return 0;
}

/* From instruction AT (exclusive) in block TO up to instruction FROM_IDX
 * (exclusive) in block FROM, along predecessors when each block on the way
 * has one (the only path): 1 when it gets there with no write on the way.
 * R, M (R's vreg >= 0): one edge on the way also has to be taken under
 * `r != m` or `r <u m`. */
static int sv_clean_path(SVState *s, int from, int from_idx, int to, int at, IROperand r, IROperand m)
{
  IRCFG *cfg = s->ctx->cfg;
  int fact = irop_get_vreg(r) < 0, b = to, end = at;
  for (int k = 0; k < 4 * SV_PROVE_DEPTH; k++)
  {
    IRBasicBlock *bb = &cfg->blocks[b];
    if (b == from)
      return from_idx < end && !sv_writes_in(s, from_idx + 1, end) && fact;
    if (sv_writes_in(s, bb->start_idx, end) || bb->num_preds != 1)
      return 0;
    int pred = bb->preds[0];
    IROperand y;
    int tok;
    if (!fact && sv_edge_cmp(s, pred, b, r, &y, &tok) && (tok == VRP_TOK_NE || tok == VRP_TOK_ULT) &&
        sv_same_value(s, y, m))
      fact = 1;
    b = pred;
    end = cfg->blocks[b].end_idx;
  }
  return 0;
}

/* VR (past copies) is strnlen(BASE, m)'s result (1; *M its m) or
   strlen(BASE)'s (2: the zero needs no test); *CALL its call */
static int sv_strnlen_of(SVState *s, int32_t vr, IROperand base, int *call, IROperand *m)
{
  TCCIRState *ir = s->ctx->ir;
  int def = sv_def_past_copies(s, &vr);
  if (def < 0 || ir->compact_instructions[def].op != TCCIR_OP_FUNCCALLVAL)
    return 0;
  Sym *callee = tcc_ir_op_src1_sym(ir, &ir->compact_instructions[def]);
  const char *name = callee ? get_tok_str(callee->v, NULL) : NULL;
  if (!name)
    return 0;
  int id = resolve_str_builtin_id(callee->v, name), kind;
  if (!strcmp(name, "__tcc_strnlen") || id == STRBI_STRNLEN)
    kind = 1;
  else if (!strcmp(name, "__tcc_strlen") || id == STRBI_STRLEN)
    kind = 2;
  else
    return 0;
  IROperand p;
  if (!ir_opt_get_call_param_operand(ir, def, 0, &p) || !sv_same_value(s, p, base))
    return 0;
  if (kind == 1 && !ir_opt_get_call_param_operand(ir, def, 1, m))
    return 0;
  if (kind == 2)
    *m = irop_make_vreg(-1, IROP_BTYPE_INT32);
  *call = def;
  return kind;
}

/* The write at W (block WB) stores strnlen(base, m)'s result with the fact
   on its way and nothing written since the call, or a constant k that IX
   (read here) is not */
static int sv_strnlen_write(SVState *s, int w, int wb, IROperand base, IROperand ix)
{
  TCCIRState *ir = s->ctx->ir;
  IROperand v = sv_write_value(s, w), m;
  int64_t k, lo, hi;
  int call;
  if (irop_is_immediate(v))
    return irop_is_plain_imm(v) && sv_read_const32(ir, v, &k) &&
           ((k == 0 && sv_nonzero(s, ix, 0)) || (sv_operand_range(s, ix, &lo, &hi) && (k < lo || k > hi)));
  int kind;
  if (v.is_lval || !(kind = sv_strnlen_of(s, irop_get_vreg(v), base, &call, &m)))
    return 0;
  return sv_clean_path(s, s->ctx->cfg->instr_to_block[call], call, wb, w,
                       kind == 2 ? irop_make_vreg(-1, IROP_BTYPE_INT32) : v, m);
}

/* IX = the VAR's value read at VDEF (its writes VW): every write that may
   reach VDEF, the last in its block, with nothing written after it on the
   way -- up VDEF's single-predecessor chain to a write, or to a join whose
   every predecessor ends by writing the VAR (the phi SSA did not make) */
static int sv_strnlen_var(SVState *s, int vdef, const int *vw, IROperand base, IROperand ix)
{
  IRCFG *cfg = s->ctx->cfg;
  int b = cfg->instr_to_block[vdef], end = vdef;
  for (int step = 0; step < 4 * SV_PROVE_DEPTH; step++)
  {
    IRBasicBlock *bb = &cfg->blocks[b];
    int last = -1;
    for (int k = 0; k < SV_VAR_WRITES && vw[k] >= 0; k++)
      if (cfg->instr_to_block[vw[k]] == b && vw[k] < end && vw[k] > last)
        last = vw[k];
    if (last >= 0)
      return !sv_writes_in(s, last + 1, end) && sv_strnlen_write(s, last, b, base, ix);
    if (sv_writes_in(s, bb->start_idx, end))
      return 0;
    if (bb->num_preds == 1)
    {
      b = bb->preds[0];
      end = cfg->blocks[b].end_idx;
      continue;
    }
    if (bb->num_preds < 2)
      return 0;
    for (int j = 0; j < bb->num_preds; j++)
    {
      int p = bb->preds[j], pl = -1;
      for (int k = 0; k < SV_VAR_WRITES && vw[k] >= 0; k++)
        if (cfg->instr_to_block[vw[k]] == p && vw[k] > pl)
          pl = vw[k];
      if (pl < 0 || sv_writes_in(s, pl + 1, cfg->blocks[p].end_idx) || !sv_strnlen_write(s, pl, p, base, ix))
        return 0;
    }
    return 1;
  }
  return 0;
}

/* the load at LD (block LB): T <- base LOAD_INDEXED i, a byte, scale 0.
   1 when it reads the zero strnlen found */
static int sv_strnlen_zero(SVState *s, int lb, int ld)
{
  TCCIRState *ir = s->ctx->ir;
  IRCFG *cfg = s->ctx->cfg;
  IRQuadCompact *q = &ir->compact_instructions[ld];
  IROperand dest = tcc_ir_op_get_dest(ir, q), base = tcc_ir_op_get_src1(ir, q), ix = tcc_ir_op_get_src2(ir, q);
  IROperand sc = tcc_ir_op_get_scale(ir, q);
  int64_t scale;
  if (dest.btype != IROP_BTYPE_INT8 || tcc_ir_instr_access_is_volatile(ir, q) || base.is_lval || ix.is_lval ||
      ix.btype != IROP_BTYPE_INT32 || irop_get_vreg(ix) < 0 || irop_is_immediate(ix) ||
      (irop_is_immediate(sc) && (!sv_read_const32(ir, sc, &scale) || scale != 0)))
    return 0;
  IROperand m;
  IROperand none = irop_make_vreg(-1, IROP_BTYPE_INT32);
  int call, kind;
  if ((kind = sv_strnlen_of(s, irop_get_vreg(ix), base, &call, &m)))
    return sv_clean_path(s, cfg->instr_to_block[call], call, lb, ld,
                         kind == 2 ? none : irop_make_vreg(irop_get_vreg(ix), IROP_BTYPE_INT32), m);
  int32_t vr = irop_get_vreg(ix);
  int vdef = sv_def_past_copies(s, &vr);
  const int *vw = vdef >= 0 ? sv_load_var_writes(s, vdef) : NULL;
  if (vw)
    return sv_clean_path(s, cfg->instr_to_block[vdef], vdef, lb, ld, none, none) &&
           sv_strnlen_var(s, vdef, vw, base, ix);
  /* i = phi(r..., k...): each r strnlen's with the fact on its way in, each
     constant k not a value i has here (`if (len == 0) return`) */
  if (vdef >= 0 || vr < 0)
    return 0;
  IRPhiNode *phi = sv_phi_for(s, vr);
  IRSSAVregInfo *vi = ssa_opt_vinfo(s->ctx, vr);
  if (!phi || !vi || phi->num_operands <= 0 || sv_phi_is_loop_phi(s, phi, vr))
    return 0;
  int pb = vi->def_phi_block;
  if (pb < 0 || !sv_clean_path(s, pb, cfg->blocks[pb].start_idx - 1, lb, ld, none, none))
    return 0;
  int64_t lo, hi;
  int have_range = sv_operand_range(s, ix, &lo, &hi);
  int nonzero = sv_nonzero(s, ix, 0);
  for (int j = 0; j < phi->num_operands; j++)
  {
    int32_t ovr = phi->operands[j].vreg;
    int pred = phi->operands[j].pred_block;
    int64_t k;
    IROperand o = irop_make_vreg(ovr, IROP_BTYPE_INT32);
    int odef = ovr;
    int d = ovr >= 0 ? sv_def_past_copies(s, &odef) : -1;
    if (d >= 0 && ir->compact_instructions[d].op == TCCIR_OP_ASSIGN)
    {
      IROperand c = tcc_ir_op_get_src1(ir, &ir->compact_instructions[d]);
      if (irop_is_immediate(c) && irop_is_plain_imm(c) && sv_read_const32(ir, c, &k) &&
          ((k == 0 && nonzero) || (have_range && (k < lo || k > hi))))
        continue;
    }
    if (pred < 0 || !(kind = sv_strnlen_of(s, ovr, base, &call, &m)))
      return 0;
    /* strlen's zero needs no test; strnlen's may be the edge into the phi's
       block itself */
    IROperand y;
    int tok;
    if (kind == 2 || (sv_edge_cmp(s, pred, pb, o, &y, &tok) && (tok == VRP_TOK_NE || tok == VRP_TOK_ULT) &&
                      sv_same_value(s, y, m)))
      o = none;
    if (!sv_clean_path(s, cfg->instr_to_block[call], call, pred, cfg->blocks[pred].end_idx, o, m))
      return 0;
  }
  return 1;
}

/* Decide an unsigned CMP a,b from sv_prove_below (the check of a[i] when i
 * counts down from len - 1: dirname, basename). */
static int sv_try_fold_below(IRSSAOptCtx *ctx, SVState *s, int cmp_idx, int jmp_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
  int tok = (int)tcc_ir_op_src1_imm(ir, &ir->compact_instructions[jmp_idx]);
  if (cq->op != TCCIR_OP_CMP || tok < 0 || tcc_ir_barrel_shift_at(ir, cq) ||
      tcc_ir_instr_access_is_volatile(ir, cq) || sv_next_reads_flags(ir, jmp_idx))
    return 0;
  IROperand a = tcc_ir_op_get_src1(ir, cq), b = tcc_ir_op_get_src2(ir, cq);
  if (a.btype != IROP_BTYPE_INT32 || b.btype != IROP_BTYPE_INT32)
    return 0;
  int verdict = -1;
  for (int side = 0; side < 2 && verdict < 0; side++)
  {
    IROperand x = side ? b : a, k = side ? a : b;
    int t = side ? vrp_swap_cmp_tok(tok) : tok;
    if (t == VRP_TOK_ULT || t == VRP_TOK_UGE)
    {
      if (sv_prove_below(s, x, k, 1, 0))
        verdict = t == VRP_TOK_ULT;
    }
    else if (t == VRP_TOK_ULE || t == VRP_TOK_UGT)
    {
      if (sv_prove_below(s, x, k, 0, 0))
        verdict = t == VRP_TOK_ULE;
    }
  }
  if (verdict < 0)
    return 0;
  sv_commit_jumpif(ctx, cmp_idx, jmp_idx, verdict);
  sv_drop_cmp_use(ctx, irop_get_vreg(a), cmp_idx);
  sv_drop_cmp_use(ctx, irop_get_vreg(b), cmp_idx);
  return 1;
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
          if (!fired && q->op == TCCIR_OP_CMP && !tcc_ir_op_src2_is_imm(ir, q))
            fired = sv_try_fold_regreg(ctx, block, i, j);
          if (!fired)
            fired = sv_try_fold_iv_bound(ctx, s, i, j);
          if (!fired)
            fired = sv_try_fold_below(ctx, s, i, j);
          changes += fired;
        }
        else if (op2 == TCCIR_OP_SETIF)
          changes += sv_try_fold_setif(ctx, s, i, j);
        else if (op2 == TCCIR_OP_SELECT)
          changes += sv_try_fold_select(ctx, s, i, j);
      }
      continue;
    }
    changes += sv_try_fold_mask_identity(ctx, s, i);
    sv_seed_def(s, q);
    if (q->op == TCCIR_OP_LOAD_INDEXED && sv_strnlen_zero(s, block, i))
    {
      int dslot = sv_slot(s, irop_get_vreg(tcc_ir_op_get_dest(ir, q)));
      if (dslot >= 0)
        sv_set(s, dslot, 0, 0);
    }
  }
  return changes;
}

/* Whether any instruction directly writes vr (as a non-lval dest). */
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

  /* Precomputed so sv_slot's per-lookup test is O(1) instead of an O(n) scan.
   * ONE fused pass over the instructions collects write counts and
   * address-taken flags for every PARAM and VAR slot at once; the previous
   * per-slot sv_vreg_ever_written / sv_var_write_count /
   * ir_opt_vreg_address_taken_between scans made setup O((P+V)*n) —
   * 18.5 s of tests2/101_cleanup's -O1 compile before the domwalk even
   * started (the pass runs ~23x through the pipeline).
   *
   * Semantics mirrored exactly: a PARAM counts as written only by a
   * non-lval dest (sv_vreg_ever_written); a VAR write counts lval slot
   * STOREs too (sv_var_write_count); address-taken is a LEA whose src1 is
   * the slot's vreg. */
  if (s.param_cap > 0 || s.var_cap > 0)
  {
    int n = ir->next_instruction_index;
    uint8_t *pw = s.param_cap > 0 ? tcc_mallocz((size_t)s.param_cap * 2) : NULL;
    uint8_t *pt = pw ? pw + s.param_cap : NULL;
    uint8_t *vw = s.var_cap > 0 ? tcc_mallocz((size_t)s.var_cap * 2) : NULL;
    uint8_t *vt = vw ? vw + s.var_cap : NULL;
    int *vwr = s.var_cap > 0 ? tcc_malloc((size_t)s.var_cap * SV_VAR_WRITES * sizeof(int)) : NULL;
    for (int k = 0; vwr && k < s.var_cap * SV_VAR_WRITES; k++)
      vwr[k] = -1;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op == TCCIR_OP_LEA)
      {
        int32_t avr = tcc_ir_op_src1_vreg(ir, q);
        if (avr >= 0)
        {
          int at = TCCIR_DECODE_VREG_TYPE(avr), ap = TCCIR_DECODE_VREG_POSITION(avr);
          if (pt && at == TCCIR_VREG_TYPE_PARAM && ap >= 0 && ap < s.param_cap)
            pt[ap] = 1;
          if (vt && at == TCCIR_VREG_TYPE_VAR && ap >= 0 && ap < s.var_cap)
            vt[ap] = 1;
        }
      }
      if (!irop_config[q->op].has_dest)
        continue;
      int32_t dvr = tcc_ir_op_dest_vreg(ir, q);
      if (dvr < 0)
        continue;
      {
        int dt = TCCIR_DECODE_VREG_TYPE(dvr), dp = TCCIR_DECODE_VREG_POSITION(dvr);
        if (pw && dt == TCCIR_VREG_TYPE_PARAM && dp >= 0 && dp < s.param_cap && !tcc_ir_op_dest_is_lval(ir, q))
          pw[dp] = 1;
        if (vw && dt == TCCIR_VREG_TYPE_VAR && dp >= 0 && dp < s.var_cap && vw[dp] < 2)
          vw[dp]++;
        if (vwr && dt == TCCIR_VREG_TYPE_VAR && dp >= 0 && dp < s.var_cap && vwr[dp * SV_VAR_WRITES] != -2)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int *w = &vwr[dp * SV_VAR_WRITES], k = 0;
          while (k < SV_VAR_WRITES && w[k] >= 0)
            k++;
          if (k == SV_VAR_WRITES || (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_ASSIGN) ||
              d.btype != IROP_BTYPE_INT32 || irop_is_llocal(d) || (d.is_lval && !irop_is_local(d)) ||
              tcc_ir_barrel_shift_at(ir, q))
            w[0] = -2;
          else
            w[k] = i;
        }
      }
    }
    if (s.param_cap > 0)
    {
      SVFlagSeq_init(&param_stable_owner, (size_t)s.param_cap);
      s.param_stable = SVFlagSeq_data(&param_stable_owner);
      for (int p = 0; p < s.param_cap; p++)
        s.param_stable[p] = !pw[p] && !pt[p];
    }
    /* Stable VAR: written once, address never taken, not volatile. */
    if (s.var_cap > 0)
    {
      SVFlagSeq_init(&var_stable_owner, (size_t)s.var_cap);
      s.var_stable = SVFlagSeq_data(&var_stable_owner);
      for (int v = 0; v < s.var_cap; v++)
      {
        int is_vol = (v < ir->variables_live_intervals_size &&
                      ir->variables_live_intervals[v].is_volatile);
        s.var_stable[v] = !is_vol && vw[v] == 1 && !vt[v];
        if (is_vol || vt[v])
          vwr[v * SV_VAR_WRITES] = -2;
      }
      s.var_writes = vwr;
      s.var_addr = tcc_mallocz((size_t)s.var_cap);
      for (int v = 0; v < s.var_cap; v++)
        s.var_addr[v] = vt[v];
    }
    tcc_free(pw);
    tcc_free(vw);
  }

  OptSSADomWalk walk = {
      .state = &s,
      .mark = sv_mark,
      .reset = sv_reset,
      .enter = sv_enter,
      .visit = sv_visit,
  };
  int changes = opt_ssa_domwalk(ctx, &walk);
  tcc_free(s.var_writes);
  tcc_free(s.var_addr);
  /* Our branch folds strand blocks, whose phi operands must go the same way
   * ssa_opt_branch prunes its own (a stale operand's def is later NOP'd by
   * dce, leaving it unresolvable for good). */
  if (changes > 0)
    changes += ssa_opt_prune_unreachable_phis(ctx);
  return changes;
}

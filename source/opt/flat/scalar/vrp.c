/*
 *  TCC IR - Value Range Propagation (VRP)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Per-vreg [min,max] range tracking over one forward walk; folds CMP+JUMPIF, CMP+SETIF and reg-reg compare chains. No SSA analog (Branch A). */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "opt_alias.h"
#include "opt_engine.h"
#include "memory/small_sequence.h"

#define VRP_MAX_POS 256

typedef struct
{
  int valid;
  int64_t min_val;
  int64_t max_val;
} VRPRange;

/* Inline cap 1: the 3*256-slot range table always exceeds it, so it always heap-allocates. */
TCC_SMALL_SEQUENCE_DEFINE(VrpRangeSeq, VRPRange, 1)

/* Condition tokens (per tcc.h): unsigned ULT/UGE/ULE/UGT, EQ/NE, signed LT/GE/LE/GT. */
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

/* (vreg_type, position) → flat slot: PARAM [0,256), TEMP [256,512), VAR [512,768); -1 if untracked. */
static int vrp_get_slot(int vr_type, int pos)
{
  if (pos < 0 || pos >= VRP_MAX_POS)
    return -1;
  if (vr_type == TCCIR_VREG_TYPE_PARAM)
    return pos;
  if (vr_type == TCCIR_VREG_TYPE_TEMP)
    return VRP_MAX_POS + pos;
  if (vr_type == TCCIR_VREG_TYPE_VAR)
    return 2 * VRP_MAX_POS + pos;
  return -1;
}

/* Read a constant into the pass's sign-extended-int32 domain; reject genuinely 64-bit-typed operands (mixing zero/sign-extended encodings flips unsigned compares — ptr fuzz 35289). */
static int vrp_read_const32(const TCCIRState *ir, IROperand op, int64_t *out)
{
  if (op.btype == IROP_BTYPE_INT64)
    return 0;
  *out = (int64_t)(int32_t)irop_get_imm64_ex(ir, op);
  return 1;
}

/* Fold a compare over [rmin,rmax]: 1 always-taken, 0 never, -1 unknown. Unsigned safe only when both endpoints share sign (monotone uint32 ordering). */
static int vrp_fold_cmp(int64_t rmin, int64_t rmax, int64_t cmp_val, int tok)
{
  /* Mixed-sign range spans both halves of uint32 space → endpoints prove nothing. */
  if ((tok == VRP_TOK_ULT || tok == VRP_TOK_UGE || tok == VRP_TOK_ULE || tok == VRP_TOK_UGT) &&
      (rmin < 0) != (rmax < 0))
    return -1;
  int res_min = evaluate_compare_condition(rmin, cmp_val, tok);
  int res_max = evaluate_compare_condition(rmax, cmp_val, tok);
  if (res_min < 0 || res_max < 0 || res_min != res_max)
    return -1;
  return res_min;
}

/* Whether a range [lo,hi] proves `x <tok> cmp_val`: 1 always, 0 never, -1 unknown. EQ/NE need the
 * value outside the range or a singleton; the rest defer to vrp_fold_cmp (mixed-sign guard inside). */
static int vrp_range_verdict(int64_t lo, int64_t hi, int64_t cmp_val, int tok)
{
  if (tok == VRP_TOK_EQ || tok == VRP_TOK_NE)
  {
    if (cmp_val < lo || cmp_val > hi)
      return (tok == VRP_TOK_NE) ? 1 : 0;
    if (lo == hi)
      return (tok == VRP_TOK_EQ) ? 1 : 0;
    return -1;
  }
  return vrp_fold_cmp(lo, hi, cmp_val, tok);
}

/* Loop-carried state for one tcc_ir_opt_vrp run — threaded through the per-instruction handlers. */
typedef struct
{
  TCCIRState *ir;
  int n;
  size_t bytes;               /* byte size of one range table (for memset/memcpy) */
  VRPRange *ranges;           /* current facts */
  VRPRange *deferred;         /* facts snapshotted at a dominating jump, reinstalled at its target */
  const uint8_t *is_merge;

  int ranges_dirty;           /* dirty==0 ⇔ ranges is entirely .valid==0 (skip the 18 KB wipe) */
  int deferred_dirty;
  int deferred_target;        /* index at which `deferred` is reinstalled, or -1 */

  int pending_at;             /* fall-through constraint applies at this index, or -1 */
  int pending_slot;
  int64_t pending_min, pending_max;

  int eq_end;                 /* scoped equality holds until this index, or -1 */
  int eq_slot;
  int eq_src_slot;            /* source PARAM slot the equality was back-propagated to, or -1 */
  int64_t eq_val;

  int changes;
} VrpState;

/* Decode a vreg operand's tracking slot, or -1 if it has no vreg / isn't tracked. */
static int vrp_slot(int32_t vr)
{
  return vr >= 0 ? vrp_get_slot(TCCIR_DECODE_VREG_TYPE(vr), TCCIR_DECODE_VREG_POSITION(vr)) : -1;
}

static int vrp_merge_at(const VrpState *s, int i)
{
  return s->is_merge[i / 8] & (1 << (i % 8));
}

/* Condition token of a JUMPIF/SETIF (held as an immediate in its src1). */
static int vrp_cond_tok(TCCIRState *ir, IRQuadCompact *branch)
{
  return (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, branch));
}

/* Target instruction index of a JUMP/JUMPIF (held as an immediate in its dest). */
static int vrp_jump_target(TCCIRState *ir, IRQuadCompact *jump)
{
  return (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jump));
}

/* Drop all facts; only touches memory when something is actually live. */
static void vrp_wipe(VrpState *s)
{
  if (s->ranges_dirty)
  {
    memset(s->ranges, 0, s->bytes);
    s->ranges_dirty = 0;
  }
}

static void vrp_set(VrpState *s, int slot, int64_t lo, int64_t hi)
{
  s->ranges[slot].valid = 1;
  s->ranges[slot].min_val = lo;
  s->ranges[slot].max_val = hi;
  s->ranges_dirty = 1;
}

/* Snapshot current facts (plus an optional extra fact slot∈[lo,hi]) into `deferred`, to be
 * reinstalled at `target` — a forward block this branch uniquely dominates (its sole predecessor).
 * extra_slot < 0 means no extra fact. */
static void vrp_defer_to(VrpState *s, int target, int extra_slot, int64_t lo, int64_t hi)
{
  if (s->ranges_dirty)
  {
    memcpy(s->deferred, s->ranges, s->bytes);
    s->deferred_dirty = 1;
  }
  else if (extra_slot >= 0)
  {
    memset(s->deferred, 0, s->bytes);   /* establish a clean base for just the extra fact */
    s->deferred_dirty = 1;
  }
  else
    s->deferred_dirty = 0;

  if (extra_slot >= 0)
  {
    s->deferred[extra_slot].valid = 1;
    s->deferred[extra_slot].min_val = lo;
    s->deferred[extra_slot].max_val = hi;
  }
  s->deferred_target = target;
}

/* Fold "branch taken": drop the CMP, rewrite the branch to an unconditional JUMP to jmp_dest. */
static void vrp_fold_taken(VrpState *s, IRQuadCompact *cmp, IRQuadCompact *branch, int branch_idx,
                           IROperand jmp_dest)
{
  cmp->op = TCCIR_OP_NOP;
  branch->op = TCCIR_OP_JUMP;
  tcc_ir_set_dest(s->ir, branch_idx, jmp_dest);
  s->changes++;
}

/* Fold "branch never taken": drop both the CMP and the branch. */
static void vrp_fold_untaken(VrpState *s, IRQuadCompact *cmp, IRQuadCompact *branch)
{
  cmp->op = TCCIR_OP_NOP;
  branch->op = TCCIR_OP_NOP;
  s->changes++;
}

/* Block-entry transitions: end a scoped equality at its target, then either reinstall deferred
 * facts (dominating jump), clear at a merge point, or apply a pending fall-through constraint. */
static void vrp_enter(VrpState *s, int i)
{
  if (i == s->eq_end)
  {
    if (s->eq_slot >= 0)
      s->ranges[s->eq_slot].valid = 0;
    if (s->eq_src_slot >= 0)
      s->ranges[s->eq_src_slot].valid = 0;
    s->eq_end = s->eq_slot = s->eq_src_slot = -1;
  }

  if (i == s->deferred_target)
  {
    if (s->deferred_dirty)
    {
      memcpy(s->ranges, s->deferred, s->bytes);
      s->ranges_dirty = 1;
    }
    else
      vrp_wipe(s);
    s->deferred_target = -1;
  }
  else if (vrp_merge_at(s, i))
  {
    vrp_wipe(s);
    s->pending_at = s->pending_slot = -1;
    /* Scoped-eq is intentionally not re-applied here — not every merge in [JUMPIF+2,target) is
     * dominated by the fall-through; the CMP+SETIF scan handles the target directly. */
  }
  else if (s->pending_at == i && s->pending_slot >= 0)
  {
    VRPRange *r = &s->ranges[s->pending_slot];
    int64_t lo = s->pending_min, hi = s->pending_max;
    if (r->valid)   /* intersect with any existing range */
    {
      lo = lo > r->min_val ? lo : r->min_val;
      hi = hi < r->max_val ? hi : r->max_val;
    }
    if (lo <= hi)
      vrp_set(s, s->pending_slot, lo, hi);
    s->pending_at = s->pending_slot = -1;
  }
}

/* T/P_dest = T/P_src1 +/- #imm → shift src's range by imm. Returns 1 (op consumed → caller continues). */
static int vrp_step_addsub(VrpState *s, IRQuadCompact *q)
{
  TCCIRState *ir = s->ir;
  if (q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
    return 0;
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  if (!irop_is_immediate(src2))
    return 0;

  int32_t src_vr = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
  int32_t dst_vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
  if (src_vr >= 0 && dst_vr >= 0)
  {
    int src_slot = vrp_slot(src_vr);
    int dst_slot = vrp_slot(dst_vr);
    int64_t imm;
    if (src_slot >= 0 && s->ranges[src_slot].valid && dst_slot >= 0 &&
        vrp_read_const32(ir, src2, &imm))
    {
      int add = (q->op == TCCIR_OP_ADD);
      int64_t lo = add ? s->ranges[src_slot].min_val + imm : s->ranges[src_slot].min_val - imm;
      int64_t hi = add ? s->ranges[src_slot].max_val + imm : s->ranges[src_slot].max_val - imm;
      /* Result outside int32 wraps and is no longer an interval — drop it rather than clamp. */
      if (lo < (int64_t)INT32_MIN || hi > (int64_t)INT32_MAX)
        s->ranges[dst_slot].valid = 0;
      else
        vrp_set(s, dst_slot, lo, hi);
    }
    else if (dst_slot >= 0)
      s->ranges[dst_slot].valid = 0;
  }
  return 1;
}

/* Range through bitwise-AND / shift with a constant 2nd operand. Returns 1 (op consumed) for
 * AND/SHL/SAR/SHR #imm, else 0 (generic invalidation handles it). */
static int vrp_step_bitop(VrpState *s, IRQuadCompact *q)
{
  int op = q->op;
  if (op != TCCIR_OP_AND && op != TCCIR_OP_SHL && op != TCCIR_OP_SAR && op != TCCIR_OP_SHR)
    return 0;
  TCCIRState *ir = s->ir;
  int64_t k;
  if (!irop_is_immediate(tcc_ir_op_get_src2(ir, q)) ||
      !vrp_read_const32(ir, tcc_ir_op_get_src2(ir, q), &k))
    return 0;

  int dst_slot = vrp_slot(irop_get_vreg(tcc_ir_op_get_dest(ir, q)));
  if (dst_slot < 0)
    return 1;

  int64_t lo = 0, hi = 0;
  int have = 0;
  if (op == TCCIR_OP_AND)
  {
    /* x & m with m >= 0 → [0, m], independent of x (result bits ⊆ m's, and m >= 0 keeps it signed). */
    if (k >= 0) { lo = 0; hi = k; have = 1; }
  }
  else if (k >= 0 && k <= 31)   /* shifts need x's range and a 0..31 amount */
  {
    int src_slot = vrp_slot(irop_get_vreg(tcc_ir_op_get_src1(ir, q)));
    if (src_slot >= 0 && s->ranges[src_slot].valid)
    {
      int64_t xlo = s->ranges[src_slot].min_val, xhi = s->ranges[src_slot].max_val;
      if (op == TCCIR_OP_SHL)
      {
        lo = xlo * (1LL << k); hi = xhi * (1LL << k);   /* * not << to keep negatives well-defined */
        have = (lo >= (int64_t)INT32_MIN && hi <= (int64_t)INT32_MAX);
      }
      else if (op == TCCIR_OP_SAR)   /* arithmetic (signed): floor(x / 2^k), monotone, fits int32 */
      {
        lo = xlo >> k; hi = xhi >> k; have = 1;
      }
      else if (xlo >= 0)   /* SHR (logical): only monotone when x is non-negative */
      {
        lo = xlo >> k; hi = xhi >> k; have = 1;
      }
    }
  }

  if (have)
    vrp_set(s, dst_slot, lo, hi);
  else
    s->ranges[dst_slot].valid = 0;
  return 1;
}

/* Propagate a range through ASSIGN. Returns 1 if a range was seeded/forwarded (caller continues),
 * 0 to fall through to the generic invalidation below. */
static int vrp_step_assign(VrpState *s, IRQuadCompact *q)
{
  TCCIRState *ir = s->ir;
  if (q->op != TCCIR_OP_ASSIGN || !irop_config[q->op].has_dest)
    return 0;

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int32_t s1_vr = irop_get_vreg(src1);
  int32_t d_vr = irop_get_vreg(dest);

  /* Seed [imm,imm] from T = #imm — the only range source that creates one from a bare immediate (see docs/bugs.md #6). */
  if (irop_is_immediate(src1) && d_vr >= 0 && !dest.is_lval && !dest.is_sym)
  {
    int d_slot = vrp_slot(d_vr);
    if (d_slot >= 0)
    {
      int64_t imm;
      if (vrp_read_const32(ir, src1, &imm))
        vrp_set(s, d_slot, imm, imm);
      else
        s->ranges[d_slot].valid = 0;
      return 1;
    }
  }

  /* Copy-forward a source that carries a value range: a non-lval, or a VAR/PARAM lval naming a
   * local (a TEMP lval is a pointer deref whose value is unrelated to the pointer's range). */
  int src_type = (s1_vr >= 0) ? TCCIR_DECODE_VREG_TYPE(s1_vr) : -1;
  int src_forwards =
      s1_vr >= 0 &&
      (!src1.is_lval ||
       ((src_type == TCCIR_VREG_TYPE_VAR || src_type == TCCIR_VREG_TYPE_PARAM) &&
        !src1.is_llocal && !src1.is_sym));
  if (src_forwards && d_vr >= 0)
  {
    int s_slot = vrp_slot(s1_vr);
    int d_slot = vrp_slot(d_vr);
    if (s_slot >= 0 && d_slot >= 0 && s->ranges[s_slot].valid)
    {
      s->ranges[d_slot] = s->ranges[s_slot];   /* source valid ⇒ ranges already dirty */
      return 1;
    }
    if (d_slot >= 0)
      s->ranges[d_slot].valid = 0;
  }
  return 0;
}

/* Classify instruction bq as a reaching def of target: 1 = ASSIGN/LOAD def (src1 → *bs),
 * 0 = not a def of target (skip), -1 = def of target but not a plain ASSIGN/LOAD (unknown). */
static int vrp_def_src(TCCIRState *ir, IRQuadCompact *bq, int32_t target, IROperand *bs)
{
  if (bq->op == TCCIR_OP_NOP || !irop_config[bq->op].has_dest)
    return 0;
  if (irop_get_vreg(tcc_ir_op_get_dest(ir, bq)) != target)
    return 0;
  if (bq->op != TCCIR_OP_ASSIGN && bq->op != TCCIR_OP_LOAD)
    return -1;
  *bs = tcc_ir_op_get_src1(ir, bq);
  return 1;
}

/* Scan defs of target_vr before i; if all are either an immediate != val (dead on the fall-through)
 * or a non-lval load of one common PARAM, seed that PARAM = [val,val] and return its slot, else -1. */
static int vrp_backprop_param(VrpState *s, int i, int32_t target_vr, int64_t val)
{
  TCCIRState *ir = s->ir;
  int32_t param_vr = -1;
  int param_slot = -1;
  for (int bi = 0; bi < i; bi++)
  {
    IROperand bs;
    int cls = vrp_def_src(ir, &ir->compact_instructions[bi], target_vr, &bs);
    if (cls == 0)
      continue;
    if (cls < 0)
      return -1;

    if (irop_is_immediate(bs))
    {
      int64_t bv;
      if (!vrp_read_const32(ir, bs, &bv) || bv == val)
        return -1;
      continue;
    }
    int32_t bsv = irop_get_vreg(bs);
    if (bsv < 0 || bs.is_lval || TCCIR_DECODE_VREG_TYPE(bsv) != TCCIR_VREG_TYPE_PARAM)
      return -1;
    if (param_vr >= 0 && param_vr != bsv)
      return -1;
    param_vr = bsv;
    param_slot = vrp_slot(bsv);
  }
  if (param_vr >= 0 && param_slot >= 0)
  {
    vrp_set(s, param_slot, val, val);
    return param_slot;
  }
  return -1;
}

/* NOT(cond) holds on the JUMPIF fall-through: record the implied [lo,hi] constraint for i+2, and
 * for an equality (fall-through of !=) also open a scoped constraint and back-propagate to the PARAM. */
static void vrp_derive_fallthrough(VrpState *s, IRQuadCompact *q, IRQuadCompact *jq, int i,
                                   int src_slot, int64_t cmp_val, int tok)
{
  TCCIRState *ir = s->ir;
  int64_t lo = INT32_MIN, hi = INT32_MAX;
  int set = 0;
  switch (tok)   /* fall-through means cond is FALSE for (src1 vs cmp_val) */
  {
  case VRP_TOK_LE:   /* !( <=S ) → src1 > cmp_val */
    if (cmp_val < (int64_t)INT32_MAX) { lo = cmp_val + 1; hi = INT32_MAX; set = 1; }
    break;
  case VRP_TOK_LT:   /* !( <S ) → src1 >= cmp_val */
    lo = cmp_val < (int64_t)INT32_MIN ? INT32_MIN : cmp_val; hi = INT32_MAX; set = 1;
    break;
  case VRP_TOK_GE:   /* !( >=S ) → src1 < cmp_val */
    lo = INT32_MIN; hi = cmp_val > (int64_t)INT32_MAX ? INT32_MAX : cmp_val - 1;
    set = (hi >= (int64_t)INT32_MIN);
    break;
  case VRP_TOK_GT:   /* !( >S ) → src1 <= cmp_val */
    lo = INT32_MIN; hi = cmp_val > (int64_t)INT32_MAX ? INT32_MAX : cmp_val; set = 1;
    break;
  case VRP_TOK_NE:   /* !( != ) → src1 == cmp_val */
    lo = cmp_val; hi = cmp_val; set = (cmp_val >= INT32_MIN && cmp_val <= INT32_MAX);
    break;
  default:
    break;
  }
  if (!set || lo > hi)
    return;

  s->pending_at = i + 2;
  s->pending_slot = src_slot;
  s->pending_min = lo;
  s->pending_max = hi;
  if (lo != hi)
    return;

  s->eq_end = vrp_jump_target(ir, jq);
  s->eq_slot = src_slot;
  s->eq_src_slot = -1;
  s->eq_val = lo;
  /* Back-propagate == to the source PARAM only for a direct vreg CMP (a deref would confuse the
   * pointer address with the pointed-to value). */
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  if (!src1.is_lval)
  {
    int pslot = vrp_backprop_param(s, i, irop_get_vreg(src1), lo);
    if (pslot >= 0)
      s->eq_src_slot = pslot;
  }
}

/* CMP x,#c ; JUMPIF: tautology fold, range fold, else derive the fall-through constraint. */
static void vrp_cmp_jumpif_const(VrpState *s, IRQuadCompact *q, IRQuadCompact *jq, int i)
{
  TCCIRState *ir = s->ir;
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  int32_t src1_vr = irop_get_vreg(src1);
  if (src1_vr < 0)
    return;

  int src_slot = vrp_slot(src1_vr);
  int64_t cmp_val = 0;
  int cmp_val_ok = vrp_read_const32(ir, src2, &cmp_val);
  int tok = vrp_cond_tok(ir, jq);
  IROperand jmp_dest = tcc_ir_op_get_dest(ir, jq);

  /* Tautology: unsigned compare vs 0 is always-true (UGE) or always-false (ULT), no range needed. */
  if (irop_get_imm64_ex(ir, src2) == 0)
  {
    if (tok == VRP_TOK_UGE) { vrp_fold_taken(s, q, jq, i + 1, jmp_dest); return; }
    if (tok == VRP_TOK_ULT) { vrp_fold_untaken(s, q, jq); return; }
  }

  if (src_slot >= 0 && s->ranges[src_slot].valid && cmp_val_ok)
  {
    int v = vrp_range_verdict(s->ranges[src_slot].min_val, s->ranges[src_slot].max_val, cmp_val, tok);
    if (v == 1) { vrp_fold_taken(s, q, jq, i + 1, jmp_dest); return; }
    if (v == 0) { vrp_fold_untaken(s, q, jq); return; }
  }

  /* EQ taken edge: x == cmp_val holds in the jump target when that target is a forward block this
   * branch uniquely dominates. Carry the singleton there (mirrors the NE fall-through equality). */
  if (tok == VRP_TOK_EQ && src_slot >= 0 && cmp_val_ok)
  {
    int target = vrp_jump_target(ir, jq);
    if (target > i + 1 && target < s->n && !vrp_merge_at(s, target))
      vrp_defer_to(s, target, src_slot, cmp_val, cmp_val);
  }

  if (src_slot >= 0 && i + 2 < s->n && cmp_val_ok)
    vrp_derive_fallthrough(s, q, jq, i, src_slot, cmp_val, tok);
}

/* CMP A,B ; JUMPIF c1 (fall-through !c1) then CMP A,B ; JUMPIF c2 — fold #2 when !c1 ⇒ c2 (or !c2). */
static void vrp_cmp_jumpif_regreg(VrpState *s, IRQuadCompact *q, IRQuadCompact *jq, int i)
{
  TCCIRState *ir = s->ir;
  int32_t vr1 = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
  int32_t vr2 = irop_get_vreg(tcc_ir_op_get_src2(ir, q));
  if (vr1 < 0 || vr2 < 0 || i + 3 >= s->n)
    return;

  int known = vrp_negate_cmp_tok(vrp_cond_tok(ir, jq));
  if (known < 0 || vrp_merge_at(s, i + 2))
    return;

  IRQuadCompact *cmp2 = &ir->compact_instructions[i + 2];
  IRQuadCompact *jump2 = &ir->compact_instructions[i + 3];
  if (cmp2->op != TCCIR_OP_CMP || jump2->op != TCCIR_OP_JUMPIF)
    return;

  int32_t c2v1 = irop_get_vreg(tcc_ir_op_get_src1(ir, cmp2));
  int32_t c2v2 = irop_get_vreg(tcc_ir_op_get_src2(ir, cmp2));
  int tok2 = vrp_cond_tok(ir, jump2);
  int eff = -1;
  if (c2v1 == vr1 && c2v2 == vr2)
    eff = tok2;                        /* same operand order */
  else if (c2v1 == vr2 && c2v2 == vr1)
    eff = vrp_swap_cmp_tok(tok2);      /* swapped operands */
  if (eff < 0)
    return;

  if (vrp_cmp_implies(known, eff))
    vrp_fold_taken(s, cmp2, jump2, i + 3, tcc_ir_op_get_dest(ir, jump2));
  else if (vrp_cmp_implies(known, vrp_negate_cmp_tok(eff)))
    vrp_fold_untaken(s, cmp2, jump2);
}

/* True if every def of cmp_vr before i folds to the SAME verdict for (def <tok> cmp_val), where an
 * immediate def uses its own value and a non-lval load of eq_src_slot's PARAM uses eq_val. */
static int vrp_setif_defs_agree(VrpState *s, int i, int32_t cmp_vr, int64_t cmp_val, int tok)
{
  TCCIRState *ir = s->ir;
  int unified = -2;
  for (int bi = 0; bi < i; bi++)
  {
    IROperand bs;
    int cls = vrp_def_src(ir, &ir->compact_instructions[bi], cmp_vr, &bs);
    if (cls == 0)
      continue;
    if (cls < 0)
      return 0;

    int64_t def_val;
    if (irop_is_immediate(bs))
    {
      if (!vrp_read_const32(ir, bs, &def_val))
        return 0;
    }
    else
    {
      int32_t bsv = irop_get_vreg(bs);
      if (bsv < 0 || bs.is_lval || vrp_slot(bsv) != s->eq_src_slot)
        return 0;
      def_val = s->eq_val;
    }

    int f = evaluate_compare_condition(def_val, cmp_val, tok);
    if (f < 0)
      return 0;
    if (unified == -2)
      unified = f;
    else if (unified != f)
      return 0;
  }
  return unified >= 0;
}

/* CMP x,#c ; SETIF: fold the boolean to a constant when a range (stored, or derived from the scoped
 * equality across all reaching defs) proves the result. */
static void vrp_cmp_setif(VrpState *s, IRQuadCompact *q, IRQuadCompact *jq, int i)
{
  TCCIRState *ir = s->ir;
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  int32_t cmp_vr = irop_get_vreg(src1);
  if (cmp_vr < 0)
    return;
  int64_t cmp_val;
  if (!vrp_read_const32(ir, tcc_ir_op_get_src2(ir, q), &cmp_val))
    return;

  int cmp_slot = vrp_slot(cmp_vr);
  int have_range = (cmp_slot >= 0 && s->ranges[cmp_slot].valid);
  int tok = vrp_cond_tok(ir, jq);

  if (!have_range && !src1.is_lval && s->eq_src_slot >= 0 && i < s->eq_end && cmp_slot >= 0 &&
      vrp_setif_defs_agree(s, i, cmp_vr, cmp_val, tok))
  {
    vrp_set(s, cmp_slot, s->eq_val, s->eq_val);
    have_range = 1;
  }

  if (have_range)
  {
    int v = vrp_range_verdict(s->ranges[cmp_slot].min_val, s->ranges[cmp_slot].max_val, cmp_val, tok);
    if (v >= 0)
    {
      IROperand set_dest = tcc_ir_op_get_dest(ir, jq);
      q->op = TCCIR_OP_NOP;
      jq->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i + 1, irop_make_imm32(-1, v, IROP_BTYPE_INT32));
      tcc_ir_op_set_dest(ir, jq, set_dest);
      s->changes++;
    }
  }
}

/* CMP followed by JUMPIF/SETIF. Returns 1 (op consumed → caller continues) whenever there is a
 * following instruction; the branch peephole itself may or may not fire. */
static int vrp_step_cmp(VrpState *s, IRQuadCompact *q, int i)
{
  if (q->op != TCCIR_OP_CMP || i + 1 >= s->n)
    return 0;

  IRQuadCompact *jq = &s->ir->compact_instructions[i + 1];
  int src2_imm = irop_is_immediate(tcc_ir_op_get_src2(s->ir, q));
  if (jq->op == TCCIR_OP_JUMPIF && src2_imm)
    vrp_cmp_jumpif_const(s, q, jq, i);
  else if (jq->op == TCCIR_OP_JUMPIF)
    vrp_cmp_jumpif_regreg(s, q, jq, i);
  if (jq->op == TCCIR_OP_SETIF && src2_imm)
    vrp_cmp_setif(s, q, jq, i);
  return 1;
}

/* Any other instruction writing a tracked slot invalidates its range. */
static void vrp_kill_dest(VrpState *s, IRQuadCompact *q)
{
  int32_t dest_vr = irop_get_vreg(tcc_ir_op_get_dest(s->ir, q));
  if (dest_vr >= 0 && irop_config[q->op].has_dest)
  {
    int slot = vrp_slot(dest_vr);
    if (slot >= 0)
      s->ranges[slot].valid = 0;
  }
}

/* JUMP/RETURN: no linear fall-through. Hand facts to a uniquely-dominated forward block, then clear. */
static void vrp_step_terminator(VrpState *s, IRQuadCompact *q, int i)
{
  TCCIRState *ir = s->ir;
  if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_RETURNVALUE && q->op != TCCIR_OP_RETURNVOID)
    return;

  if (q->op == TCCIR_OP_JUMP)
  {
    int t = vrp_jump_target(ir, q);
    if (t > i && t < s->n && !vrp_merge_at(s, t))
      vrp_defer_to(s, t, -1, 0, 0);
  }
  vrp_wipe(s);
  s->pending_at = s->pending_slot = -1;
}

int tcc_ir_opt_vrp(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 3)
    return 0;

  const size_t vrp_ranges_bytes = sizeof(VRPRange) * (VRP_MAX_POS * 3);
  small_sequence(VrpRangeSeq) ranges_owner = {0}, deferred_owner = {0};
  if (VrpRangeSeq_init(&ranges_owner, (size_t)(VRP_MAX_POS * 3)) != 0 ||
      VrpRangeSeq_init(&deferred_owner, (size_t)(VRP_MAX_POS * 3)) != 0)
    return 0;

  uint8_t *is_merge = ir_opt_build_merge_bitmap(ir, n);

  VrpState s = {
      .ir = ir,
      .n = n,
      .bytes = vrp_ranges_bytes,
      .ranges = VrpRangeSeq_data(&ranges_owner),
      .deferred = VrpRangeSeq_data(&deferred_owner),
      .is_merge = is_merge,
      .deferred_target = -1,
      .pending_at = -1,
      .pending_slot = -1,
      .eq_end = -1,
      .eq_slot = -1,
      .eq_src_slot = -1,
  };

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    vrp_enter(&s, i);
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (vrp_step_addsub(&s, q))
      continue;
    if (vrp_step_bitop(&s, q))
      continue;
    if (vrp_step_assign(&s, q))
      continue;
    if (vrp_step_cmp(&s, q, i))
      continue;
    vrp_kill_dest(&s, q);
    vrp_step_terminator(&s, q, i);
  }

  tcc_free(is_merge);
  return s.changes;
}

int tcc_ir_opt_vrp_ex(IROptCtx *ctx) { return tcc_ir_opt_vrp(ctx->ir); }

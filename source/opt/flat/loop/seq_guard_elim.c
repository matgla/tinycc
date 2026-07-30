/*
 *  TCC IR - Zero-trip entry-guard elimination for sequential counted loops
 *
 *  Rotation (source/opt/flat/loop/loop_rotate.c) turns a top-tested loop into
 *  a bottom-tested body plus a pre-loop guard `CMP iv,#lim / JUMPIF cond exit`.
 *  That trade only pays when the guard disappears afterwards; ordinary const
 *  propagation deletes it when the IV enters from a literal, but not when it
 *  enters as the *exit value of a preceding loop* over the same variable — the
 *  shape of the gcc-torture memclr / memcpy check-loop family
 *
 *      for (i = 0; i < A; i++) check(v[i]);
 *      for (;     i < B; i++) check(v[i]);
 *      for (;     i < C; i++) check(v[i]);
 *
 *  where SSA sees a phi at each header and cannot conclude i == A on entry.
 *
 *  This module carries the IV constant forward in program order across a chain
 *  of counted loops (exit value = init + trip*step) and NOPs every pre-loop
 *  guard it proves untaken.  The same walker answers the question rotation asks
 *  before it commits (tcc_ir_loop_seq_entry_const), so loops 2..N of a chain
 *  become rotatable in the first place.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "cfg.h"
#include "opt.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"

/* Distinct IVs tracked at once; a sequential chain shares one, so this is
 * generous.  Also bounds the per-IV escape-scan cache. */
#define SEQ_MAX_TRACK 8
/* Whole-function walk bound: the region scans are O(n) each, so cap the input
 * rather than let a generated monster function go quadratic. */
#define SEQ_MAX_INSTRS 4096

typedef struct
{
  int32_t vreg;
  int64_t val;
} SeqKnown;

typedef struct
{
  SeqKnown v[SEQ_MAX_TRACK];
  int n;
} SeqState;

/* Loop shapes this pass models. */
#define SEQ_TOP_TESTED 1 /* CMP/JUMPIF is the loop test itself */
#define SEQ_ROTATED 2    /* CMP/JUMPIF is a guard in front of a rotated body */

typedef struct
{
  int kind;
  int region_lo; /* first instruction of the loop region */
  int region_hi; /* last instruction of the loop region */
  int exit_idx;  /* branch target reached when the loop is left */
  int32_t iv;
  int limit;     /* limit of the loop's own test */
  int exit_cond; /* condition under which the loop is left */
  int guard_cmp; /* SEQ_ROTATED: guard CMP index, else -1 */
  int guard_jif; /* SEQ_ROTATED: guard JUMPIF index, else -1 */
} SeqLoop;

/* A def is "direct" when it writes the vreg itself rather than memory through
 * it; same convention as loop_rotate.c's ROT_VAR_DIRECT. */
static int seq_dest_is_direct(IROperand d)
{
  if (!d.is_lval)
    return 1;
  return TCCIR_DECODE_VREG_TYPE(irop_get_vreg(d)) == TCCIR_VREG_TYPE_VAR && d.is_local;
}

static int seq_next_live(TCCIRState *ir, int i)
{
  int n = ir->next_instruction_index;
  if (i < 0)
    i = 0;
  while (i < n && ir->compact_instructions[i].op == TCCIR_OP_NOP)
    i++;
  return i;
}

static int seq_prev_live(TCCIRState *ir, int i)
{
  if (i >= ir->next_instruction_index)
    i = ir->next_instruction_index - 1;
  while (i >= 0 && ir->compact_instructions[i].op == TCCIR_OP_NOP)
    i--;
  return i;
}

static int seq_get(SeqState *st, int32_t vreg, int64_t *out)
{
  for (int k = 0; k < st->n; k++)
    if (st->v[k].vreg == vreg)
    {
      *out = st->v[k].val;
      return 1;
    }
  return 0;
}

static void seq_set(SeqState *st, int32_t vreg, int64_t val)
{
  for (int k = 0; k < st->n; k++)
    if (st->v[k].vreg == vreg)
    {
      st->v[k].val = val;
      return;
    }
  if (st->n < SEQ_MAX_TRACK)
  {
    st->v[st->n].vreg = vreg;
    st->v[st->n].val = val;
    st->n++;
  }
}

static void seq_drop(SeqState *st, int32_t vreg)
{
  for (int k = 0; k < st->n; k++)
    if (st->v[k].vreg == vreg)
    {
      st->v[k] = st->v[--st->n];
      return;
    }
}

/* Branch target of a JUMP/JUMPIF at `i`, or -1 for anything else. */
static int seq_branch_target(TCCIRState *ir, int i)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
    return -1;
  return (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
}

/* 1 when the IV's storage can be written behind the walker's back — its
 * address escapes, or it is itself used as a pointer.  Such a vreg cannot be
 * tracked across a call or an opaque store. */
static int seq_vreg_escapes(TCCIRState *ir, int32_t iv)
{
  int n = ir->next_instruction_index;
  int iv_kind = TCCIR_DECODE_VREG_TYPE(iv);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int op = q->op;
    if (op == TCCIR_OP_NOP)
      continue;
    int is_call = (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID);
    if (op == TCCIR_OP_LEA && irop_config[op].has_src1 &&
        irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == iv)
      return 1; /* &iv taken */
    IROperand ops[4];
    int no = 0;
    if (irop_config[op].has_dest)
      ops[no++] = tcc_ir_op_get_dest(ir, q);
    if (irop_config[op].has_src1 && !is_call)
      ops[no++] = tcc_ir_op_get_src1(ir, q);
    if (irop_config[op].has_src2)
      ops[no++] = tcc_ir_op_get_src2(ir, q);
    if (op == TCCIR_OP_MLA)
      ops[no++] = tcc_ir_op_get_accum(ir, q);
    for (int k = 0; k < no; k++)
    {
      if (irop_get_vreg(ops[k]) != iv)
        continue;
      /* the slot address rather than the slot's value */
      if (irop_get_tag(ops[k]) == IROP_TAG_STACKOFF && !ops[k].is_lval)
        return 1;
      /* a dereference *through* the IV; a direct VAR access is a plain use */
      if (ops[k].is_lval && !(iv_kind == TCCIR_VREG_TYPE_VAR && ops[k].is_local))
        return 1;
    }
  }
  return 0;
}

/* Sole step def of `iv` inside [rs,re]: `iv <- iv +/- #imm`, optionally through
 * the post-increment copy `T <- iv; iv <- T +/- #imm` that TCC emits.  Any
 * other def of the IV in the region invalidates the closed form.  0 on
 * failure. */
static int seq_region_step(TCCIRState *ir, int rs, int re, int32_t iv, int *out_step)
{
  int step_idx = -1;
  int64_t step = 0;
  for (int i = rs; i <= re; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int op = q->op;
    if (op == TCCIR_OP_NOP || !irop_config[op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(d) != iv)
      continue;
    if (!seq_dest_is_direct(d))
      return 0; /* written through a pointer alias */
    if (step_idx >= 0)
      return 0; /* more than one def */
    if (op != TCCIR_OP_ADD && op != TCCIR_OP_SUB)
      return 0;
    if (tcc_ir_barrel_shift_at(ir, q))
      return 0;
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    if (!irop_is_immediate(s2) || irop_is_64bit(s1) || irop_is_64bit(s2) || irop_is_64bit(d))
      return 0;
    int32_t src = irop_get_vreg(s1);
    if (src != iv)
    {
      /* accept the copy-through temp, provided the copy is its only def here */
      int copy_defs = 0, other_defs = 0;
      for (int j = rs; j <= re; j++)
      {
        IRQuadCompact *cq = &ir->compact_instructions[j];
        if (cq->op == TCCIR_OP_NOP || !irop_config[cq->op].has_dest)
          continue;
        IROperand cd = tcc_ir_op_get_dest(ir, cq);
        if (irop_get_vreg(cd) != src)
          continue;
        if (cq->op == TCCIR_OP_ASSIGN && j < i && seq_dest_is_direct(cd) &&
            irop_get_vreg(tcc_ir_op_get_src1(ir, cq)) == iv)
          copy_defs++;
        else
          other_defs++;
      }
      if (copy_defs != 1 || other_defs != 0)
        return 0;
    }
    step = irop_get_imm64_ex(ir, s2);
    if (op == TCCIR_OP_SUB)
      step = -step;
    /* compute_trip_count only models a positive step */
    if (step <= 0 || step > 0x10000000)
      return 0;
    step_idx = i;
  }
  if (step_idx < 0)
    return 0;
  *out_step = (int)step;
  return 1;
}

/* Structural sanity of a loop region: no unenumerable control flow, no branch
 * escaping the region except `exit_br_idx`, and no entry from outside. */
static int seq_region_closed(TCCIRState *ir, int rs, int re, int exit_br_idx)
{
  int n = ir->next_instruction_index;
  for (int i = rs; i <= re; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD)
      return 0;
    if (i == exit_br_idx)
      continue;
    int t = seq_branch_target(ir, i);
    if (t < 0)
      continue;
    if (t < rs || t > re)
      return 0;
  }
  for (int i = 0; i < n; i++)
  {
    if (i >= rs && i <= re)
      continue;
    int t = seq_branch_target(ir, i);
    if (t >= rs && t <= re)
      return 0;
  }
  return 1;
}

/* The straight-line stretch [from..to] that the walker has just carried a value
 * across is only reachable the way the walker came: every branch landing in it
 * must originate either in the loop the walker just left ([prev_lo,prev_hi],
 * whose exit branch legitimately targets `from`) or in the loop it is about to
 * enter ([cur_lo,cur_hi], whose back-edge legitimately targets its own header
 * at `to`).  Anything else — a goto, a sibling loop's exit, an enclosing loop's
 * back-edge — means the tracked value is not the only one arriving here. */
static int seq_stretch_private(TCCIRState *ir, int from, int to, int prev_lo, int prev_hi,
                               int cur_lo, int cur_hi)
{
  int n = ir->next_instruction_index;
  for (int j = 0; j < n; j++)
  {
    int t = seq_branch_target(ir, j);
    if (t < from || t > to)
      continue;
    if (prev_lo >= 0 && j >= prev_lo && j <= prev_hi)
      continue;
    if (cur_lo >= 0 && j >= cur_lo && j <= cur_hi)
      continue;
    return 0;
  }
  return 1;
}

/* Drop a jump-target mark no branch reaches any more: a stale one is a phantom
 * block boundary that blocks var->temp promotion downstream. */
static void seq_refresh_jump_target(TCCIRState *ir, int target)
{
  int n = ir->next_instruction_index;
  if (target < 0 || target >= n)
    return;
  for (int i = 0; i < n; i++)
    if (seq_branch_target(ir, i) == target)
      return;
  ir->compact_instructions[target].is_jump_target = 0;
}

/* Recognize the loop introduced by `CMP iv,#lim` at `ci` with its JUMPIF at
 * `ji`, in either shape the pipeline produces.  0 when the pair is not a loop
 * test/guard this pass models. */
static int seq_match_loop(TCCIRState *ir, int ci, int ji, SeqLoop *out)
{
  int n = ir->next_instruction_index;
  IRQuadCompact *cmp_q = &ir->compact_instructions[ci];
  IRQuadCompact *jif_q = &ir->compact_instructions[ji];

  IROperand s1 = tcc_ir_op_get_src1(ir, cmp_q);
  IROperand s2 = tcc_ir_op_get_src2(ir, cmp_q);
  if (!irop_has_vreg(s1) || !irop_is_immediate(s2))
    return 0;
  if (irop_is_64bit(s1) || irop_is_64bit(s2) || tcc_ir_barrel_shift_at(ir, cmp_q))
    return 0;
  int32_t iv = irop_get_vreg(s1);
  int iv_kind = TCCIR_DECODE_VREG_TYPE(iv);
  if (iv_kind != TCCIR_VREG_TYPE_VAR && iv_kind != TCCIR_VREG_TYPE_TEMP)
    return 0;
  int limit = (int)irop_get_imm64_ex(ir, s2);
  int cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jif_q));
  int target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jif_q));
  /* target == n is the function end: the last loop of a chain exits there */
  if (target <= ji || target > n)
    return 0;

  int last = seq_prev_live(ir, target - 1);
  if (last <= ji)
    return 0;

  /* Shape 1 — top-tested: the pair IS the loop test, some branch below it
   * returns to the CMP, and the JUMPIF leaves the loop. */
  for (int e = ji + 1; e <= last; e++)
  {
    if (seq_branch_target(ir, e) != ci)
      continue;
    out->kind = SEQ_TOP_TESTED;
    out->region_lo = ci;
    out->region_hi = last;
    out->exit_idx = target;
    out->iv = iv;
    out->limit = limit;
    out->exit_cond = cond;
    out->guard_cmp = -1;
    out->guard_jif = -1;
    return 1;
  }

  /* Shape 2 — rotated: the pair is the zero-trip guard in front of a
   * bottom-tested body whose back-edge JUMPIF falls through to the guard's own
   * target. */
  {
    int body = seq_next_live(ir, ji + 1);
    if (body >= target)
      return 0;
    IRQuadCompact *back_q = &ir->compact_instructions[last];
    if (back_q->op != TCCIR_OP_JUMPIF || seq_branch_target(ir, last) != body)
      return 0;
    int c = seq_prev_live(ir, last - 1);
    if (c < body || ir->compact_instructions[c].op != TCCIR_OP_CMP)
      return 0;
    IRQuadCompact *tcmp = &ir->compact_instructions[c];
    IROperand t1 = tcc_ir_op_get_src1(ir, tcmp);
    IROperand t2 = tcc_ir_op_get_src2(ir, tcmp);
    if (irop_get_vreg(t1) != iv || !irop_is_immediate(t2))
      return 0;
    if (irop_is_64bit(t1) || irop_is_64bit(t2) || tcc_ir_barrel_shift_at(ir, tcmp))
      return 0;
    int back_cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, back_q));
    int exit_cond = invert_condition(back_cond);
    if (exit_cond < 0)
      return 0;
    out->kind = SEQ_ROTATED;
    out->region_lo = body;
    out->region_hi = last;
    out->exit_idx = target;
    out->iv = iv;
    out->limit = (int)irop_get_imm64_ex(ir, t2);
    out->exit_cond = exit_cond;
    out->guard_cmp = ci;
    out->guard_jif = ji;
    return 1;
  }
}

/* Exit value of a recognized loop given the IV's entry value.  0 when the trip
 * count is not computable, or — for the rotated shape — when the guard is not
 * provably untaken: the pass never assumes a loop it cannot prove is entered. */
static int seq_loop_exit_value(TCCIRState *ir, const SeqLoop *lp, int64_t entry,
                               int64_t *out_exit)
{
  int step;
  if (entry < 0 || entry > 0x7fffffff || lp->limit < 0)
    return 0; /* signed/unsigned equivalence below needs non-negative values */
  if (!seq_region_step(ir, lp->region_lo, lp->region_hi, lp->iv, &step))
    return 0;

  if (lp->kind == SEQ_ROTATED)
  {
    IRQuadCompact *gq = &ir->compact_instructions[lp->guard_cmp];
    IROperand g1 = tcc_ir_op_get_src1(ir, gq);
    IROperand g2 = tcc_ir_op_get_src2(ir, gq);
    IRQuadCompact *gj = &ir->compact_instructions[lp->guard_jif];
    int gcond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, gj));
    int64_t glim = irop_get_imm64_ex(ir, g2);
    if (evaluate_compare_condition_cmp_annotated(ir, gq, entry, glim, gcond, g1, g2) != 0)
      return 0;
  }

  /* The trip count is evaluated against the loop's own test.  Signed vs
   * unsigned is carried by the CMP operands, so let the folder normalize the
   * condition first and only then hand it to the (signed-range) counter. */
  int cmp_idx = (lp->kind == SEQ_TOP_TESTED) ? lp->region_lo : seq_prev_live(ir, lp->region_hi - 1);
  IRQuadCompact *tq = &ir->compact_instructions[cmp_idx];
  IROperand t1 = tcc_ir_op_get_src1(ir, tq);
  IROperand t2 = tcc_ir_op_get_src2(ir, tq);
  if (evaluate_compare_condition_cmp_annotated(ir, tq, entry, lp->limit, lp->exit_cond, t1, t2) < 0)
    return 0;

  int trip = compute_trip_count((int)entry, lp->limit, step, lp->exit_cond);
  if (trip < 0)
    return 0;
  if (lp->kind == SEQ_ROTATED && trip < 1)
    return 0; /* contradicts the guard proof — never rewrite on a paradox */

  int64_t exit_val = entry + (int64_t)trip * step;
  if (exit_val < 0 || exit_val > 0x7fffffff)
    return 0;
  *out_exit = exit_val;
  return 1;
}

/* Shared walker.  Query mode (`query_idx >= 0`) stops at `query_idx` and
 * reports the tracked constant of `query_vreg`; rewrite mode walks the whole
 * function and NOPs every guard it proves untaken, returning the count. */
static int seq_walk(TCCIRState *ir, int query_idx, int32_t query_vreg, int64_t *out_val)
{
  int n = ir->next_instruction_index;
  if (n <= 0 || n > SEQ_MAX_INSTRS)
    return 0;

  SeqState st;
  st.n = 0;
  int changed = 0;
  int32_t escape_iv[SEQ_MAX_TRACK];
  int8_t escape_res[SEQ_MAX_TRACK];
  int nescape = 0;

  int i = 0;
  int resumed_at = -1;
  /* start of the straight-line stretch the current values were carried across,
   * and the loop region that legitimately branches into it */
  int stretch_from = 0, prev_lo = -1, prev_hi = -1;
  while (i < n)
  {
    if (query_idx >= 0 && i >= query_idx)
    {
      /* The caller asks for the value on the ENTRY path of the loop headed at
       * query_idx, so that loop's own back-edges — which come from below it —
       * do not disturb it; anything else landing in the stretch does. */
      for (int j = 0; j < n; j++)
      {
        int t = seq_branch_target(ir, j);
        if (t < stretch_from || t > query_idx)
          continue;
        if (prev_lo >= 0 && j >= prev_lo && j <= prev_hi)
          continue;
        if (t == query_idx && j > query_idx)
          continue;
        return 0;
      }
      return seq_get(&st, query_vreg, out_val);
    }

    IRQuadCompact *q = &ir->compact_instructions[i];
    int op = q->op;
    if (op == TCCIR_OP_NOP)
    {
      i++;
      continue;
    }
    /* A loop head is examined BEFORE the join check below: a top-tested header
     * is always a jump target (its own back-edge) yet the entry-path value
     * still reaches it — seq_stretch_private is what proves that. */
    if (op == TCCIR_OP_CMP)
    {
      int ji = seq_next_live(ir, i + 1);
      SeqLoop lp;
      if (ji < n && ir->compact_instructions[ji].op == TCCIR_OP_JUMPIF &&
          seq_match_loop(ir, i, ji, &lp))
      {
        int64_t entry = 0, exit_val = 0;
        int ok = 0;
        if (seq_stretch_private(ir, stretch_from, i, prev_lo, prev_hi,
                                lp.region_lo, lp.region_hi) &&
            seq_get(&st, lp.iv, &entry))
        {
          int esc = -1;
          for (int k = 0; k < nescape; k++)
            if (escape_iv[k] == lp.iv)
              esc = escape_res[k];
          if (esc < 0)
          {
            esc = seq_vreg_escapes(ir, lp.iv) ? 1 : 0;
            if (nescape < SEQ_MAX_TRACK)
            {
              escape_iv[nescape] = lp.iv;
              escape_res[nescape] = (int8_t)esc;
              nescape++;
            }
          }
          if (!esc && seq_region_closed(ir, lp.region_lo, lp.region_hi,
                                        lp.kind == SEQ_TOP_TESTED ? ji : -1))
            ok = seq_loop_exit_value(ir, &lp, entry, &exit_val);
        }
        if (!ok)
        {
          /* Unmodelled loop: forget everything and resume past it.  Sound —
           * the next guard then needs its own dominating constant def. */
          st.n = 0;
          i = (lp.exit_idx > i) ? seq_next_live(ir, lp.exit_idx) : i + 1;
          stretch_from = i;
          prev_lo = prev_hi = -1;
          resumed_at = -1;
          continue;
        }
        /* Dropping the guard drops its CMP, so nothing on the surviving
         * (fall-through) path may still be reading those flags.  SETIF and
         * SELECT are the two flag consumers that are not a branch. */
        int body_op = ir->compact_instructions[lp.region_lo].op;
        if (lp.kind == SEQ_ROTATED && query_idx < 0 &&
            body_op != TCCIR_OP_SETIF && body_op != TCCIR_OP_SELECT)
        {
          ir->compact_instructions[lp.guard_cmp].op = TCCIR_OP_NOP;
          ir->compact_instructions[lp.guard_jif].op = TCCIR_OP_NOP;
          seq_refresh_jump_target(ir, lp.exit_idx);
          changed++;
          LOG_LOOP_OPT("seq_guard_elim: dropped zero-trip guard [%d,%d] "
                       "(iv entry=%lld exit=%lld)",
                       lp.guard_cmp, lp.guard_jif, (long long)entry, (long long)exit_val);
        }
        st.n = 0;
        seq_set(&st, lp.iv, exit_val);
        prev_lo = (lp.kind == SEQ_ROTATED) ? lp.guard_cmp : lp.region_lo;
        prev_hi = lp.region_hi;
        i = seq_next_live(ir, lp.exit_idx);
        stretch_from = i;
        resumed_at = i;
        continue;
      }
    }

    /* A join point invalidates everything carried along the fall-through. */
    if (q->is_jump_target && i != resumed_at)
    {
      st.n = 0;
      stretch_from = i;
      prev_lo = prev_hi = -1;
    }
    resumed_at = -1;

    if (op == TCCIR_OP_CMP)
    {
      /* A CMP that is not a loop test leaves tracked values alone. */
      i++;
      continue;
    }

    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF || op == TCCIR_OP_IJUMP ||
        op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD ||
        op == TCCIR_OP_RETURNVALUE || op == TCCIR_OP_RETURNVOID || op == TCCIR_OP_TRAP)
    {
      st.n = 0;
      i++;
      stretch_from = i;
      prev_lo = prev_hi = -1;
      continue;
    }

    if (irop_config[op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dv = irop_get_vreg(d);
      if (dv >= 0 && seq_dest_is_direct(d))
      {
        if (op == TCCIR_OP_ASSIGN && !irop_is_64bit(d) &&
            irop_is_immediate(tcc_ir_op_get_src1(ir, q)))
          seq_set(&st, dv, irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, q)));
        else
          seq_drop(&st, dv);
      }
    }
    i++;
  }

  if (query_idx >= 0)
    return 0;
  return changed;
}

int tcc_ir_loop_seq_entry_const(TCCIRState *ir, int at_idx, int32_t vreg, int64_t *out_val)
{
  if (!ir || at_idx <= 0 || vreg < 0 || !out_val)
    return 0;
  if (!tcc_ir_cfg_flat_has_backedge(ir))
    return 0;
  return seq_walk(ir, at_idx, vreg, out_val);
}

int tcc_ir_opt_loop_guard_elim(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;
  if (!tcc_ir_cfg_flat_has_backedge(ir))
    return 0;
  return seq_walk(ir, -1, -1, NULL);
}

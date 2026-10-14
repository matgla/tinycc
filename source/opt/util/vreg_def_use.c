/*
 *  TCC IR - Linear vreg def and single-use scans
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"

/* Counts per VAR, TEMP and PARAM vreg, each type a run of slots sized by the
 * IR's own vreg counters.  Every count is over non-NOP instructions in [0, n)
 * and is exactly what the scans below compute: a def is a dest naming the
 * vreg, a use an instruction naming it in src1 or src2 (once per
 * instruction), an accumulator read a MAC naming it as accumulator.  A vreg
 * outside the slots, or a count that overflowed, is left to the scans. */
#define VIDX_MANY 0xFFFF /* overflowed: no longer known */

typedef struct IRVregIndex
{
  int built;     /* 0: rebuild before the next query */
  int scans;     /* queries answered by scanning since the last rebuild was due */
  int n;         /* next_instruction_index when built */
  int base[4];   /* first slot of each type (VAR..PARAM), base[3] = total */
  int *def_at;   /* the def when there is exactly one; -1 when not known */
  uint16_t *cnt; /* per slot: defs, uses, accumulator reads */
} IRVregIndex;

static int vidx_slot(const IRVregIndex *x, int32_t vreg)
{
  int type = TCCIR_DECODE_VREG_TYPE(vreg);
  /* a negative sentinel or bits between position and type: the scans compare
   * the whole value */
  if (vreg < 0 || type < TCCIR_VREG_TYPE_VAR || type > TCCIR_VREG_TYPE_PARAM ||
      (vreg & ~(0xF << 28 | TCCIR_VREG_POSITION_MASK)))
    return -1;
  int slot = x->base[type - TCCIR_VREG_TYPE_VAR] + TCCIR_DECODE_VREG_POSITION(vreg);
  return slot < x->base[type - TCCIR_VREG_TYPE_VAR + 1] ? slot : -1;
}

static void vidx_add(IRVregIndex *x, int slot, int k, int sign)
{
  if (slot >= 0 && x->cnt[3 * slot + k] != VIDX_MANY)
    x->cnt[3 * slot + k] += sign; /* removing from 0 would wrap to MANY */
}

/* Add (sign 1) or take back (sign -1) instruction i's share of the counts. */
static void vidx_account(TCCIRState *ir, IRVregIndex *x, int i, int sign)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (q->op == TCCIR_OP_NOP)
    return;
  int d = vidx_slot(x, irop_get_vreg(tcc_ir_op_get_dest(ir, q)));
  if (d >= 0)
  {
    vidx_add(x, d, 0, sign);
    /* a lone def added is the def; after one is taken back, which remains is
     * not known */
    x->def_at[d] = sign > 0 && x->cnt[3 * d] == 1 ? i : -1;
  }
  int32_t v1 = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
  int32_t v2 = irop_get_vreg(tcc_ir_op_get_src2(ir, q));
  vidx_add(x, vidx_slot(x, v1), 1, sign);
  if (v2 != v1)
    vidx_add(x, vidx_slot(x, v2), 1, sign);
  if (tcc_ir_op_is_mac(q->op))
    vidx_add(x, vidx_slot(x, irop_get_vreg(tcc_ir_op_get_accum(ir, q))), 2, sign);
}

static void vidx_build(TCCIRState *ir, IRVregIndex *x)
{
  int n = ir->next_instruction_index;

  x->base[0] = 0;
  x->base[1] = ir->next_local_variable + 1;
  x->base[2] = x->base[1] + ir->next_temporary_variable + 1;
  x->base[3] = x->base[2] + ir->next_parameter + 1;
  int slots = x->base[3];
  tcc_free(x->def_at);
  x->def_at = tcc_malloc(slots * (sizeof(int) + 3 * sizeof(uint16_t))); /* def_at, then cnt */
  x->cnt = (uint16_t *)(x->def_at + slots);
  memset(x->cnt, 0, 3 * sizeof(uint16_t) * (size_t)slots);
  for (int i = 0; i < n; i++)
    vidx_account(ir, x, i, 1);
  x->n = n;
  x->built = 1;
}

/* The open index, built for the IR as it is now; NULL when none is open, or
 * when a build is due but not yet worth it: a build costs a few scans, so the
 * first queries after one falls due scan instead. */
static IRVregIndex *vidx_get(TCCIRState *ir)
{
  IRVregIndex *x = ir->vreg_index;
  if (x && (!x->built || x->n != ir->next_instruction_index))
  {
    if (x->scans < 2)
    {
      x->built = 0;
      x->scans++;
      return NULL;
    }
    vidx_build(ir, x);
  }
  return x;
}

int tcc_ir_vreg_index_open(TCCIRState *ir)
{
  if (!ir || ir->vreg_index)
    return 0;
  ir->vreg_index = tcc_mallocz(sizeof(IRVregIndex));
  return 1;
}

void tcc_ir_vreg_index_dirty(TCCIRState *ir)
{
  if (ir && ir->vreg_index)
    ir->vreg_index->built = ir->vreg_index->scans = 0;
}

void tcc_ir_vreg_index_unnote(TCCIRState *ir, int idx)
{
  IRVregIndex *x = ir ? ir->vreg_index : NULL;
  if (x && x->built && idx >= 0 && idx < x->n)
    vidx_account(ir, x, idx, -1);
}

void tcc_ir_vreg_index_note(TCCIRState *ir, int idx)
{
  IRVregIndex *x = ir ? ir->vreg_index : NULL;
  if (x && x->built && idx >= 0 && idx < x->n)
    vidx_account(ir, x, idx, 1);
}

void tcc_ir_vreg_index_close(TCCIRState *ir, int opened)
{
  if (!opened || !ir || !ir->vreg_index)
    return;
  tcc_free(ir->vreg_index->def_at);
  tcc_free(ir->vreg_index);
  ir->vreg_index = NULL;
}

int tcc_ir_vreg_index_defs(TCCIRState *ir, int32_t vreg)
{
  IRVregIndex *x = ir ? vidx_get(ir) : NULL;
  int slot = x ? vidx_slot(x, vreg) : -1;
  if (slot < 0 || x->cnt[3 * slot] == VIDX_MANY)
    return -1;
  return x->cnt[3 * slot];
}

int tcc_ir_find_defining_instruction(TCCIRState *ir, int32_t vreg, int before_idx)
{
  if (!ir || vreg < 0 || before_idx <= 0)
    return -1;

  /* With no def, or one def that is known, the answer needs no walk; past n
   * the walk below reads beyond the function, so leave that to it. */
  IRVregIndex *x = vidx_get(ir);
  int slot = x && before_idx <= x->n ? vidx_slot(x, vreg) : -1;
  if (slot >= 0 && x->cnt[3 * slot] == 0)
    return -1;
  if (slot >= 0 && x->cnt[3 * slot] == 1 && x->def_at[slot] >= 0)
    return x->def_at[slot] < before_idx ? x->def_at[slot] : -1;

  for (int i = before_idx - 1; i >= 0; --i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (tcc_ir_op_dest_vreg(ir, q) == vreg)
      return i;
  }
  return -1;
}

int tcc_ir_vreg_coalesced(TCCIRState *ir, int32_t vreg)
{
  if (!ir || vreg < 0)
    return 0;
  for (int k = 0; k < ir->ls.next_interval_index; k++)
    if (ir->ls.intervals[k].vreg == (uint32_t)vreg)
      return ir->ls.intervals[k].co_member != 0;
  return 0;
}

int tcc_ir_vreg_has_single_use(TCCIRState *ir, int32_t vreg, int exclude_idx)
{
  if (!ir || vreg < 0)
    return 0;

  IRVregIndex *x = vidx_get(ir);
  int slot = x ? vidx_slot(x, vreg) : -1;
  if (slot >= 0 && x->cnt[3 * slot + 1] != VIDX_MANY && x->cnt[3 * slot + 2] != VIDX_MANY)
  {
    int uses = x->cnt[3 * slot + 1], accs = x->cnt[3 * slot + 2];
    /* The scan below skips exclude_idx altogether: take its share back out. */
    if (exclude_idx >= 0 && exclude_idx < x->n && ir->compact_instructions[exclude_idx].op != TCCIR_OP_NOP)
    {
      IRQuadCompact *q = &ir->compact_instructions[exclude_idx];
      uses -= irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vreg || irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == vreg;
      accs -= tcc_ir_op_is_mac(q->op) && irop_get_vreg(tcc_ir_op_get_accum(ir, q)) == vreg;
    }
    return accs == 0 && uses == 1;
  }

  int use_count = 0;
  int n = ir->next_instruction_index;

  for (int i = 0; i < n; ++i)
  {
    if (i == exclude_idx)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* MLA's accumulator (operand_base+3) is a real use that has_src1/2 cannot
     * see.  Callers use "single use" to justify folding a def into its one use
     * site, and they only ever rewrite src1/src2 — so a vreg read in an
     * accumulator is never safely a single use.  Report multi-use rather than
     * counting it: counting would newly admit the accumulator-only case, which
     * today bails (use_count 0).  Same blind spot as the ssa:sccp phi
     * materialization fixed for fuzz seeds volatile:82433 / bitfield:88932. */
    if (tcc_ir_op_is_mac(q->op) &&
        tcc_ir_op_accum_vreg(ir, q) == vreg)
      return 0;


    if (tcc_ir_op_src1_vreg(ir, q) == vreg || tcc_ir_op_src2_vreg(ir, q) == vreg)
    {
      use_count++;
      if (use_count > 1)
        return 0;
    }
  }
  return use_count == 1;
}

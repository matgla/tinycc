/*
 *  TCC IR - Dead local-slot store elimination (flat, pre-SSA)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include <limits.h>

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_alias.h"
#include "opt_loop_utils.h"


/* Kill dead stores to non-escaping stack-local slots.  Bails on IJUMP or any
   Addr[StackLoc] use outside memset PARAM0 (offset liveness needs object
   boundaries the IR pipeline lacks). */

/* A mem* call has no lvalue operand, so its volatility rides on the address
 * PARAMs (vstore leaves them unmarked NONVOLATILE when either side of the copy
 * is volatile).  Such a call writes (or reads) volatile memory: never delete it. */
static int dls_writecall_is_volatile(TCCIRState *ir, int call_idx, IROperand p0)
{
  IROperand p1;
  Sym *callee;

  if (tcc_ir_access_is_volatile(ir, p0))
    return 1;
  /* PARAM1 is an address only for memcpy/memmove (memset's is the fill value). */
  callee = tcc_ir_op_src1_sym(ir, &ir->compact_instructions[call_idx]);
  return callee && ir_opt_is_memcpy_or_memmove_name(get_tok_str(callee->v, NULL)) &&
         ir_opt_get_call_param_operand(ir, call_idx, 1, &p1) && tcc_ir_access_is_volatile(ir, p1);
}

/* memset/memcpy/memmove return their destination: a FUNCCALLVAL whose result
 * is read anywhere hands out PARAM0's address where no PARAM shows it, so the
 * call is no plain write (its PARAMs then count as escapes). */
static int dls_call_result_used(TCCIRState *ir, int n, int ci)
{
  IRQuadCompact *cq = &ir->compact_instructions[ci];
  if (cq->op != TCCIR_OP_FUNCCALLVAL)
    return 0;
  IROperand r = tcc_ir_op_get_dest(ir, cq);
  if (irop_is_none(r))
    return 0;
  if (!irop_has_vreg(r) || r.is_lval || irop_get_vreg(r) < 0)
    return 1;
  const int32_t vr = irop_get_vreg(r);
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    const IRRegistersConfig *cfg = &irop_config[q->op];
    IROperand ops[4];
    int np = 0;
    if (cfg->has_dest && j != ci)
      ops[np++] = tcc_ir_op_get_dest(ir, q); /* a deref through it, or a redefinition: either way */
    if (cfg->has_src1)
      ops[np++] = tcc_ir_op_get_src1(ir, q);
    if (cfg->has_src2)
      ops[np++] = tcc_ir_op_get_src2(ir, q);
    if (tcc_ir_op_is_mac(q->op) || q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_SELECT)
      ops[np++] = ir->iroperand_pool[q->operand_base + 3];
    for (int k = 0; k < np; k++)
      if (irop_has_vreg(ops[k]) && irop_get_vreg(ops[k]) == vr)
        return 1;
  }
  return 0;
}

/* Resolve a TEMP vreg to its exact frame offset via Addr[StackLoc](+/-#const)*(ASSIGN/LEA)* chains; returns 1 iff every def-chain step is constant. */
static int dls_vreg_frame_off(TCCIRState *ir, int32_t vr, int before_idx, int *out_off)
{
  long acc = 0;
  for (int guard = 0; guard < 32; guard++)
  {
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
    int d = tcc_ir_find_defining_instruction(ir, vr, before_idx);
    if (d < 0)
      return 0;
    IRQuadCompact *dq = &ir->compact_instructions[d];
    if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_SUB &&
        dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_LEA)
      return 0;
    IROperand s1 = tcc_ir_op_get_src1(ir, dq);
    if (dq->op == TCCIR_OP_ADD || dq->op == TCCIR_OP_SUB)
    {
      if (!tcc_ir_op_src2_is_imm(ir, dq) || tcc_ir_op_src2_is_sym(ir, dq))
        return 0;
      long c = (long)tcc_ir_op_src2_imm(ir, dq);
      acc += (dq->op == TCCIR_OP_SUB) ? -c : c;
    }
    /* s1 is the base: a direct Addr[StackLoc] terminates the walk, else recurse. */
    if (irop_get_tag(s1) == IROP_TAG_STACKOFF && s1.is_local && !s1.is_lval &&
        irop_get_vreg(s1) == -1)
    {
      long off = (long)irop_get_stack_offset(s1) + acc;
      if (off < INT_MIN || off > INT_MAX)
        return 0;
      *out_off = (int)off;
      return 1;
    }
    if (s1.is_lval || irop_get_vreg(s1) < 0)
      return 0;
    vr = irop_get_vreg(s1);
    before_idx = d;
  }
  return 0;
}

/* Parallel per-slot tame state; end[] bounds a slot's extent so a non-tame
 * deref through one slot can't poison eliminations on non-overlapping slots. */
typedef struct DlsTameSlots
{
  int *off;
  uint8_t *ok;
  int *end;
  int n, cap;
} DlsTameSlots;

/* Index of the tame slot at offset `off`, or -1. */
static int dls_tame_find(const DlsTameSlots *ts, int off)
{
  for (int i = 0; i < ts->n; i++)
    if (ts->off[i] == off)
      return i;
  return -1;
}

/* 1 iff [off, off + width) intersects any non-tame slot's bounded extent (its
 * deref might observe the write); has_unknown_deref is a hard bail. */
static int dls_nontame_overlaps(const DlsTameSlots *ts, int has_unknown_deref, int off, int width)
{
  int hit = has_unknown_deref;
  for (int t = 0; !hit && t < ts->n; t++)
  {
    if (ts->ok[t])
      continue;
    int ns = ts->off[t];
    int ne = ts->end[t];
    if (off < ne && ns < off + width)
      hit = 1;
  }
  return hit;
}

/* Index of the tame slot at offset `off`, adding it (tame, end 0) if new. */
static int dls_tame_find_or_add(DlsTameSlots *ts, int off)
{
  int x = dls_tame_find(ts, off);
  if (x < 0)
  {
    if (ts->n >= ts->cap)
    {
      ts->cap *= 2;
      ts->off = tcc_realloc(ts->off, (size_t)ts->cap * sizeof(int));
      ts->ok = tcc_realloc(ts->ok, (size_t)ts->cap * sizeof(uint8_t));
      ts->end = tcc_realloc(ts->end, (size_t)ts->cap * sizeof(int));
    }
    x = ts->n++;
    ts->off[x] = off;
    ts->ok[x] = 1;
    ts->end[x] = 0;
  }
  return x;
}

/* Flat index of vreg vr into the per-vreg tables (TEMPs, then VARs, then
 * PARAMs), or -1 when it is none of those or out of range. */
static int dls_vr_flat(const TCCIRState *ir, int max_vreg, int32_t vr)
{
  int t = TCCIR_DECODE_VREG_TYPE(vr);
  int p = TCCIR_DECODE_VREG_POSITION(vr);
  int b = -1;
  if (t == TCCIR_VREG_TYPE_TEMP)
    b = p;
  else if (t == TCCIR_VREG_TYPE_VAR)
    b = ir->next_temporary_variable + p;
  else if (t == TCCIR_VREG_TYPE_PARAM)
    b = ir->next_temporary_variable + ir->next_local_variable + p;
  return (b >= 0 && b < max_vreg) ? b : -1;
}

/* (offset, width, position) read/escape ranges for position-aware liveness. */
typedef struct DlsLiveRange
{
  int off;
  int width;
  int pos;
} DlsLiveRange;

typedef struct DlsLive
{
  DlsLiveRange *r;
  int n, cap;
} DlsLive;

static void dls_live_add(DlsLive *l, int off, int width, int pos)
{
  if (l->n >= l->cap)
  {
    l->cap *= 2;
    l->r = tcc_realloc(l->r, sizeof(DlsLiveRange) * l->cap);
  }
  l->r[l->n].off = off;
  l->r[l->n].width = width;
  l->r[l->n].pos = pos;
  l->n++;
}

int tcc_ir_opt_dead_local_slot_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Nested function: StackLoc is the parent's frame via static chain — its reads are invisible, so any store may be live. */
  if (ir->captured_count > 0 || ir->has_static_chain)
    return 0;

  /* Precise vreg-deref relaxation is only sound when all slot reads are in lv.r[]; indexed/postinc loads aren't recorded, so they disable it. */
  int dls_has_indexed = 0;
  /* Position-based liveness (read.pos > store.pos) is only sound in forward-only CFG; a back-edge makes a loop-carried store look dead, so it disables the relaxation. */
  int dls_has_backedge = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int op = q->op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
    if (op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED ||
        op == TCCIR_OP_LOAD_POSTINC || op == TCCIR_OP_STORE_POSTINC)
      dls_has_indexed = 1;
    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF)
    {
      int tg = (int)tcc_ir_op_dest_u_imm32(ir, q);
      if (tg >= 0 && tg <= i)
        dls_has_backedge = 1;
    }
    if (op == TCCIR_OP_SWITCH_TABLE)
      dls_has_backedge = 1; /* targets unknown here — conservatively a back-edge */
  }
  int dls_precise_ok = !dls_has_indexed && !dls_has_backedge;

  int max_call_id = ir->next_call_id;
  /* Per-call bitmaps: write-through-PARAM0 helpers (memset/memcpy/memmove). */
  uint8_t *is_writecall = NULL;       /* any of: memset/memcpy/memmove family */
  uint8_t *writecall_size_at_2 = NULL; /* 1 = size is param 2; 0 = size is param 1 */
  uint8_t *is_memcpy_like = NULL;      /* also READ through PARAM1 (bounded) */
  int writecall_count = 0;
  if (max_call_id > 0)
  {
    is_writecall = tcc_mallocz((max_call_id + 7) / 8);
    writecall_size_at_2 = tcc_mallocz((max_call_id + 7) / 8);
    is_memcpy_like = tcc_mallocz((max_call_id + 7) / 8);
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL)
      continue;
    Sym *callee = tcc_ir_op_src1_sym(ir, q);
    if (!callee)
      continue;
    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
      continue;
    int sz_at_2 = -1;
    int memcpy_like = 0;
    int ms_size_idx, ms_fill_idx;
    if (ir_opt_memset_params(name, &ms_size_idx, &ms_fill_idx))
      sz_at_2 = ms_size_idx == 2;
    else if (ir_opt_is_memcpy_or_memmove_name(name))
    {
      sz_at_2 = 1;
      memcpy_like = 1;
    }
    if (sz_at_2 < 0 || dls_call_result_used(ir, n, i))
      continue;
    int cid = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
    if (cid < 0 || cid >= max_call_id || !is_writecall)
      continue;
    is_writecall[cid / 8] |= (1 << (cid % 8));
    if (sz_at_2)
      writecall_size_at_2[cid / 8] |= (1 << (cid % 8));
    if (memcpy_like)
      is_memcpy_like[cid / 8] |= (1 << (cid % 8));
    writecall_count++;
  }

  /* Tameness analysis: prove address-taken local slots that never reach a read stay eliminable. */
  int max_vreg = ir->next_temporary_variable + ir->next_local_variable + ir->next_parameter + 16;
  /* vreg_slot[V]: -1 unknown, VR_SLOT_AMBIG (INT_MIN) ambiguous, else the frame offset V points to. */
  int *vreg_slot = tcc_malloc((size_t)max_vreg * sizeof(int));
  for (int v = 0; v < max_vreg; v++)
    vreg_slot[v] = -1;
  /* vreg_external[V]=1: V provably came from external memory (param or ptr-arith on one), so can't alias the local frame. */
  unsigned char *vreg_external = tcc_mallocz((size_t)max_vreg);
#define VR_SLOT_AMBIG INT_MIN

  /* Parallel per-slot tame state; ts.end[] bounds a slot's extent so a non-tame deref through one slot can't poison eliminations on non-overlapping slots. */
  DlsTameSlots ts;
  ts.cap = 32;
  ts.off = tcc_malloc((size_t)ts.cap * sizeof(int));
  ts.ok = tcc_malloc((size_t)ts.cap * sizeof(uint8_t));
  ts.end = tcc_malloc((size_t)ts.cap * sizeof(int));
  ts.n = 0;
  /* An unknown-slot TMP deref could touch any frame byte; disables the "off not in tame_slot → eligible" shortcut. */
  int has_unknown_deref = 0;

  /* Step 1: seed vreg_slot[] from direct addr-of-local sources. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA &&
        q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    int dv = dls_vr_flat(ir, max_vreg, tcc_ir_op_dest_vreg(ir, q));
    if (dv < 0)
      continue;
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(s1) != IROP_TAG_STACKOFF || !s1.is_local || s1.is_lval || irop_get_vreg(s1) != -1)
      continue;
    int slot = irop_get_stack_offset(s1);
    dls_tame_find_or_add(&ts, slot);
    if (vreg_slot[dv] == -1)
      vreg_slot[dv] = slot;
    else if (vreg_slot[dv] != slot)
      vreg_slot[dv] = VR_SLOT_AMBIG;
  }

  /* Step 2: propagate slot membership through ASSIGN/ADD/SUB; exact offset is irrelevant for tameness. */
  int changed = 1;
  while (changed)
  {
    changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA &&
          q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
        continue;
      if (!irop_config[q->op].has_dest)
        continue;
      int dv = dls_vr_flat(ir, max_vreg, tcc_ir_op_dest_vreg(ir, q));
      if (dv < 0)
        continue;
      int sv1 = dls_vr_flat(ir, max_vreg, tcc_ir_op_src1_vreg(ir, q));
      int s1_slot = (sv1 >= 0) ? vreg_slot[sv1] : -1;
      int next = s1_slot;
      if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA)
      {
        int sv2 = dls_vr_flat(ir, max_vreg, tcc_ir_op_src2_vreg(ir, q));
        int s2_slot = (sv2 >= 0) ? vreg_slot[sv2] : -1;
        /* &slot + ptr-to-slot' is ambiguous. */
        if (s2_slot != -1)
        {
          if (s1_slot == -1)
            next = s2_slot;
          else if (s1_slot != s2_slot)
            next = VR_SLOT_AMBIG;
        }
      }
      if (next == -1)
        continue;
      int prev = vreg_slot[dv];
      if (prev == -1)
      {
        vreg_slot[dv] = next;
        changed = 1;
      }
      else if (prev == VR_SLOT_AMBIG)
      {
        /* already poisoned */
      }
      else if (prev != next)
      {
        vreg_slot[dv] = VR_SLOT_AMBIG;
        changed = 1;
      }
    }
  }

  /* Step 2b: seed and propagate vreg_external (param/imm sources → external). */
  {
    int changed_ext = 1;
    while (changed_ext)
    {
      changed_ext = 0;
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB)
          continue;
        if (!irop_config[q->op].has_dest)
          continue;
        int dv = dls_vr_flat(ir, max_vreg, tcc_ir_op_dest_vreg(ir, q));
        if (dv < 0 || vreg_external[dv])
          continue;
        if (vreg_slot[dv] != -1)
          continue; /* known to carry a stack slot — not "external" */
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        IROperand s2 = irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : s1;
        int sv1 = dls_vr_flat(ir, max_vreg, irop_get_vreg(s1));
        int sv2 = irop_config[q->op].has_src2 ? dls_vr_flat(ir, max_vreg, irop_get_vreg(s2)) : -1;
        int s1_param = (irop_get_vreg(s1) != -1 && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s1)) == TCCIR_VREG_TYPE_PARAM &&
                        !s1.is_lval);
        int s1_ext = s1_param || (sv1 >= 0 && vreg_external[sv1]);
        int s1_const = (irop_get_tag(s1) == IROP_TAG_IMM32 || irop_get_tag(s1) == IROP_TAG_I64);
        int s2_param = (irop_config[q->op].has_src2 && irop_get_vreg(s2) != -1 &&
                        TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s2)) == TCCIR_VREG_TYPE_PARAM && !s2.is_lval);
        int s2_ext = s2_param || (sv2 >= 0 && vreg_external[sv2]);
        int s2_const = irop_config[q->op].has_src2 &&
                       (irop_get_tag(s2) == IROP_TAG_IMM32 || irop_get_tag(s2) == IROP_TAG_I64);
        int ok;
        if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA)
          ok = (s1_ext || s1_const);
        else /* ADD/SUB: both must be ext/const (and at least one non-const) */
          ok = (s1_ext && (s2_ext || s2_const)) || (s2_ext && (s1_ext || s1_const));
        if (ok && !vreg_external[dv])
        {
          vreg_external[dv] = 1;
          changed_ext = 1;
        }
      }
    }
  }

  /* Collect stack lval read ranges: an addr-of-local stored to a never-read slot is a dead escape, so the source slot stays tame. */
  typedef struct
  {
    int off;
    int width;
  } StackLvalRead;
  int slr_cap = 32, slr_count = 0;
  StackLvalRead *slr = tcc_malloc(sizeof(StackLvalRead) * slr_cap);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int is_store_q = (q->op == TCCIR_OP_STORE);
    for (int k = 0; k < 3; k++)
    {
      if (k == 0 && is_store_q)
        continue;
      IROperand op;
      int has_op;
      if (k == 0)
      {
        has_op = irop_config[q->op].has_dest;
        if (has_op)
          op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        has_op = irop_config[q->op].has_src1;
        if (has_op)
          op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        has_op = irop_config[q->op].has_src2;
        if (has_op)
          op = tcc_ir_op_get_src2(ir, q);
      }
      if (!has_op)
        continue;
      if (irop_get_tag(op) != IROP_TAG_STACKOFF || !op.is_local || !op.is_lval || irop_get_vreg(op) != -1)
        continue;
      if (q->op == TCCIR_OP_BLOCK_COPY && k == 0)
        continue;
      int soff = irop_get_stack_offset(op);
      int sw = ir_opt_store_btype_size_bytes(irop_get_btype(op));
      if (sw <= 0)
        sw = irop_is_64bit(op) ? 8 : 4;
      if (op.is_complex)
        sw *= 2;
      if (slr_count >= slr_cap)
      {
        slr_cap *= 2;
        slr = tcc_realloc(slr, sizeof(StackLvalRead) * slr_cap);
      }
      slr[slr_count].off = soff;
      slr[slr_count].width = sw;
      slr_count++;
    }
  }

  /* Step 3: classify every use of a derived-address vreg; an unrecognized pattern marks its slot non-tame. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Check if this is a write-call PARAM0 (which is harmless). */
    int is_write_p0 = 0;
    /* PARAM1 of memcpy/memmove is a bounded read (modelled as a lv.r[] range later), so tag it tame here. */
    int is_memcpy_src_bounded = 0;
    if ((q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID) && is_writecall)
    {
      uint32_t enc = (uint32_t)tcc_ir_op_src2_imm(ir, q);
      int cid = TCCIR_DECODE_CALL_ID(enc);
      int pidx = TCCIR_DECODE_PARAM_IDX(enc);
      if (cid >= 0 && cid < max_call_id && (is_writecall[cid / 8] & (1 << (cid % 8))) && pidx == 0)
        is_write_p0 = 1;
      else if (cid >= 0 && cid < max_call_id && (is_memcpy_like[cid / 8] & (1 << (cid % 8))) && pidx == 1)
      {
        /* memcpy/memmove source pointer: need a constant size to bound the read, else leave non-tame. */
        IROperand sz_op;
        for (int j = i + 1; j < n; j++)
        {
          IRQuadCompact *qj = &ir->compact_instructions[j];
          if (qj->op == TCCIR_OP_NOP)
            continue;
          if (qj->op != TCCIR_OP_FUNCPARAMVAL && qj->op != TCCIR_OP_FUNCPARAMVOID &&
              qj->op != TCCIR_OP_FUNCCALLVOID && qj->op != TCCIR_OP_FUNCCALLVAL)
            continue;
          if (qj->op == TCCIR_OP_FUNCPARAMVAL || qj->op == TCCIR_OP_FUNCPARAMVOID)
          {
            uint32_t encj = (uint32_t)tcc_ir_op_src2_imm(ir, qj);
            if (TCCIR_DECODE_CALL_ID(encj) == cid && TCCIR_DECODE_PARAM_IDX(encj) == 2)
            {
              sz_op = tcc_ir_op_get_src1(ir, qj);
              if (irop_get_tag(sz_op) == IROP_TAG_IMM32)
                is_memcpy_src_bounded = 1;
              break;
            }
          }
          else if (TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, qj)) == cid)
            break;
        }
      }
    }

    /* Scan each operand role for addr-of-local uses (direct Addr[StackLoc] or a vreg with vreg_slot[] set); unrecognized uses mark the slot non-tame.  k==3 is the MLA accumulator: a slot deref appearing only as an MLA addend must be seen here, else the slot looks tame with no recorded read. */
    for (int k = 0; k < 4; k++)
    {
      IROperand op;
      int has;
      if (k == 0)
      {
        has = irop_config[q->op].has_dest;
        if (has)
          op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        has = irop_config[q->op].has_src1;
        if (has)
          op = tcc_ir_op_get_src1(ir, q);
      }
      else if (k == 2)
      {
        has = irop_config[q->op].has_src2;
        if (has)
          op = tcc_ir_op_get_src2(ir, q);
      }
      else
      {
        has = (q->op == TCCIR_OP_MLA);
        if (has)
          op = tcc_ir_op_get_accum(ir, q);
      }
      if (!has)
        continue;

      int slot = INT_MIN; /* sentinel for "no slot involved" */
      if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && !op.is_lval && irop_get_vreg(op) == -1)
        slot = irop_get_stack_offset(op);

      int vr = irop_get_vreg(op);
      int v_slot = VR_SLOT_AMBIG + 1; /* "unknown" sentinel < AMBIG */
      if (vr != -1)
      {
        int vf = dls_vr_flat(ir, max_vreg, vr);
        if (vf >= 0)
          v_slot = vreg_slot[vf];
      }

      /* TMP lval src is a deref: known slot → non-tame; unknown slot → poison all + has_unknown_deref.  VAR/PARAM lval just loads the value, harmless. */
      int vr_type = (vr != -1) ? TCCIR_DECODE_VREG_TYPE(vr) : 0;
      if (op.is_lval && vr != -1 && k != 0 && vr_type == TCCIR_VREG_TYPE_TEMP)
      {
        int vf2 = dls_vr_flat(ir, max_vreg, vr);
        int is_external = (vf2 >= 0 && vreg_external && vreg_external[vf2]);
        if ((v_slot == -1 || v_slot == VR_SLOT_AMBIG) && !is_external)
        {
          has_unknown_deref = 1;
          for (int t = 0; t < ts.n; t++)
            ts.ok[t] = 0;
        }
        else if (v_slot != -1 && v_slot != VR_SLOT_AMBIG && v_slot != VR_SLOT_AMBIG + 1)
        {
          int foff;
          /* A deref via a resolvable exact offset is recorded precisely in lv.r[] below, so no poison (gated on dls_precise_ok). */
          if (!dls_precise_ok || !dls_vreg_frame_off(ir, vr, i, &foff))
          {
            int idx = dls_tame_find_or_add(&ts, v_slot);
            if (idx >= 0)
              ts.ok[idx] = 0;
          }
        }
      }

      /* Direct addr-of-local: classify in-place. */
      if (slot != INT_MIN)
      {
        int idx = dls_tame_find_or_add(&ts, slot);
        int tame_here = 0;
        switch (q->op)
        {
        case TCCIR_OP_ASSIGN:
        case TCCIR_OP_LEA:
          /* dest <-- &slot is the seed itself; harmless. */
          tame_here = (k == 1);
          break;
        case TCCIR_OP_ADD:
        case TCCIR_OP_SUB:
        {
          if (k == 1 && (tcc_ir_op_src2_tag(ir, q) == IROP_TAG_IMM32 || tcc_ir_op_src2_tag(ir, q) == IROP_TAG_I64))
            tame_here = 1;
          else if (k == 1)
          {
            int adv = dls_vr_flat(ir, max_vreg, tcc_ir_op_dest_vreg(ir, q));
            if (adv >= 0 && vreg_slot[adv] == slot)
              tame_here = 1;
          }
          break;
        }
        case TCCIR_OP_CMP:
          tame_here = (k == 1 || k == 2);
          break;
        case TCCIR_OP_FUNCPARAMVAL:
        case TCCIR_OP_FUNCPARAMVOID:
          if ((is_write_p0 || is_memcpy_src_bounded) && k == 1)
            tame_here = 1;
          break;
        default:
          break;
        }
        if (!tame_here)
          ts.ok[idx] = 0;
      }

      /* Vreg use of a derived-address vreg: plain value (is_lval=0) IS the address; a VAR/PARAM lval load yields the stored address too (so calls like strlen(local_ptr) get classified).  TMP lval is a deref, handled above.  Skip the dest role for ADD/SUB/ASSIGN (propagation target, not a use). */
      int classify_use = (vr != -1 && v_slot != -1 && v_slot != VR_SLOT_AMBIG + 1);
      if (classify_use && op.is_lval) {
        int vt = TCCIR_DECODE_VREG_TYPE(vr);
        if (vt != TCCIR_VREG_TYPE_VAR && vt != TCCIR_VREG_TYPE_PARAM)
          classify_use = 0;
      }
      if (classify_use)
      {
        int idx = (v_slot == VR_SLOT_AMBIG) ? -1 : dls_tame_find_or_add(&ts, v_slot);
        int tame_here = 0;
        switch (q->op)
        {
        case TCCIR_OP_ASSIGN:
        case TCCIR_OP_LEA:
          /* Propagation step; the actual use (src1) is already known harmless. */
          tame_here = 1;
          break;
        case TCCIR_OP_ADD:
        case TCCIR_OP_SUB:
        {
          if (k == 0)
            tame_here = 1; /* dest: propagation target, not a use */
          else if (k == 1 && (tcc_ir_op_src2_tag(ir, q) == IROP_TAG_IMM32 || tcc_ir_op_src2_tag(ir, q) == IROP_TAG_I64))
            tame_here = 1;
          else if (k == 1)
          {
            int adv = dls_vr_flat(ir, max_vreg, tcc_ir_op_dest_vreg(ir, q));
            if (adv >= 0 && vreg_slot[adv] == v_slot)
              tame_here = 1;
          }
          /* k == 2 (V on the RHS as src2): only OK in narrow cases — bail. */
          break;
        }
        case TCCIR_OP_CMP:
          tame_here = 1;
          break;
        case TCCIR_OP_STORE:
        case TCCIR_OP_STORE_INDEXED:
        case TCCIR_OP_STORE_POSTINC:
          /* Vreg-as-dest is the address (write through V). */
          if (k == 0)
            tame_here = 1;
          else if (k == 1 && q->op == TCCIR_OP_STORE)
          {
            /* Address stored into a never-read local slot is dead and cannot escape. */
            IROperand sdest = tcc_ir_op_get_dest(ir, q);
            if (irop_get_tag(sdest) == IROP_TAG_STACKOFF && sdest.is_local &&
                irop_get_vreg(sdest) == -1)
            {
              int sdoff = irop_get_stack_offset(sdest);
              int sdw = ir_opt_store_btype_size_bytes(irop_get_btype(sdest));
              if (sdw <= 0)
                sdw = 4;
              int target_read = 0;
              for (int sr = 0; sr < slr_count; sr++)
                if (sdoff < slr[sr].off + slr[sr].width &&
                    sdoff + sdw > slr[sr].off)
                {
                  target_read = 1;
                  break;
                }
              if (!target_read && dls_tame_find(&ts, sdoff) < 0)
                tame_here = 1;
            }
          }
          /* Vreg-as-src1 stored to a live slot: address escapes to memory — non-tame (falls through). */
          break;
        case TCCIR_OP_FUNCPARAMVAL:
        case TCCIR_OP_FUNCPARAMVOID:
          if ((is_write_p0 || is_memcpy_src_bounded) && k == 1)
            tame_here = 1;
          /* Any other PARAM — callee may dereference, non-tame. */
          break;
        default:
          break;
        }
        if (!tame_here)
        {
          if (v_slot == VR_SLOT_AMBIG)
          {
            for (int t = 0; t < ts.n; t++)
              ts.ok[t] = 0;
          }
          else if (idx >= 0)
            ts.ok[idx] = 0;
        }
      }
    }
  }

  /* Collect (offset,width,position) read/escape ranges for position-aware liveness: a STORE at i is dead if no read after i touches the same bytes. */
  DlsLive lv;
  lv.cap = 64;
  lv.r = tcc_malloc(sizeof(DlsLiveRange) * lv.cap);
  lv.n = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* BLOCK_COPY's dest is a pure write of a bounded range (size in src2), just
     * like a STORE's -- recording it as a read would make every copy into a
     * frame buffer look live and no dead one could ever be removed. */
    int is_store = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_BLOCK_COPY);

    for (int k = 0; k < 4; k++)
    {
      if (k == 0 && is_store)
        continue;
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else if (k == 2)
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      else
      {
        /* MLA's 4th source (accumulator): a local-slot value consumed only as an MLA addend would otherwise look dead — record it like a normal src. */
        if (q->op != TCCIR_OP_MLA)
          continue;
        op = tcc_ir_op_get_accum(ir, q);
      }

      /* A TEMP lval src resolving to an exact frame offset is an explicit read; record it (mirrors the no-poison decision above, keeping lv.r[] complete). */
      if (dls_precise_ok && k != 0 && op.is_lval)
      {
        int rvr = irop_get_vreg(op);
        if (rvr != -1 && TCCIR_DECODE_VREG_TYPE(rvr) == TCCIR_VREG_TYPE_TEMP)
        {
          int foff;
          if (dls_vreg_frame_off(ir, rvr, i, &foff))
          {
            int w = ir_opt_store_btype_size_bytes(irop_get_btype(op));
            if (w <= 0)
              w = irop_is_64bit(op) ? 8 : 4;
            if (op.is_complex)
              w *= 2;
            dls_live_add(&lv, foff, w, i);
          }
        }
      }

      if (irop_get_tag(op) != IROP_TAG_STACKOFF)
        continue;
      if (!op.is_local)
        continue;
      if (irop_get_vreg(op) != -1)
        continue;
      /* STRUCT lval width isn't recoverable → poison that slot; addr-of was already classified above.  COMPLEX width is recoverable (2 * scalar). */
      if (op.is_lval && irop_get_btype(op) == IROP_BTYPE_STRUCT)
      {
        /* BLOCK_COPY's dest is a bounded wide write (size in src2), so it doesn't taint tameness. */
        if (q->op == TCCIR_OP_BLOCK_COPY && k == 0)
          continue;
        int off = irop_get_stack_offset(op);
        int tidx = dls_tame_find_or_add(&ts, off);
        ts.ok[tidx] = 0;
        continue;
      }
      int off = irop_get_stack_offset(op);

      if (op.is_lval)
      {
        int w = ir_opt_store_btype_size_bytes(irop_get_btype(op));
        if (w <= 0)
          w = irop_is_64bit(op) ? 8 : 4;
        /* _Complex T touches 2 * sizeof(T) consecutive bytes (both halves). */
        if (op.is_complex)
          w *= 2;
        dls_live_add(&lv, off, w, i);
      }
    }
  }

  /* Bounded-read lv.r[] for memcpy/memmove sources: emit the read range the callee will actually read. */
  if (is_memcpy_like)
  {
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
        continue;
      int cid = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
      if (cid < 0 || cid >= max_call_id)
        continue;
      if (!(is_memcpy_like[cid / 8] & (1 << (cid % 8))))
        continue;
      IROperand src_p, sz_p;
      if (!ir_opt_get_call_param_operand(ir, i, 1, &src_p))
        continue;
      if (!ir_opt_get_call_param_operand(ir, i, 2, &sz_p))
        continue;
      if (irop_get_tag(sz_p) != IROP_TAG_IMM32)
        continue;
      int sz = (int)irop_get_imm64_ex(ir, sz_p);
      if (sz <= 0)
        continue;
      /* Direct addr-of-local source */
      if (irop_get_tag(src_p) == IROP_TAG_STACKOFF && src_p.is_local && !src_p.is_lval &&
          irop_get_vreg(src_p) == -1)
      {
        int off = irop_get_stack_offset(src_p);
        dls_live_add(&lv, off, sz, i);
      }
      else
      {
        int vr = irop_get_vreg(src_p);
        if (vr == -1)
          continue;
        int vf = dls_vr_flat(ir, max_vreg, vr);
        if (vf < 0)
          continue;
        int slot = vreg_slot[vf];
        if (slot == -1)
          continue;
        /* The source was tagged tame above on the promise that its read is
         * recorded here, so every case must record one: the exact bytes when
         * the pointer resolves to an offset; else everything from its slot up
         * (it may point anywhere inside, `&x + 16`); else -- a pointer into
         * one of several slots, `p = c ? &a : &b.f` -- the whole frame.  The
         * SD driver's get_config lost its config copy to the last case. */
        int foff;
        if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && dls_vreg_frame_off(ir, vr, i, &foff))
          dls_live_add(&lv, foff, sz, i);
        else if (slot != VR_SLOT_AMBIG)
          dls_live_add(&lv, slot, 1 << 28, i);
        else
          dls_live_add(&lv, -(1 << 28), 1 << 29, i);
      }
    }
  }

  int changes = 0;

  /* Bound each tame slot's extent by the nearest higher tame-slot offset (a confirmed allocation start); a safe over-approximation, 0 = frame top. */
  for (int t = 0; t < ts.n; t++)
  {
    int s = ts.off[t];
    int end = 0;
    int best_set = 0;
    for (int u = 0; u < ts.n; u++)
    {
      int o = ts.off[u];
      if (o > s && (!best_set || o < end))
      {
        end = o;
        best_set = 1;
      }
    }
    ts.end[t] = end;
  }

  /* STORE and direct-PARAM0 elimination skip entirely if any slot escaped non-tamely (its deref could read anywhere in the slot, bounds unknown). */
  int any_nontame = has_unknown_deref;
  for (int t = 0; !any_nontame && t < ts.n; t++)
    if (!ts.ok[t])
      any_nontame = 1;

  if (!any_nontame)
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    /* A volatile access is observable even when nothing reads the slot back. */
    if (tcc_ir_instr_access_is_volatile(ir, q))
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_tag(dest) != IROP_TAG_STACKOFF)
      continue;
    if (!dest.is_local || irop_get_vreg(dest) != -1)
      continue;
    int off = irop_get_stack_offset(dest);
    int width;
    if (irop_get_btype(dest) == IROP_BTYPE_STRUCT)
    {
      /* STRUCT width unrecoverable, but any_nontame==0 means no STRUCT lval read exists; bound by distance to frame top. */
      width = off < 0 ? -off : 4;
    }
    else
    {
      width = ir_opt_store_btype_size_bytes(irop_get_btype(dest));
      if (width <= 0)
        continue;
    }
    if (dest.is_complex)
      width *= 2;
    /* Position-aware liveness: only reads after this STORE make it live; with a back-edge any overlapping read keeps it (a read may execute after the store). */
    int alive = 0;
    for (int k = 0; k < lv.n; k++)
      if ((dls_has_backedge || lv.r[k].pos > i) &&
          off < lv.r[k].off + lv.r[k].width && off + width > lv.r[k].off)
      {
        alive = 1;
        break;
      }
    if (alive)
      continue;
    LOG_IR_GEN("DEAD LOCAL SLOT: nop STORE to StackLoc[%d] at i=%d w=%d", off, i, width);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* STORE through a vreg resolving to an exact frame offset: offset-level liveness like the direct-StackLoc block, gated on !any_nontame and dls_precise_ok.  Also removes a latent wild store the RA would otherwise emit. */
  if (!any_nontame && dls_precise_ok)
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    /* A volatile access is observable even when nothing reads the slot back. */
    if (tcc_ir_instr_access_is_volatile(ir, q))
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!dest.is_lval)
      continue;
    int vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (irop_get_btype(dest) == IROP_BTYPE_STRUCT)
      continue;
    int foff;
    if (!dls_vreg_frame_off(ir, vr, i, &foff))
      continue;
    int width = ir_opt_store_btype_size_bytes(irop_get_btype(dest));
    if (width <= 0)
      continue;
    if (dest.is_complex)
      width *= 2;
    if (dls_nontame_overlaps(&ts, has_unknown_deref, foff, width))
      continue;
    int alive = 0;
    for (int kk = 0; kk < lv.n; kk++)
      if (lv.r[kk].pos > i && foff < lv.r[kk].off + lv.r[kk].width && foff + width > lv.r[kk].off)
      {
        alive = 1;
        break;
      }
    if (alive)
      continue;
    LOG_IR_GEN("DEAD LOCAL SLOT: nop STORE via known-offset vreg at i=%d (off=%d w=%d)", i, foff, width);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* STORE_INDEXED/STORE_POSTINC via a vreg V → tame slot S: eliminate when S is tame, no non-tame deref reaches it, and no later read intersects [S, ts.end).  Whole-slot liveness (exact offset unknown).  Skip entirely if any indexed/postinc load exists (not recorded in lv.r[]). */
  if (!has_unknown_deref && !dls_has_indexed)
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC)
      continue;
    /* A volatile access is observable even when nothing reads the slot back. */
    if (tcc_ir_instr_access_is_volatile(ir, q))
      continue;
    int vr = tcc_ir_op_dest_vreg(ir, q);
    if (vr == -1)
      continue;
    int vf = dls_vr_flat(ir, max_vreg, vr);
    if (vf < 0)
      continue;
    int slot = vreg_slot[vf];
    if (slot == -1 || slot == VR_SLOT_AMBIG)
      continue;
    int tidx = dls_tame_find(&ts, slot);
    if (tidx < 0 || !ts.ok[tidx])
      continue;
    int slot_end = ts.end[tidx];
    /* No non-tame deref through a different slot may reach into S's bytes. */
    if (dls_nontame_overlaps(&ts, has_unknown_deref, slot, slot_end - slot))
      continue;
    /* Whole-slot liveness: any later read in [slot, slot_end)? */
    int alive = 0;
    for (int k = 0; k < lv.n; k++)
      if ((dls_has_backedge || lv.r[k].pos > i) &&
          lv.r[k].off < slot_end && lv.r[k].off + lv.r[k].width > slot)
      {
        alive = 1;
        break;
      }
    if (alive)
      continue;
    LOG_IR_GEN("DEAD LOCAL SLOT: nop STORE_INDEXED via vreg at i=%d (slot=%d end=%d)", i, slot, slot_end);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  /* BLOCK_COPY dead-store elim: the copy into [base, base+size) is dead if nothing reads those bytes (no later StackLoc read, no overlapping non-tame deref). */
  if (!has_unknown_deref)
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_BLOCK_COPY)
      continue;
    /* A volatile access is observable even when nothing reads the slot back. */
    if (tcc_ir_instr_access_is_volatile(ir, q))
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_tag(dest) != IROP_TAG_STACKOFF || !dest.is_local || irop_get_vreg(dest) != -1)
      continue;
    if (tcc_ir_op_src2_tag(ir, q) != IROP_TAG_IMM32)
      continue;
    int base = irop_get_stack_offset(dest);
    int width = (int)tcc_ir_op_src2_imm(ir, q);
    if (width <= 0)
      continue;
    if (dls_nontame_overlaps(&ts, has_unknown_deref, base, width))
      continue;
    int alive = 0;
    for (int k = 0; k < lv.n; k++)
      if ((dls_has_backedge || lv.r[k].pos > i) &&
          base < lv.r[k].off + lv.r[k].width && base + width > lv.r[k].off)
      {
        alive = 1;
        break;
      }
    if (alive)
      continue;
    LOG_IR_GEN("DEAD LOCAL SLOT: nop BLOCK_COPY at i=%d (base=%d size=%d)", i, base, width);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  if (!any_nontame && writecall_count > 0)
  {
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL)
        continue;
      int cid = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
      if (cid < 0 || cid >= max_call_id)
        continue;
      if (!(is_writecall[cid / 8] & (1 << (cid % 8))))
        continue;
      int sz_pidx = (writecall_size_at_2[cid / 8] & (1 << (cid % 8))) ? 2 : 1;
      IROperand p0, p_sz;
      if (!ir_opt_get_call_param_operand(ir, i, 0, &p0))
        continue;
      if (!ir_opt_get_call_param_operand(ir, i, sz_pidx, &p_sz))
        continue;
      if (dls_writecall_is_volatile(ir, i, p0))
        continue;
      if (irop_get_tag(p0) != IROP_TAG_STACKOFF || !p0.is_local || p0.is_lval)
        continue;
      if (irop_get_vreg(p0) != -1)
        continue;
      if (irop_get_tag(p_sz) != IROP_TAG_IMM32)
        continue;
      int base = irop_get_stack_offset(p0);
      int sz = (int)irop_get_imm64_ex(ir, p_sz);
      if (sz <= 0)
        continue;
      /* Position-aware: only reads after this call keep it alive. */
      int alive = 0;
      for (int k = 0; k < lv.n; k++)
        if ((dls_has_backedge || lv.r[k].pos > i) &&
            base < lv.r[k].off + lv.r[k].width && base + sz > lv.r[k].off)
        {
          alive = 1;
          break;
        }
      if (alive)
        continue;
      LOG_IR_GEN("DEAD LOCAL SLOT: nop write-call at i=%d (base=%d size=%d)", i, base, sz);
      ir_opt_nop_call_params(ir, i);
      q->op = TCCIR_OP_NOP;
      changes++;
    }
  }

  /* vreg-PARAM0 write-call elim: a memset/memcpy/memmove writing through a vreg → tame slot S is dead iff S is tame and no lv.r[] entry reaches offset >= S.  Gated on any_nontame==0: StackLoc offsets aren't allocation boundaries, so a non-tame escape of an overlapping slot must block it. */
  if (!any_nontame && writecall_count > 0)
  {
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL)
        continue;
      int cid = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
      if (cid < 0 || cid >= max_call_id)
        continue;
      if (!(is_writecall[cid / 8] & (1 << (cid % 8))))
        continue;
      IROperand p0;
      if (!ir_opt_get_call_param_operand(ir, i, 0, &p0))
        continue;
      if (dls_writecall_is_volatile(ir, i, p0))
        continue;
      /* Skip the direct Addr[StackLoc[X]] case — handled above. */
      if (irop_get_tag(p0) == IROP_TAG_STACKOFF && !p0.is_lval && irop_get_vreg(p0) == -1)
        continue;
      /* Need a vreg PARAM0 with a known-single tame slot. */
      int vr = irop_get_vreg(p0);
      if (vr == -1)
        continue;
      int vf = dls_vr_flat(ir, max_vreg, vr);
      if (vf < 0)
        continue;
      int slot = vreg_slot[vf];
      if (slot == -1 || slot == VR_SLOT_AMBIG)
        continue;
      int tidx = dls_tame_find(&ts, slot);
      if (tidx < 0 || !ts.ok[tidx])
        continue;
      /* Whole-slot check: any live range whose end exceeds slot, read after this call?  (Wide reads may start below slot and extend across it.) */
      int alive = 0;
      for (int k = 0; k < lv.n; k++)
        if ((dls_has_backedge || lv.r[k].pos > i) && lv.r[k].off + lv.r[k].width > slot)
        {
          alive = 1;
          break;
        }
      if (alive)
        continue;
      LOG_IR_GEN("DEAD LOCAL SLOT: nop write-call (vreg PARAM0) at i=%d (slot=%d)", i, slot);
      ir_opt_nop_call_params(ir, i);
      q->op = TCCIR_OP_NOP;
      changes++;
    }
  }

  tcc_free(lv.r);
  tcc_free(slr);
  tcc_free(is_writecall);
  tcc_free(writecall_size_at_2);
  tcc_free(is_memcpy_like);
  tcc_free(vreg_slot);
  tcc_free(vreg_external);
  tcc_free(ts.off);
  tcc_free(ts.ok);
  tcc_free(ts.end);
  return changes;
#undef VR_SLOT_AMBIG
}

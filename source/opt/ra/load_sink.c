/*
 *  TCC IR - Sink a copied word's load to its store, before RA
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/*
 * A struct copied field by field after SRA reads every word into a TEMP first
 * and writes them out later: the Zig C backend's
 * `t31 = (*t30); ... (*t68) = init_at(t31, ...)` loads all eight words of a
 * 32-byte interface where the copy is taken and stores them after the other
 * fields are computed, so all eight are live at once -- across the u64 field
 * stores -- and the allocator spills two of them, or the values around them
 * (ROMFS FileHeader.load).  Nothing else moves a load down to its only use.
 *
 * A load sinks to just in front of the store of its value when:
 *   - it is a LOAD through a plain pointer TEMP or a LOAD_INDEXED by a
 *     constant, of a byte, halfword or word, into a TEMP named by nothing but
 *     the load and that STORE / STORE_INDEXED (the value it stores);
 *   - the window between them is one block (no jump target, branch, return,
 *     call or call setup, asm, VLA, setjmp, trap, volatile access, flag setter
 *     or reader -- a load's lowering may add an address with a narrow ADDS);
 *   - nothing in the window redefines the load's base, and every memory write
 *     in it is a store through a pointer with the same root as the load's (a
 *     single-definition TEMP or an unwritten PARAM, reached through copies and
 *     constant offsets) to bytes the load does not read, or -- when that root
 *     is a PARAM -- a direct frame-slot store: a pointer the function was
 *     handed cannot point into its own frame.  Any other frame or VAR write,
 *     or a store through an unrelated pointer, stops the window.
 * Then the load reads what it read before, and its value lives from the store's
 * slot only.  A base no later instruction names is left alone: it would live
 * longer by what the value lives shorter.
 *
 * Runs on the de-SSA'd IR next to ra:bump_sink; disableable via
 * TCC_DISABLE_PASS=ra:load_sink.
 */

#define USING_GLOBALS

#include "ir.h"
#include "regalloc.h"
#include "opt_range.h"
#include "opt_utils.h"
#include "memory/unique_ptr.h"

#define RA_LOAD_SINK_WINDOW 64
#define RA_LOAD_SINK_HOPS 8

typedef struct
{
  int32_t *def_at;   /* per TEMP position: its definition, -1 none, -2 several */
  uint16_t *refs;    /* per TEMP position: operands naming it (saturating) */
  uint8_t *pwritten; /* per PARAM position: some instruction defines it */
  int ntemp, nparam;
} RaLsInfo;

/* The vreg instruction q (re)defines, or -1.  STORE_INDEXED's dest is the base
 * it reads. */
static int32_t ra_ls_def(TCCIRState *ir, IRQuadCompact *q)
{
  if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest || q->op == TCCIR_OP_STORE_INDEXED)
    return -1;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  return irop_dest_defines_vreg(d) ? irop_get_vreg(d) : -1;
}

static void ra_ls_count_ref(RaLsInfo *li, IROperand o)
{
  if (irop_is_immediate(o) || !irop_has_vreg(o))
    return;
  int32_t v = irop_get_vreg(o);
  if (v < 0 || TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_TEMP)
    return;
  int p = TCCIR_DECODE_VREG_POSITION(v);
  if (p < li->ntemp && li->refs[p] < UINT16_MAX)
    li->refs[p]++;
}

static void ra_ls_scan(TCCIRState *ir, RaLsInfo *li)
{
  const int n = ir->next_instruction_index;
  li->ntemp = ir->next_temporary_variable + 1;
  li->nparam = ir->next_parameter + 1;
  li->def_at = tcc_malloc(sizeof(int32_t) * li->ntemp);
  li->refs = tcc_mallocz(sizeof(uint16_t) * li->ntemp);
  li->pwritten = tcc_mallocz(li->nparam);
  for (int p = 0; p < li->ntemp; p++)
    li->def_at[p] = -1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[q->op].has_dest)
      ra_ls_count_ref(li, tcc_ir_op_get_dest(ir, q));
    if (irop_config[q->op].has_src1)
      ra_ls_count_ref(li, tcc_ir_op_get_src1(ir, q));
    if (irop_config[q->op].has_src2)
      ra_ls_count_ref(li, tcc_ir_op_get_src2(ir, q));
    if (ir_op_has(q->op, IROP_A_SLOT3))
      ra_ls_count_ref(li, ir->iroperand_pool[q->operand_base + 3]);
    if (tcc_ir_op_is_mac(q->op))
      ra_ls_count_ref(li, tcc_ir_op_get_accum(ir, q));
    int32_t dv[2] = {ra_ls_def(ir, q), -1};
    if (ir_op_has(q->op, IR_HZ_UPDATES_SRC) && irop_config[q->op].has_src1)
      dv[1] = irop_get_vreg(tcc_ir_op_get_src1(ir, q)); /* the advanced pointer */
    for (int k = 0; k < 2; k++)
    {
      int32_t v = dv[k];
      if (v < 0)
        continue;
      int p = TCCIR_DECODE_VREG_POSITION(v);
      if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP && p < li->ntemp)
        li->def_at[p] = li->def_at[p] == -1 ? i : -2;
      else if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_PARAM && p < li->nparam)
        li->pwritten[p] = 1;
    }
  }
}

/* A plain register value operand: a vreg read, not memory through it. */
static int ra_ls_plain(IROperand o)
{
  return irop_get_tag(o) == IROP_TAG_VREG && irop_get_vreg(o) >= 0 && !o.is_lval && !o.is_local &&
         !o.is_llocal && !o.is_sym && !o.is_complex;
}

/* Pointer `v` as (root, offset): through copies and constant ADDs of
 * single-definition TEMPs.  The root must hold one value over the whole
 * window (lo, hi] -- a single-definition TEMP defined outside it, or a PARAM
 * no instruction writes and whose address is never taken -- or 0 is returned:
 * a TEMP defined inside the window (a loop-carried read before its def) names
 * two values there.  Instruction moves keep the answer. */
static int ra_ls_root(TCCIRState *ir, const RaLsInfo *li, int32_t v, int lo, int hi, int32_t *root, int32_t *off)
{
  int32_t acc = 0;
  for (int hop = 0; hop < RA_LOAD_SINK_HOPS; hop++)
  {
    int p = TCCIR_DECODE_VREG_POSITION(v);
    if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_PARAM)
    {
      if (p >= li->nparam || li->pwritten[p] ||
          ir_opt_vreg_address_taken_between(ir, v, -1, ir->next_instruction_index))
        return 0;
      *root = v, *off = acc;
      return 1;
    }
    if (TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_TEMP || p >= li->ntemp || li->def_at[p] < 0 ||
        (li->def_at[p] > lo && li->def_at[p] <= hi))
      return 0;
    IRQuadCompact *q = &ir->compact_instructions[li->def_at[p]];
    IROperand s1 = irop_config[q->op].has_src1 ? tcc_ir_op_get_src1(ir, q) : IROP_NONE;
    int follow = 0;
    if (q->op == TCCIR_OP_ASSIGN && ra_ls_plain(s1) && irop_get_btype(s1) == IROP_BTYPE_INT32)
      follow = 1;
    else if (q->op == TCCIR_OP_ADD && ra_ls_plain(s1) && irop_get_btype(s1) == IROP_BTYPE_INT32 &&
             tcc_ir_op_src2_is_imm(ir, q) && !tcc_ir_barrel_shift_at(ir, q))
    {
      IROperand c = tcc_ir_op_get_src2(ir, q);
      if (irop_get_tag(c) != IROP_TAG_IMM32 || c.is_lval)
        return 0;
      acc += c.u.imm32;
      follow = 1;
    }
    if (!follow)
    {
      *root = v, *off = acc;
      return 1;
    }
    v = irop_get_vreg(s1);
  }
  return 0;
}

static int ra_ls_bytes(int bt)
{
  switch (bt)
  {
  case IROP_BTYPE_INT8:
    return 1;
  case IROP_BTYPE_INT16:
    return 2;
  case IROP_BTYPE_INT32:
    return 4;
  case IROP_BTYPE_INT64:
    return 8;
  default:
    return 0;
  }
}

/* A load or store through a plain pointer TEMP, by a constant: its base vreg,
 * displacement and access operand (width, volatility), or 0. */
static int ra_ls_access(TCCIRState *ir, IRQuadCompact *q, int32_t *base, int32_t *disp, IROperand *acc)
{
  int is_store = q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED;
  int indexed = q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_LOAD_INDEXED;
  if (!is_store && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_LOAD_INDEXED)
    return 0;
  IROperand b = tcc_ir_op_get_dest_or_src1(ir, q, !is_store);
  if (irop_get_tag(b) != IROP_TAG_VREG || irop_get_vreg(b) < 0 || b.is_local || b.is_llocal || b.is_sym ||
      b.is_complex || b.is_lval == indexed)
    return 0;
  *base = irop_get_vreg(b);
  *disp = 0;
  *acc = b;
  if (indexed)
  {
    IROperand scale = tcc_ir_op_get_scale(ir, q);
    if (tcc_ir_op_src2_tag(ir, q) != IROP_TAG_IMM32 || tcc_ir_op_src2_is_lval(ir, q) ||
        (!irop_is_none(scale) && (irop_get_tag(scale) != IROP_TAG_IMM32 || irop_get_imm32(scale) != 0)))
      return 0;
    *disp = tcc_ir_op_src2_imm32(ir, q);
    *acc = tcc_ir_op_get_dest_or_src1(ir, q, is_store);
  }
  return ra_ls_bytes(irop_get_btype(*acc)) && !acc->is_complex;
}

/* Does q name vreg v in any operand (a read, or the base of a store)? */
static int ra_ls_names(TCCIRState *ir, IRQuadCompact *q, int32_t v)
{
  IROperand o[5];
  int no = 0;
  if (irop_config[q->op].has_dest)
    o[no++] = tcc_ir_op_get_dest(ir, q);
  if (irop_config[q->op].has_src1)
    o[no++] = tcc_ir_op_get_src1(ir, q);
  if (irop_config[q->op].has_src2)
    o[no++] = tcc_ir_op_get_src2(ir, q);
  if (ir_op_has(q->op, IROP_A_SLOT3))
    o[no++] = ir->iroperand_pool[q->operand_base + 3];
  if (tcc_ir_op_is_mac(q->op))
    o[no++] = tcc_ir_op_get_accum(ir, q);
  for (int m = 0; m < no; m++)
    if (!irop_is_immediate(o[m]) && irop_has_vreg(o[m]) && irop_get_vreg(o[m]) == v)
      return 1;
  return 0;
}

/* A frame slot written directly (`StackLoc[-n] <-- v`), not through a
 * pointer the slot holds. */
static int ra_ls_frame_slot(IROperand d)
{
  return irop_get_tag(d) == IROP_TAG_STACKOFF && d.is_local && !d.is_llocal && !d.is_sym && !d.is_complex;
}

static int ra_ls_temp_pos(int32_t v, const RaLsInfo *li)
{
  if (v < 0 || TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  int p = TCCIR_DECODE_VREG_POSITION(v);
  return p < li->ntemp ? p : -1;
}

int ra_load_sink(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  const uint32_t stop_hz = IR_HZ_BRANCH | IR_HZ_RETURN | IR_HZ_CALL | IR_HZ_CALL_PARAM | IR_HZ_CALL_SEQ |
                           IR_HZ_UPDATES_SRC | IR_HZ_ASM | IR_HZ_VLA | IR_HZ_NONLOCAL | IR_HZ_CHAIN | IR_HZ_TRAP |
                           IR_HZ_FLAGS_SET | IR_HZ_FLAGS_READ | IR_HZ_JOIN | IR_HZ_VOLATILE;
  const uint32_t write_hz = IR_HZ_MEM_WRITE | IR_HZ_DEST_LVAL | IR_HZ_DEST_STACKOFF;
  RaLsInfo li = {0};
  int moved = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_LOAD_INDEXED)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (!ra_ls_plain(d) || irop_get_btype(d) != IROP_BTYPE_INT32 || tcc_ir_instr_access_is_volatile(ir, q))
      continue;
    int32_t lbase, ldisp;
    IROperand lacc;
    if (!ra_ls_access(ir, q, &lbase, &ldisp, &lacc) || ra_ls_bytes(irop_get_btype(lacc)) > 4)
      continue;
    if (!li.def_at)
      ra_ls_scan(ir, &li);
    int32_t dv = irop_get_vreg(d);
    int dp = ra_ls_temp_pos(dv, &li);
    if (dp < 0 || li.def_at[dp] != i || li.refs[dp] != 2 || dv == lbase)
      continue;

    /* The window: up to the store of the value, or the first hazard. */
    int use = -1, seen = 0;
    int32_t lroot = -1, loff = 0;
    for (int k = i + 1; k < n; k++)
    {
      IRQuadCompact *kq = &ir->compact_instructions[k];
      if (ir_q_hazards(ir, kq, stop_hz))
        break;
      if (kq->op == TCCIR_OP_NOP)
        continue;
      if (++seen > RA_LOAD_SINK_WINDOW)
        break;
      if ((kq->op == TCCIR_OP_STORE || kq->op == TCCIR_OP_STORE_INDEXED) &&
          irop_get_vreg(tcc_ir_op_get_src1(ir, kq)) == dv)
      {
        IROperand v = tcc_ir_op_get_src1(ir, kq);
        if (ra_ls_plain(v))
          use = k;
        break;
      }
      if (ra_ls_def(ir, kq) == lbase)
        break; /* the base is redefined: the load must read it first */
      if (!ir_q_hazards(ir, kq, write_hz))
        continue;
      if (kq->op == TCCIR_OP_STORE && ra_ls_frame_slot(tcc_ir_op_get_dest(ir, kq)))
      {
        /* A pointer this function was handed cannot point into its own
         * frame, which did not exist when the pointer was made. */
        if (!ra_ls_root(ir, &li, lbase, i, k, &lroot, &loff) || TCCIR_DECODE_VREG_TYPE(lroot) != TCCIR_VREG_TYPE_PARAM)
          break;
        continue;
      }
      int32_t sbase, sdisp, sroot, soff;
      IROperand sacc;
      if (!ra_ls_access(ir, kq, &sbase, &sdisp, &sacc) ||
          (kq->op != TCCIR_OP_STORE && kq->op != TCCIR_OP_STORE_INDEXED))
        break;
      if (!ra_ls_root(ir, &li, lbase, i, k, &lroot, &loff) || !ra_ls_root(ir, &li, sbase, i, k, &sroot, &soff) ||
          sroot != lroot)
        break;
      int64_t llo = (int64_t)loff + ldisp, lhi = llo + ra_ls_bytes(irop_get_btype(lacc));
      int64_t slo = (int64_t)soff + sdisp, shi = slo + ra_ls_bytes(irop_get_btype(sacc));
      if (slo < lhi && llo < shi)
        break; /* writes what the load reads */
    }
    if (use < 0)
      continue;
    int to = use - 1;
    while (to > i && ir->compact_instructions[to].op == TCCIR_OP_NOP)
      to--;
    if (to <= i)
      continue; /* already in front of its store */
    to = use - 1;
    /* A base nothing names after the load dies there: sinking would only
     * trade the loaded value's range for the base's (an address computed for
     * this load alone).  One the copy's other words still read stays live. */
    int base_live = 0;
    for (int k = i + 1; k < n && !base_live; k++)
      base_live = ir->compact_instructions[k].op != TCCIR_OP_NOP && ra_ls_names(ir, &ir->compact_instructions[k], lbase);
    if (!base_live)
      continue;

    IRQuadCompact ld = *q;
    for (int k = i; k < to; k++)
    {
      IRQuadCompact *dst = &ir->compact_instructions[k];
      IRQuadCompact nxt = ir->compact_instructions[k + 1];
      /* jump-target and alignment marks belong to the position */
      nxt.is_jump_target = dst->is_jump_target;
      nxt.align_target = dst->align_target;
      *dst = nxt;
    }
    {
      IRQuadCompact *dst = &ir->compact_instructions[to];
      ld.is_jump_target = dst->is_jump_target;
      ld.align_target = dst->align_target;
      *dst = ld;
    }
    /* Definitions in the window moved up by one. */
    for (int p = 0; p < li.ntemp; p++)
      if (li.def_at[p] > i && li.def_at[p] <= to)
        li.def_at[p]--;
    li.def_at[dp] = to;
    moved++;
    i--; /* slot i holds the window's first instruction now */
  }
  tcc_free(li.def_at);
  tcc_free(li.refs);
  tcc_free(li.pwritten);
  return moved;
}

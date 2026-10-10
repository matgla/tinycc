/*
 *  TCC IR - Loop-Invariant Code Motion (LICM) Optimization
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "licm.h"
#include "opt.h"
#include "opt_utils.h"
#include "cfg.h"
#include "core.h"
#include "pool.h"
#include "vreg.h"
#include "arm_regalloc.h"
#include "memory/vector.h"
#include <string.h>

typedef struct {
  int first, last, words;
} LoopParamSpan;
TCC_VECTOR_DEFINE(LoopParamSpans, LoopParamSpan)
TCC_VECTOR_DEFINE(LoopParamCounts, int)

int tcc_ir_loop_call_promotion_budget(TCCIRState *ir, IRLoops *loops)
{
  int n = ir->next_instruction_index;
  int base[4] = {0, 0, ir->next_local_variable,
                 ir->next_local_variable + ir->next_temporary_variable};
  int limit[4] = {0, ir->next_local_variable, ir->next_temporary_variable, ir->next_parameter};
  int count = base[3] + limit[3];
  LoopParamSpans spans;
  LoopParamCounts counts;
  LoopParamSpans_init(&spans);
  LoopParamCounts_init(&counts);
  LoopParamSpans_resize(&spans, count);
  LoopParamCounts_resize(&counts, n + 1);
  memset(counts.data, 0, (n + 1) * sizeof(*counts.data));
  for (int k = 0; k < count; ++k)
    spans.data[k] = (LoopParamSpan){n, -1, 1};
  for (int i = 0; i < n; ++i) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int nops = irop_config[q->op].has_dest + irop_config[q->op].has_src1 + irop_config[q->op].has_src2;
    if (ir_op_has(q->op, IROP_A_SLOT3))
      ++nops;
    for (int s = 0; s < nops; ++s) {
      IROperand op = ir->iroperand_pool[q->operand_base + s];
      int32_t vr = irop_get_vreg(op);
      if (vr < 0 || irop_is_immediate(op))
        continue;
      int type = TCCIR_DECODE_VREG_TYPE(vr), pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (type < 1 || type > 3 || pos >= limit[type])
        continue;
      IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, vr);
      if (!li || li->addrtaken || li->is_volatile || li->is_struct)
        continue;
      LoopParamSpan *span = &spans.data[base[type] + pos];
      int first = type == TCCIR_VREG_TYPE_PARAM ? -1 : i;
      if (first < span->first)
        span->first = first;
      span->last = i;
      span->words = li->is_complex ? 4 : (li->is_double || li->is_llong ? 2 : 1);
    }
  }
  for (int k = 0; k < count; ++k) {
    LoopParamSpan *span = &spans.data[k];
    // Extend entry values through the backedge, including calls after their last textual use.
    int changed;
    do {
      changed = 0;
      for (int l = 0; l < loops->num_loops; ++l) {
        IRLoop *loop = &loops->loops[l];
        if (span->first < loop->start_idx && span->last >= loop->start_idx && span->last < loop->end_idx) {
          span->last = loop->end_idx;
          changed = 1;
        }
      }
    } while (changed);
    if (span->first + 1 < span->last) {
      counts.data[span->first + 1] += span->words;
      counts.data[span->last] -= span->words;
    }
  }
  int live = 0, peak = 0;
  for (int i = 0; i < n; ++i) {
    live += counts.data[i];
    if (ir_op_has(ir->compact_instructions[i].op, IR_HZ_CALL) && live > peak)
      peak = live;
  }
  LoopParamSpans_cleanup(&spans);
  LoopParamCounts_cleanup(&counts);
  int available = tcc_ir_callee_saved_capacity(ir, arm_get_regalloc_target());
  return available - peak;
}

/*
 * Create an ASSIGN instruction to copy a value
 * This properly allocates space in the operand pool
 */
static IRQuadCompact create_assign_instr(TCCIRState *ir, int32_t dest_vreg, IROperand src, int dest_btype)
{
  IRQuadCompact q = {0};
  q.op = TCCIR_OP_ASSIGN;

  /* ASSIGN has dest (slot 0) and src1 (slot 1) */
  /* Allocate operand pool space for both operands */
  IROperand dest_op = irop_make_vreg(dest_vreg, dest_btype);

  /* Add operands to pool and set operand_base */
  q.operand_base = tcc_ir_pool_add(ir, dest_op); /* dest at base + 0 */
  tcc_ir_pool_add(ir, src);                      /* src1 at base + 1 */

  return q;
}

// Cache stack parameters in entry temps after the last pass that could forward them back.
// `*rep_bt` carries the width the cacheable uses read the param at: every use
// must agree (an entry copy at u8 cannot serve a use that reads u32 — the
// caller's slot holds one meaningful byte), and INT64/float uses stay
// uncacheable.  Narrow widths are the mem.findScalarPos needle: a u8 5th
// argument whose home was re-read from the incoming argument area every loop
// byte (docs/bugs/kernel-ls-bin-scan-family-gap.md).
static int param_seen_at(TCCIRState *ir, int32_t penc, int i, int *is_dest,
                         int *is_deref, IROperand *rep, int *rep_bt)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  int seen = 0;
  if (irop_config[q->op].has_dest &&
      tcc_ir_op_dest_vreg(ir, q) == penc)
    *is_dest = 1;
  for (int slot = 0; slot < 3; slot++) {
    IROperand s;
    if (slot == 0) { if (!irop_config[q->op].has_src1) continue; s = tcc_ir_op_get_src1(ir, q); }
    else if (slot == 1) { if (!irop_config[q->op].has_src2) continue; s = tcc_ir_op_get_src2(ir, q); }
    else { if (!ir_op_has(q->op, IROP_A_SLOT3)) continue; s = ir->iroperand_pool[q->operand_base + 3]; }
    if (irop_get_vreg(s) != penc)
      continue;
    seen = 1;
    int bt = irop_get_btype(s);
    /* is_lval|is_local on a stack param = load its value from home (cacheable); only is_llocal derefs memory that may change. */
    if (s.is_llocal || tcc_ir_access_is_volatile(ir, s) ||
        (bt != IROP_BTYPE_INT32 && bt != IROP_BTYPE_INT16 && bt != IROP_BTYPE_INT8))
      *is_deref = 1;
    else if (*rep_bt >= 0 && *rep_bt != bt)
      *is_deref = 1; /* mixed widths: one entry copy cannot serve both */
    else {
      *rep = s;
      *rep_bt = bt;
    }
  }
  return seen;
}

int tcc_ir_promote_loop_stack_params(TCCIRState *ir)
{
  if (!ir || TCC_OPT(tcc_state, optimize) <= 0)
    return 0;
  const int param_count = ir->next_parameter;
  if (param_count <= 0)
    return 0;

  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0; /* computed goto: insertion index shifts are unsafe */

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0) {
    tcc_ir_free_loops(loops);
    return 0;
  }

  /* AAPCS: params beyond r0-r3 arrive on the stack; restrict to scalar 32-bit, non-address-taken. */
  uint8_t *stack_passed = tcc_mallocz((size_t)param_count);
  int argno = 0;
  for (int p = 0; p < param_count; p++) {
    IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, p));
    int is64 = li && (li->is_double || li->is_llong);
    if (is64 && (argno & 1))
      argno++;
    int in_regs = is64 ? (argno <= 2) : (argno <= 3);
    if (li && !in_regs && !is64 && !li->addrtaken && !li->is_volatile && !li->is_struct)
      stack_passed[p] = 1;
    argno += is64 ? 2 : 1;
  }

  // Parameters already count in the call-pressure estimate; replacing one adds no live value.
  int first_call = n;
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID ||
        op == TCCIR_OP_BUILTIN_APPLY) { first_call = i; break; }
  }
  int call_budget = first_call < n ? tcc_ir_loop_call_promotion_budget(ir, loops) : -1;

  /* Phase 1: decide which params to promote (loop info still valid — no mutation). */
  int32_t *promote_penc = tcc_mallocz(sizeof(int32_t) * param_count);
  IROperand *promote_rep = tcc_mallocz(sizeof(IROperand) * param_count);
  int *promote_bt = tcc_mallocz(sizeof(int) * param_count);
  int promote_count = 0;
  for (int p = 0; p < param_count; p++) {
    if (!stack_passed[p])
      continue;
    int32_t penc = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, p);
    int has_use = 0, is_dest = 0, is_deref = 0, used_in_loop = 0, span_end = -1, rep_bt = -1;
    IROperand rep = {0};
    for (int i = 0; i < n; i++) {
      if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
        continue;
      if (!param_seen_at(ir, penc, i, &is_dest, &is_deref, &rep, &rep_bt))
        continue;
      has_use = 1;
      if (i > span_end)
        span_end = i;
      /* Loop-carried: the held register lives to the back-edge, so extend the span to each containing loop's end (catches a call nested after the use). */
      for (int L = 0; L < loops->num_loops; L++)
        if (tcc_ir_is_in_loop(&loops->loops[L], i)) {
          used_in_loop = 1;
          if (loops->loops[L].end_idx > span_end)
            span_end = loops->loops[L].end_idx;
        }
    }
    if (!has_use || is_dest || is_deref || !used_in_loop || (span_end >= first_call && call_budget < 0))
      continue;
    promote_penc[promote_count] = penc;
    promote_rep[promote_count] = rep;
    promote_bt[promote_count] = rep_bt;
    promote_count++;
  }
  tcc_ir_free_loops(loops);
  tcc_free(stack_passed);

  /* Phase 2: rewrite by-value uses to a fresh temp, then insert the entry copy (which renumbers via tcc_ir_insert_instruction_before), rescanning n each time. */
  int promoted = 0;
  for (int k = 0; k < promote_count; k++) {
    int32_t penc = promote_penc[k];
    int32_t tx = tcc_ir_get_vreg_temp(ir);
    n = ir->next_instruction_index;
    for (int i = 0; i < n; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_src1) {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s) == penc) {
          IROperand nv = irop_make_vreg(tx, irop_get_btype(s));
          nv.is_unsigned = s.is_unsigned;
          tcc_ir_op_set_src1(ir, q, nv);
        }
      }
      if (irop_config[q->op].has_src2) {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        if (irop_get_vreg(s) == penc) {
          IROperand nv = irop_make_vreg(tx, irop_get_btype(s));
          nv.is_unsigned = s.is_unsigned;
          tcc_ir_op_set_src2(ir, q, nv);
        }
      }
      if (ir_op_has(q->op, IROP_A_SLOT3)) {
        IROperand s = ir->iroperand_pool[q->operand_base + 3];
        if (irop_get_vreg(s) == penc) {
          IROperand nv = irop_make_vreg(tx, irop_get_btype(s));
          nv.is_unsigned = s.is_unsigned;
          ir->iroperand_pool[q->operand_base + 3] = nv;
        }
      }
    }
    IRQuadCompact assign = create_assign_instr(ir, tx, promote_rep[k], promote_bt[k]);
    tcc_ir_insert_instruction_before(ir, 0, &assign);
    promoted++;
  }

  tcc_free(promote_penc);
  tcc_free(promote_rep);
  tcc_free(promote_bt);
  return promoted;
}

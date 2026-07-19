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
#include <string.h>

/*
 * Create an ASSIGN instruction to copy a value
 * This properly allocates space in the operand pool
 */
static IRQuadCompact create_assign_instr(TCCIRState *ir, int32_t dest_vreg, IROperand src)
{
  IRQuadCompact q = {0};
  q.op = TCCIR_OP_ASSIGN;

  /* ASSIGN has dest (slot 0) and src1 (slot 1) */
  /* Allocate operand pool space for both operands */
  IROperand dest_op = irop_make_vreg(dest_vreg, IROP_BTYPE_INT32);

  /* Add operands to pool and set operand_base */
  q.operand_base = tcc_ir_pool_add(ir, dest_op); /* dest at base + 0 */
  tcc_ir_pool_add(ir, src);                      /* src1 at base + 1 */

  return q;
}

/* ra:stack_param_promote — cache a loop-invariant stack param in an entry temp so RA holds it in a register instead of reloading it per iteration (docs/plan_stack_param_reg_promotion.md). */
static int param_seen_at(TCCIRState *ir, int32_t penc, int i, int *is_dest,
                         int *is_deref, IROperand *rep)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  int seen = 0;
  if (irop_config[q->op].has_dest &&
      irop_get_vreg(tcc_ir_op_get_dest(ir, q)) == penc)
    *is_dest = 1;
  for (int slot = 0; slot < 3; slot++) {
    IROperand s;
    if (slot == 0) { if (!irop_config[q->op].has_src1) continue; s = tcc_ir_op_get_src1(ir, q); }
    else if (slot == 1) { if (!irop_config[q->op].has_src2) continue; s = tcc_ir_op_get_src2(ir, q); }
    else { if (q->op != TCCIR_OP_MLA) continue; s = tcc_ir_op_get_accum(ir, q); }
    if (irop_get_vreg(s) != penc)
      continue;
    seen = 1;
    /* is_lval|is_local on a stack param = load its value from home (cacheable); only is_llocal derefs memory that may change. INT32 so the INT32 entry copy is full-width. */
    if (s.is_llocal || irop_get_btype(s) != IROP_BTYPE_INT32)
      *is_deref = 1;
    else
      *rep = s;
  }
  return seen;
}

int tcc_ir_promote_loop_stack_params(TCCIRState *ir)
{
  if (!ir || tcc_state->optimize <= 0)
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
    if (li && !in_regs && !is64 && !li->addrtaken)
      stack_passed[p] = 1;
    argno += is64 ? 2 : 1;
  }

  /* A promoted value held across a call needs a callee-saved reg; several in a call-heavy function flood the set and spill (docs/plan_stack_param_reg_promotion.md). Only promote params whose span ends before the first call. */
  int first_call = n;
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID ||
        op == TCCIR_OP_BUILTIN_APPLY) { first_call = i; break; }
  }

  /* Phase 1: decide which params to promote (loop info still valid — no mutation). */
  int32_t *promote_penc = tcc_mallocz(sizeof(int32_t) * param_count);
  IROperand *promote_rep = tcc_mallocz(sizeof(IROperand) * param_count);
  int promote_count = 0;
  for (int p = 0; p < param_count; p++) {
    if (!stack_passed[p])
      continue;
    int32_t penc = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, p);
    int has_use = 0, is_dest = 0, is_deref = 0, used_in_loop = 0, span_end = -1;
    IROperand rep = {0};
    for (int i = 0; i < n; i++) {
      if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
        continue;
      if (!param_seen_at(ir, penc, i, &is_dest, &is_deref, &rep))
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
    if (!has_use || is_dest || is_deref || !used_in_loop || span_end >= first_call)
      continue;
    promote_penc[promote_count] = penc;
    promote_rep[promote_count] = rep;
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
      if (q->op == TCCIR_OP_MLA) {
        IROperand s = tcc_ir_op_get_accum(ir, q);
        if (irop_get_vreg(s) == penc) {
          IROperand nv = irop_make_vreg(tx, irop_get_btype(s));
          nv.is_unsigned = s.is_unsigned;
          tcc_ir_op_set_accum(ir, q, nv);
        }
      }
    }
    IRQuadCompact assign = create_assign_instr(ir, tx, promote_rep[k]);
    tcc_ir_insert_instruction_before(ir, 0, &assign);
    promoted++;
  }

  tcc_free(promote_penc);
  tcc_free(promote_rep);
  return promoted;
}


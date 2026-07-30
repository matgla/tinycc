/*
 *  TCC IR - SSA DCE: return-path frame-store elimination
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "dce_passes.h"
#include <limits.h>


/* VAR/PARAM vregs share storage with their home slot, so a store through a taken
 * address redefines what the vreg read observes (pr85095). */
static int dce_ret_op_is_pure_read(TCCIRState *ir, IROperand op)
{
  (void)ir;
  if (op.is_lval)
    return 0;
  if (op.tag != IROP_TAG_VREG)
    return 1;
  int32_t vr = irop_get_vreg(op);
  return vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP;
}

/* The frame dies at return, so even an escaped address cannot legally read it. */
int dce_ret_path_frame_store(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  int changes = 0;

  /* A nested function's frame store may resolve to the parent's live frame (pr22061-3). */
  if (ir->has_static_chain)
    return 0;
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
  }

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!dest.is_lval || dest.is_llocal)
      continue;
    int frame_slot = 0;
    if (dest.is_local && irop_get_vreg(dest) < 0) {
      frame_slot = 1;
    } else if (!dest.is_local && !dest.is_sym && dest.tag == IROP_TAG_VREG) {
      int32_t pvr = irop_get_vreg(dest);
      if (pvr >= 0 && TCCIR_DECODE_VREG_TYPE(pvr) == TCCIR_VREG_TYPE_TEMP &&
          ssa_opt_resolve_lea_stackloc(ctx, pvr) != INT_MIN)
        frame_slot = 1;
    }
    if (!frame_slot)
      continue;

    int dead = 0;
    for (int j = i + 1; j < n; j++) {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_NOP)
        continue;
      if (jq->op == TCCIR_OP_RETURNVOID) {
        dead = 1;
        break;
      }
      if (jq->op == TCCIR_OP_RETURNVALUE) {
        dead = dce_ret_op_is_pure_read(ir, tcc_ir_op_get_src1(ir, jq));
        break;
      }
      if (ssa_opt_has_side_effects(jq->op) || jq->op == TCCIR_OP_LOAD ||
          jq->op == TCCIR_OP_LOAD_INDEXED || jq->op == TCCIR_OP_LOAD_POSTINC ||
          jq->op == TCCIR_OP_SELECT)
        break;
      if ((irop_config[jq->op].has_src1 && !dce_ret_op_is_pure_read(ir, tcc_ir_op_get_src1(ir, jq))) ||
          (irop_config[jq->op].has_src2 && !dce_ret_op_is_pure_read(ir, tcc_ir_op_get_src2(ir, jq))))
        break;
      if (jq->op == TCCIR_OP_MLA && !dce_ret_op_is_pure_read(ir, tcc_ir_op_get_accum(ir, jq)))
        break;
    }
    if (dead) {
      ssa_opt_nop_instr(ctx, i);
      changes++;
    }
  }
  return changes;
}

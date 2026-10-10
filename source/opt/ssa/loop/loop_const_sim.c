/*
 *  TCC IR - SSA loop: constant simulation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_loop_const_sim.h"
#include "loop_cand.h"
#include "opt_utils.h"

#define SSA_LOOP_CONST_SIM_MAX_PASSES 4
#define SSA_LCS_MAX_SPAN 256

/* `V` (VAR vreg, is_local + is_lval) is a register-resident scalar, not memory:
 * lcs_read_operand models it as a plain slot, so a LOAD or STORE naming one is
 * an ordinary copy.  Every other lval — stack slot, symbol, pointer deref — is
 * real memory the simulator would have to model, and `is_local && !is_lval` is
 * the address-of form, which hands out an address it could then be stored
 * through.  Both keep the region out. */
static int lcs_operand_is_memory(IROperand op)
{
  if (op.is_sym || op.is_llocal)
    return op.is_lval || op.is_local;
  int32_t vr = irop_get_vreg(op);
  int is_var = (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR);
  if (op.is_lval)
    return !(op.is_local && is_var);
  return op.is_local;
}

/* A volatile VAR is not a register-resident scalar: every read and write of it
 * is a mandated access -- `for (volatile int i = 0; i < 1000; i++);` is a
 * delay loop precisely because simulating it away is not allowed. */
static int lcs_operand_is_volatile(TCCIRState *ir, IROperand op)
{
  return tcc_ir_operand_names_volatile_var(ir, op);
}

static int lcs_span_has_memory(TCCIRState *ir, int start_idx, int end_idx)
{
  for (int i = start_idx; i <= end_idx; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (tcc_ir_instr_access_is_volatile(ir, q))
      return 1;
    if ((irop_config[q->op].has_src1 && lcs_operand_is_volatile(ir, tcc_ir_op_get_src1(ir, q))) ||
        (irop_config[q->op].has_src2 && lcs_operand_is_volatile(ir, tcc_ir_op_get_src2(ir, q))) ||
        (irop_config[q->op].has_dest && lcs_operand_is_volatile(ir, tcc_ir_op_get_dest(ir, q))))
      return 1;
    if (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC ||
        q->op == TCCIR_OP_BLOCK_COPY)
      return 1;
    if (irop_config[q->op].has_src1 && lcs_operand_is_memory(tcc_ir_op_get_src1(ir, q)))
      return 1;
    if (irop_config[q->op].has_src2 && lcs_operand_is_memory(tcc_ir_op_get_src2(ir, q)))
      return 1;
    if (irop_config[q->op].has_dest && lcs_operand_is_memory(tcc_ir_op_get_dest(ir, q)))
      return 1;
    if (q->op == TCCIR_OP_MLA && lcs_operand_is_memory(tcc_ir_op_get_accum(ir, q)))
      return 1;
  }
  return 0;
}

static int lcs_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
                             uint8_t *member, uint8_t *scratch)
{
  LcsSpan sp;
  if (!lcs_cand_span(ir, cfg, header_b, member, scratch, SSA_LCS_MAX_SPAN, &sp))
    return 0;
  if (lcs_span_has_memory(ir, sp.start, sp.end))
    return 0;
  return lcs_fold_region(ir, sp.start, sp.end, sp.header, sp.preheader,
                         /*allow_extension*/ 0);
}

int ssa_opt_loop_const_sim(TCCIRState *ir)
{
  return lcs_run_outermost(ir, SSA_LOOP_CONST_SIM_MAX_PASSES, 0, lcs_try_candidate);
}

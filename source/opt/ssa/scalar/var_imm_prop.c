/*
 *  TCC IR - SSA VAR Immediate Propagation
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "opt_dsl_var_const.h"
#include "opt/ssa/var_imm_prop.h"
#include "opt/ssa/ssa_opt_helpers.h"

/* ssa:var_imm_prop — an unpromoted non-addrtaken VAR whose lone def is
 * `V <- #imm32 [ASSIGN]` is an SSA value in all but name; forward the
 * immediate into dominated plain-value uses (legacy const_prop VAR analog).
 * The single-def / escape / dominance analysis is the reusable VAR_IMM
 * primitive (source/opt/framework/opt_dsl_var_const.h); this pass adds the
 * consumer-opcode vetoes and the operand rewrite. */

static int var_imm_prop_slot(IRSSAOptCtx *ctx, const OptDslVarImmState *st,
                             int i, int slot)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  VAR_IMM(st, slot);
  if (q->op == TCCIR_OP_CMP && slot == 0 &&
      !ssa_cprop_imm_other_operand_const(ir, q, viv, 1))
    return 0;
  if ((q->op == TCCIR_OP_BOOL_AND || q->op == TCCIR_OP_BOOL_OR) &&
      !ssa_cprop_imm_other_operand_const(ir, q, viv, slot + 1))
    return 0;
  IROperand imm = ssa_cprop_imm_for_use(videf, viuse);
  if (slot == 0)
    tcc_ir_op_set_src1(ir, q, imm);
  else if (slot == 1)
    tcc_ir_op_set_src2(ir, q, imm);
  else
    tcc_ir_op_set_accum(ir, q, imm);
  if (q->op == TCCIR_OP_LOAD) {
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_op_set_src2(ir, q, IROP_NONE);
  }
  return 1;
}

int ssa_opt_var_imm_prop(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  if (!cfg || !cfg->instr_to_block)
    return 0;

  OptDslVarImmState st;
  if (!opt_dsl_var_imm_state_build(ctx, &st))
    return 0;

  int n = ir->next_instruction_index;
  int changes = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (tcc_ir_barrel_shift_at(ir, q))
      continue;
    int rewrote = 0;
    if (irop_config[q->op].has_src1)
      rewrote |= var_imm_prop_slot(ctx, &st, i, 0);
    if (irop_config[q->op].has_src2)
      rewrote |= var_imm_prop_slot(ctx, &st, i, 1);
    if (q->op == TCCIR_OP_MLA)
      rewrote |= var_imm_prop_slot(ctx, &st, i, 2);
    changes += rewrote;
  }

  opt_dsl_var_imm_state_free(&st);
  return changes;
}

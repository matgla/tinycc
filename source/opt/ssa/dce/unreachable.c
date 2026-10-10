/*
 *  TCC IR - SSA DCE: unreachable instructions
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
#include "opt_utils.h"


int dce_unreachable(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* IJUMP targets are runtime-computed; reachability can't be proven. */
  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;
  /* Likewise `asm goto`: its labels are edges the IR does not record. */
  if (ir->func_has_asm_goto)
    return 0;

  uint8_t *reach = tcc_mallocz((n + 7) / 8);
  int *wl = tcc_malloc(n * sizeof(int));
  IrReachWorklist rw = {reach, wl, 0, 0, n};

  ir_opt_reach_mark(&rw, 0);
  while (rw.head < rw.tail) {
    int i = wl[rw.head++];
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op) {
    case TCCIR_OP_JUMP:
      ir_opt_reach_mark(&rw, (int)tcc_ir_op_dest_u_imm32(ir, q));
      break;
    case TCCIR_OP_JUMPIF:
      ir_opt_reach_mark(&rw, (int)tcc_ir_op_dest_u_imm32(ir, q));
      ir_opt_reach_mark(&rw, i + 1);
      break;
    case TCCIR_OP_SWITCH_TABLE: {
      int tid = (int)tcc_ir_op_src2_imm(ir, q);
      if (tid >= 0 && tid < ir->num_switch_tables) {
        TCCIRSwitchTable *t = &ir->switch_tables[tid];
        for (int j = 0; j < t->num_entries; j++)
          ir_opt_reach_mark(&rw, t->targets[j]);
        ir_opt_reach_mark(&rw, t->default_target);
      }
      break;
    }
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      break;
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID: {
      Sym *callee = tcc_ir_op_src1_sym(ir, q);
      if (!tcc_ir_callee_is_noreturn(callee))
        ir_opt_reach_mark(&rw, i + 1);
      break;
    }
    default:
      ir_opt_reach_mark(&rw, i + 1);
      break;
    }
  }

  int changes = 0;
  for (int i = 0; i < n; i++) {
    if (reach[i / 8] & (1 << (i % 8)))
      continue;
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    ssa_opt_nop_instr(ctx, i);
    changes++;
  }
  tcc_free(reach);
  tcc_free(wl);
  return changes;
}

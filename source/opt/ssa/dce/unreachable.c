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

  uint8_t *reach = tcc_mallocz((n + 7) / 8);
  int *wl = tcc_malloc(n * sizeof(int));
  int head = 0, tail = 0;
#define DCE_MARK(idx)                                                                                                   \
  do {                                                                                                                  \
    int _i = (idx);                                                                                                     \
    if (_i >= 0 && _i < n && !(reach[_i / 8] & (1 << (_i % 8)))) {                                                      \
      reach[_i / 8] |= (1 << (_i % 8));                                                                                 \
      wl[tail++] = _i;                                                                                                  \
    }                                                                                                                   \
  } while (0)

  DCE_MARK(0);
  while (head < tail) {
    int i = wl[head++];
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op) {
    case TCCIR_OP_JUMP:
      DCE_MARK((int)tcc_ir_op_get_dest(ir, q).u.imm32);
      break;
    case TCCIR_OP_JUMPIF:
      DCE_MARK((int)tcc_ir_op_get_dest(ir, q).u.imm32);
      DCE_MARK(i + 1);
      break;
    case TCCIR_OP_SWITCH_TABLE: {
      int tid = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
      if (tid >= 0 && tid < ir->num_switch_tables) {
        TCCIRSwitchTable *t = &ir->switch_tables[tid];
        for (int j = 0; j < t->num_entries; j++)
          DCE_MARK(t->targets[j]);
        DCE_MARK(t->default_target);
      }
      break;
    }
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      break;
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID: {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (!tcc_ir_callee_is_noreturn(callee))
        DCE_MARK(i + 1);
      break;
    }
    default:
      DCE_MARK(i + 1);
      break;
    }
  }
#undef DCE_MARK

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

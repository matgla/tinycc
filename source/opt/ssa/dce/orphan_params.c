/*
 *  TCC IR - SSA DCE: orphaned call params
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


int dce_orphan_params(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  int max_cid = 0, saw_param = 0;
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op != TCCIR_OP_FUNCPARAMVAL && op != TCCIR_OP_FUNCPARAMVOID &&
        op != TCCIR_OP_FUNCCALLVAL && op != TCCIR_OP_FUNCCALLVOID)
      continue;
    if (op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_FUNCPARAMVOID)
      saw_param = 1;
    int cid = TCCIR_DECODE_CALL_ID((int32_t)irop_get_imm64_ex(
        ir, tcc_ir_op_get_src2(ir, &ir->compact_instructions[i])));
    if (cid > max_cid)
      max_cid = cid;
  }
  if (!saw_param || max_cid <= 0)
    return 0;

  int nbytes = (max_cid / 8) + 1;
  uint8_t *has_call = tcc_mallocz(nbytes);
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op != TCCIR_OP_FUNCCALLVAL && op != TCCIR_OP_FUNCCALLVOID)
      continue;
    int cid = TCCIR_DECODE_CALL_ID((int32_t)irop_get_imm64_ex(
        ir, tcc_ir_op_get_src2(ir, &ir->compact_instructions[i])));
    if (cid >= 0 && cid <= max_cid)
      has_call[cid / 8] |= (uint8_t)(1 << (cid % 8));
  }

  int changes = 0;
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op != TCCIR_OP_FUNCPARAMVAL && op != TCCIR_OP_FUNCPARAMVOID)
      continue;
    int cid = TCCIR_DECODE_CALL_ID((int32_t)irop_get_imm64_ex(
        ir, tcc_ir_op_get_src2(ir, &ir->compact_instructions[i])));
    if (cid < 0 || cid > max_cid ||
        !(has_call[cid / 8] & (1 << (cid % 8)))) {
      ssa_opt_nop_instr(ctx, i);
      changes++;
    }
  }
  tcc_free(has_call);
  return changes;
}

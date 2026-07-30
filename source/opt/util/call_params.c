/*
 *  TCC IR - Call-parameter and callee-symbol edits
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_utils.h"

int ir_opt_get_call_param_operand(TCCIRState *ir, int call_idx, int param_idx, IROperand *out)
{
  IRQuadCompact *call_q;
  IROperand call_src2;
  int call_id;

  if (!ir || call_idx < 0 || call_idx >= ir->next_instruction_index || !out)
    return 0;

  call_q = &ir->compact_instructions[call_idx];
  if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
    return 0;

  call_src2 = tcc_ir_op_get_src2(ir, call_q);
  call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, call_src2));

  for (int i = call_idx - 1; i >= 0; --i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
      continue;

    IROperand enc = tcc_ir_op_get_src2(ir, q);
    uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, enc);
    if (TCCIR_DECODE_CALL_ID(encoded) != call_id)
      continue;
    if (TCCIR_DECODE_PARAM_IDX(encoded) != param_idx)
      continue;

    *out = tcc_ir_op_get_src1(ir, q);
    return 1;
  }

  return 0;
}

int ir_opt_get_call_param_index(TCCIRState *ir, int call_idx, int param_idx)
{
  IRQuadCompact *call_q;
  IROperand call_src2;
  int call_id;

  if (!ir || call_idx < 0 || call_idx >= ir->next_instruction_index)
    return -1;

  call_q = &ir->compact_instructions[call_idx];
  if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
    return -1;

  call_src2 = tcc_ir_op_get_src2(ir, call_q);
  call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, call_src2));

  for (int i = call_idx - 1; i >= 0; --i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
      continue;

    IROperand enc = tcc_ir_op_get_src2(ir, q);
    uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, enc);
    if (TCCIR_DECODE_CALL_ID(encoded) != call_id)
      continue;
    if (TCCIR_DECODE_PARAM_IDX(encoded) != param_idx)
      continue;

    return i;
  }

  return -1;
}

void ir_opt_nop_call_params(TCCIRState *ir, int call_idx)
{
  IRQuadCompact *call_q;
  int call_id;

  if (!ir || call_idx < 0 || call_idx >= ir->next_instruction_index)
    return;

  call_q = &ir->compact_instructions[call_idx];
  if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
    return;

  call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call_q)));
  for (int i = call_idx - 1; i >= 0; --i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand enc;
    uint32_t encoded;

    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
      continue;

    enc = tcc_ir_op_get_src2(ir, q);
    encoded = (uint32_t)irop_get_imm64_ex(ir, enc);
    if (TCCIR_DECODE_CALL_ID(encoded) == call_id)
      q->op = TCCIR_OP_NOP;
  }
}

void ir_opt_nop_call_param(TCCIRState *ir, int call_idx, int param_idx)
{
  IRQuadCompact *call_q;
  int call_id;

  if (!ir || call_idx < 0 || call_idx >= ir->next_instruction_index)
    return;

  call_q = &ir->compact_instructions[call_idx];
  if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
    return;

  call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call_q)));
  for (int i = call_idx - 1; i >= 0; --i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand enc;
    uint32_t encoded;

    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
      continue;

    enc = tcc_ir_op_get_src2(ir, q);
    encoded = (uint32_t)irop_get_imm64_ex(ir, enc);
    if (TCCIR_DECODE_CALL_ID(encoded) == call_id && TCCIR_DECODE_PARAM_IDX(encoded) == param_idx)
      q->op = TCCIR_OP_NOP;
  }
}

void ir_opt_change_call_argc(TCCIRState *ir, int call_idx, int argc)
{
  IRQuadCompact *call_q;
  uint32_t encoded;
  int call_id;

  if (!ir || call_idx < 0 || call_idx >= ir->next_instruction_index)
    return;

  call_q = &ir->compact_instructions[call_idx];
  if (call_q->op != TCCIR_OP_FUNCCALLVAL && call_q->op != TCCIR_OP_FUNCCALLVOID)
    return;

  encoded = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call_q));
  call_id = TCCIR_DECODE_CALL_ID(encoded);
  tcc_ir_set_src2(ir, call_idx, irop_make_imm32(-1, (int32_t)TCCIR_ENCODE_CALL(call_id, argc), IROP_BTYPE_INT32));
}

int change_callee_sym(TCCIRState *ir, int instr_idx, const char *new_name, int ret_btype)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IRPoolSymref *entry = irop_get_symref_ex(ir, src1);
  if (!entry)
    return 0;

  CType ftype;
  ftype.t = VT_FUNC;
  ftype.ref = sym_push2(&global_stack, SYM_FIELD, ret_btype, 0);
  if (!ftype.ref)
    return 0; /* out of symbols: leave callee unchanged */
  ftype.ref->f.func_call = FUNC_CDECL;
  ftype.ref->f.func_type = FUNC_OLD;

  Sym *new_sym = external_global_sym(tok_alloc_const(new_name), &ftype);
  if (!new_sym)
    return 0;
  if (entry->sym == new_sym)
    return 0; /* already this callee: no change, so the optimizer converges */
  entry->sym = new_sym;
  return 1;
}

int change_callee_sym_keep_type(TCCIRState *ir, int instr_idx, const char *new_name)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IRPoolSymref *entry = irop_get_symref_ex(ir, src1);
  Sym *new_sym;

  if (!entry || !entry->sym)
    return 0;

  new_sym = external_global_sym(tok_alloc_const(new_name), &entry->sym->type);
  if (!new_sym)
    return 0;
  if (entry->sym == new_sym)
    return 0; /* already this callee: no change, so the optimizer converges */

  entry->sym = new_sym;
  return 1;
}

/*
 *  TCC IR - Integer Arithmetic Generation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

/* ============================================================================
 * Token to IR Operation Mapping
 * ============================================================================ */

TccIrOp tcc_irop_from_token(int token)
{
  switch (token)
  {
  case '+':
    return TCCIR_OP_ADD;
  case TOK_ADDC1:
    return TCCIR_OP_ADC_GEN;
  case TOK_ADDC2:
    return TCCIR_OP_ADC_USE;
  case '-':
    return TCCIR_OP_SUB;
  case TOK_SUBC1:
    return TCCIR_OP_SUBC_GEN;
  case TOK_SUBC2:
    return TCCIR_OP_SUBC_USE;
  case '&':
    return TCCIR_OP_AND;
  case '^':
    return TCCIR_OP_XOR;
  case '|':
    return TCCIR_OP_OR;
  case '*':
    return TCCIR_OP_MUL;
  case TOK_UMULL:
    return TCCIR_OP_UMULL;
  case TOK_SMULL:
    return TCCIR_OP_SMULL;
  case TOK_SHL:
    return TCCIR_OP_SHL;
  case TOK_SAR:
    return TCCIR_OP_SAR;
  case TOK_SHR:
    return TCCIR_OP_SHR;
  case '/':
    return TCCIR_OP_DIV;
  case TOK_PDIV:
    return TCCIR_OP_DIV;
  case TOK_UDIV:
    return TCCIR_OP_UDIV;
  case '%':
    return TCCIR_OP_IMOD;
  case TOK_UMOD:
    return TCCIR_OP_UMOD;
  case TOK_EQ:
  case TOK_NE:
  case TOK_LT:
  case TOK_GT:
  case TOK_LE:
  case TOK_GE:
  case TOK_ULT:
  case TOK_UGT:
  case TOK_ULE:
  case TOK_UGE:
    return TCCIR_OP_CMP;
  default:
    fprintf(stderr, "tcc_irop_from_token: unknown token %d(0x%x)\n", token, token);
    exit(1);
    return TCCIR_OP_NOP; /* unreachable, silences warning */
  };
}

/* ============================================================================
 * Core Integer IR Generation
 * ============================================================================ */

void tcc_ir_gen_i(TCCIRState *ir, int op)
{
  const TccIrOp ir_op = tcc_irop_from_token(op);
  SValue dest;

  if (ir_op == TCCIR_OP_CMP)
  {
    tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], NULL);
    --vtop;
    vtop->r = VT_CMP;
    vtop->cmp_op = op;
    vtop->jfalse = -1; /* -1 = no chain */
    vtop->jtrue = -1;  /* -1 = no chain */
    return;
  }

  svalue_init(&dest);
  dest.vr = tcc_ir_get_vreg_temp(ir);
  dest.r = 0;
  /* Most integer ops preserve the operand type, but UMULL/SMULL produce a 64-bit result. */
  if (ir_op == TCCIR_OP_UMULL)
  {
    dest.type.t = VT_LLONG | VT_UNSIGNED;
    tcc_ir_set_llong_type(ir, dest.vr);
  }
  else if (ir_op == TCCIR_OP_SMULL)
  {
    dest.type.t = VT_LLONG;
    tcc_ir_set_llong_type(ir, dest.vr);
  }
  else
  {
    dest.type.t = vtop[-1].type.t;
  }
  tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], &dest);
  vtop[-1].vr = dest.vr;
  vtop[-1].r = 0;
  vtop[-1].type = dest.type; /* Update type - critical for UMULL/SMULL which produce 64-bit from 32-bit inputs */
  --vtop;
}

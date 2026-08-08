/*
 *  TCC IR - Inline Assembly Blocks
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

#ifdef CONFIG_TCC_ASM

/* Ensure inline asm array has capacity for needed elements */
static void tcc_ir_inline_asms_ensure_capacity(TCCIRState *ir, int needed)
{
  if (!ir)
    return;
  if (ir->inline_asm_capacity >= needed)
    return;
  int new_cap = ir->inline_asm_capacity ? ir->inline_asm_capacity : 8;
  while (new_cap < needed)
    new_cap <<= 1;
  ir->inline_asms = tcc_realloc(ir->inline_asms, sizeof(TCCIRInlineAsm) * new_cap);
  memset(ir->inline_asms + ir->inline_asm_capacity, 0, sizeof(TCCIRInlineAsm) * (new_cap - ir->inline_asm_capacity));
  ir->inline_asm_capacity = new_cap;
}

/* Add inline assembly block, return ID */
int tcc_ir_asm_add(TCCIRState *ir, const char *asm_str, int asm_len, int must_subst, ASMOperand *operands,
                   int nb_operands, int nb_outputs, int nb_labels, const uint8_t *clobber_regs)
{
  if (!ir)
    return -1;
  if (!asm_str || asm_len < 0)
    tcc_error("IR: invalid inline asm string");
  if (nb_operands < 0 || nb_operands > MAX_ASM_OPERANDS)
    tcc_error("IR: invalid asm operand count");
  if (nb_labels < 0 || nb_operands + nb_labels > MAX_ASM_OPERANDS)
    tcc_error("IR: invalid asm label count");
  if (nb_outputs < 0 || nb_outputs > nb_operands)
    tcc_error("IR: invalid asm output count");

  tcc_ir_inline_asms_ensure_capacity(ir, ir->inline_asm_count + 1);
  const int id = ir->inline_asm_count++;
  TCCIRInlineAsm *ia = &ir->inline_asms[id];

  ia->asm_len = asm_len;
  ia->asm_str = tcc_mallocz((size_t)asm_len + 1);
  memcpy(ia->asm_str, asm_str, (size_t)asm_len);
  ia->must_subst = must_subst;
  ia->nb_operands = nb_operands;
  ia->nb_outputs = nb_outputs;
  ia->nb_labels = nb_labels;
  if (clobber_regs)
    memcpy(ia->clobber_regs, clobber_regs, NB_ASM_REGS);
  else
    memset(ia->clobber_regs, 0, NB_ASM_REGS);

  ia->operands = tcc_mallocz(sizeof(ASMOperand) * (nb_operands + nb_labels));
  memcpy(ia->operands, operands, sizeof(ASMOperand) * (nb_operands + nb_labels));

  ia->values = tcc_mallocz(sizeof(SValue) * nb_operands);
  for (int i = 0; i < nb_operands; ++i)
  {
    if (!operands[i].vt)
      tcc_error("IR: asm operand missing value");
    ia->values[i] = *operands[i].vt;
    ia->operands[i].vt = &ia->values[i];
  }
  for (int i = nb_operands; i < nb_operands + nb_labels; ++i)
  {
    ia->operands[i].vt = NULL;
  }

  /* Conservative: inline asm is call-like for leaf analysis. */
  ir->leaffunc = 0;

  return id;
}

/* Put inline assembly instruction */
void tcc_ir_asm_put(TCCIRState *ir, int asm_id)
{
  if (!ir)
    return;
  SValue id_sv = tcc_svalue_const_i64(asm_id);
  (void)tcc_ir_put(ir, TCCIR_OP_INLINE_ASM, &id_sv, NULL, NULL);
  ir->leaffunc = 0;
}

/* Legacy wrapper for tcc_ir_asm_add */
int tcc_ir_add_inline_asm(TCCIRState *ir, const char *asm_str, int asm_len, int must_subst, ASMOperand *operands,
                          int nb_operands, int nb_outputs, int nb_labels, const uint8_t *clobber_regs)
{
  return tcc_ir_asm_add(ir, asm_str, asm_len, must_subst, operands, nb_operands, nb_outputs, nb_labels, clobber_regs);
}

/* Legacy wrapper for tcc_ir_asm_put */
void tcc_ir_put_inline_asm(TCCIRState *ir, int inline_asm_id)
{
  tcc_ir_asm_put(ir, inline_asm_id);
}

#endif /* CONFIG_TCC_ASM */

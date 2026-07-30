/*
 *  TCC IR - small global memset to a direct store (flat, pre-SSA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_utils.h"



/* memset(&global[off], 0, N) becomes one naturally-aligned STORE #0; ARMv8-M Baseline faults on unaligned access. */
/* Depends on the tu_static_writer late-reopt cleanup: such a function must not be re-emitted twice. */
int tcc_ir_opt_small_global_memset_to_store(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;
    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
      continue;
    int is_aeabi = strcmp(name, "__aeabi_memset") == 0;
    if (!is_aeabi && strcmp(name, "memset") != 0)
      continue;

    /* memset returns dst: the FUNCCALLVAL form is droppable only if nothing reads that vreg. */
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      int32_t ret_vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      int has_reader = 0;
      if (ret_vr >= 0)
      {
        for (int j = 0; j < n && !has_reader; j++)
        {
          if (j == i)
            continue;
          IRQuadCompact *sq = &ir->compact_instructions[j];
          if (sq->op == TCCIR_OP_NOP)
            continue;
          if (irop_config[sq->op].has_src1)
          {
            IROperand s = tcc_ir_op_get_src1(ir, sq);
            if (irop_has_vreg(s) && irop_get_vreg(s) == ret_vr)
              has_reader = 1;
          }
          if (!has_reader && irop_config[sq->op].has_src2)
          {
            IROperand s = tcc_ir_op_get_src2(ir, sq);
            if (irop_has_vreg(s) && irop_get_vreg(s) == ret_vr)
              has_reader = 1;
          }
        }
      }
      if (has_reader)
        continue;
    }

    /* memset(dst, val, len);  __aeabi_memset(dst, len, val). */
    IROperand p_dst, p1, p2;
    if (!ir_opt_get_call_param_operand(ir, i, 0, &p_dst))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 1, &p1))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 2, &p2))
      continue;
    IROperand p_fill = is_aeabi ? p2 : p1;
    IROperand p_size = is_aeabi ? p1 : p2;

    /* Fill must be the constant 0. */
    if (irop_get_tag(p_fill) != IROP_TAG_IMM32)
      continue;
    if ((int)irop_get_imm64_ex(ir, p_fill) != 0)
      continue;

    /* Cap at 4: an 8-byte strd needs the zero in two registers, negating the win. */
    if (irop_get_tag(p_size) != IROP_TAG_IMM32)
      continue;
    int total_size = (int)irop_get_imm64_ex(ir, p_size);
    if (total_size <= 0 || total_size > 4)
      continue;

    /* Dest must be a global symbol address (symref LEA form: is_lval=0, is_local=0). */
    if (irop_get_tag(p_dst) != IROP_TAG_SYMREF || p_dst.is_lval || p_dst.is_local)
      continue;
    IRPoolSymref *sr = irop_get_symref_ex(ir, p_dst);
    if (!sr || !sr->sym)
      continue;
    int32_t base_addend = sr->addend;
    if (base_addend < 0)
      continue;

    /* Natural alignment is a safe lower bound on what the linker gives the symbol. */
    int salign = 1;
    {
      int a = 1;
      if (type_size(&sr->sym->type, &a) > 0 && a > 0)
        salign = a;
    }
    if (salign > 8)
      salign = 8;

    /* Single naturally-aligned store covering exactly [0, total_size). */
    int addr_align = salign;
    if (base_addend != 0)
    {
      int off_align = base_addend & -base_addend;
      if (off_align < addr_align)
        addr_align = off_align;
    }
    int btype;
    if (total_size == 4 && addr_align >= 4)
      btype = IROP_BTYPE_INT32;
    else if (total_size == 2 && addr_align >= 2)
      btype = IROP_BTYPE_INT16;
    else if (total_size == 1)
      btype = IROP_BTYPE_INT8;
    else
      continue; /* would need >1 store — leave as a call */

    /* Repurpose the CALL slot itself as the STORE; NOP the call's PARAMs. */
    int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
    uint32_t sidx = tcc_ir_pool_add_symref(ir, sr->sym, base_addend, sr->flags);
    IROperand st_dest = irop_make_symref(-1, sidx, /*is_lval*/ 1, /*is_local*/ 0, /*is_const*/ 0, btype);
    IROperand st_src = irop_make_imm32(-1, 0, btype);
    int pool_base = tcc_ir_iroperand_pool_add(ir, st_dest);
    tcc_ir_iroperand_pool_add(ir, st_src);

    for (int j = i - 1; j >= 0; j--)
    {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      if (pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID)
        continue;
      uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, pq));
      if (TCCIR_DECODE_CALL_ID(enc) == call_id)
        pq->op = TCCIR_OP_NOP;
    }
    q->op = TCCIR_OP_STORE;
    q->operand_base = pool_base;
    changes++;
  }

  return changes;
}

/*
 *  TCC IR - small local memset to direct stores (flat, pre-SSA)
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



/* memset(&stack[off], 0, N) with N <= 8 becomes one or two direct STORE #0. */
int tcc_ir_opt_small_memset_to_store(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;
    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
      continue;
    if (strcmp(name, "__aeabi_memset") != 0 && strcmp(name, "memset") != 0)
      continue;

    IROperand p_dst, p_size, p_fill;
    if (!ir_opt_get_call_param_operand(ir, i, 0, &p_dst))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 1, &p_size))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 2, &p_fill))
      continue;

    /* Fill must be 0 */
    if (irop_get_tag(p_fill) != IROP_TAG_IMM32)
      continue;
    if ((int)irop_get_imm64_ex(ir, p_fill) != 0)
      continue;

    /* Size must be a known small positive constant */
    if (irop_get_tag(p_size) != IROP_TAG_IMM32)
      continue;
    int total_size = (int)irop_get_imm64_ex(ir, p_size);
    if (total_size <= 0 || total_size > 8)
      continue;

    /* Dest must be a stack address (LEA form: is_lval=0, is_local=1) */
    if (irop_get_tag(p_dst) != IROP_TAG_STACKOFF || !p_dst.is_local || p_dst.is_lval)
      continue;
    int base_offset = (int)irop_get_imm64_ex(ir, p_dst);

    /* Decompose total_size into at most two power-of-2 stores: 8,4,2,1. */
    int chunk_btype[2] = {0, 0};
    int chunk_off[2] = {0, 0};
    int nchunks = 0;
    int remaining = total_size;
    int cur_off = 0;
    while (remaining > 0 && nchunks < 2)
    {
      int sz;
      int bt;
      if (remaining >= 8)
      {
        sz = 8;
        bt = IROP_BTYPE_INT64;
      }
      else if (remaining >= 4)
      {
        sz = 4;
        bt = IROP_BTYPE_INT32;
      }
      else if (remaining >= 2)
      {
        sz = 2;
        bt = IROP_BTYPE_INT16;
      }
      else
      {
        sz = 1;
        bt = IROP_BTYPE_INT8;
      }
      chunk_btype[nchunks] = bt;
      chunk_off[nchunks] = base_offset + cur_off;
      nchunks++;
      cur_off += sz;
      remaining -= sz;
    }
    if (remaining != 0)
      continue; /* would need >2 stores, skip */

    /* The second store needs a slot: repurpose a PARAM* of this call. */
    int extra_store_idx = -1;
    if (nchunks == 2)
    {
      int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
      for (int j = i - 1; j >= 0; j--)
      {
        IRQuadCompact *pq = &ir->compact_instructions[j];
        if (pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID)
          continue;
        IROperand enc = tcc_ir_op_get_src2(ir, pq);
        if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, enc)) != call_id)
          continue;
        extra_store_idx = j;
        break;
      }
      if (extra_store_idx < 0)
        continue;
    }

    /* Must NOP params before rewriting q: nop_call_params reads call_id from src2. */
    ir_opt_nop_call_params(ir, i);

    /* STORE pool layout is [dest, src1]. */
    IROperand st_dest0 = irop_make_stackoff(-1, chunk_off[0], /*is_lval*/ 1, /*is_llocal*/ 0, /*is_param*/ 0,
                                            chunk_btype[0]);
    IROperand st_src0 = irop_make_imm32(-1, 0, chunk_btype[0]);
    int pool_base0 = tcc_ir_iroperand_pool_add(ir, st_dest0);
    tcc_ir_iroperand_pool_add(ir, st_src0);
    q->op = TCCIR_OP_STORE;
    q->operand_base = pool_base0;

    if (nchunks == 2)
    {
      IROperand st_dest1 = irop_make_stackoff(-1, chunk_off[1], /*is_lval*/ 1, /*is_llocal*/ 0, /*is_param*/ 0,
                                              chunk_btype[1]);
      IROperand st_src1 = irop_make_imm32(-1, 0, chunk_btype[1]);
      int pool_base1 = tcc_ir_iroperand_pool_add(ir, st_dest1);
      tcc_ir_iroperand_pool_add(ir, st_src1);
      IRQuadCompact *eq = &ir->compact_instructions[extra_store_idx];
      eq->op = TCCIR_OP_STORE;
      eq->operand_base = pool_base1;
    }

    changes++;
  }

  return changes;
}

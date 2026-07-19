/*
 *  TCC IR - block-copy initializer materialization (flat, pre-SSA)
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



int tcc_ir_opt_block_copy_init(TCCIRState *ir)
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

    /* Param order is __aeabi_memset(dest, size, fill_value). */
    IROperand param_dest, param_size, param_fill;
    if (!ir_opt_get_call_param_operand(ir, i, 0, &param_dest))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 1, &param_size))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 2, &param_fill))
      continue;

    if (irop_get_tag(param_fill) != IROP_TAG_IMM32)
      continue;
    if ((int)irop_get_imm64_ex(ir, param_fill) != 0)
      continue;

    if (irop_get_tag(param_size) != IROP_TAG_IMM32)
      continue;
    int total_size = (int)irop_get_imm64_ex(ir, param_size);
    if (total_size <= 0 || (total_size & 3) || total_size > 1024)
      continue;

    if (irop_get_tag(param_dest) != IROP_TAG_STACKOFF)
      continue;
    int base_offset = (int)irop_get_imm64_ex(ir, param_dest);

    int store_indices[256];
    int store_offsets[256]; /* byte offset relative to base */
    int store_sizes[256];   /* 4 or 8 bytes */
    IROperand store_values[256];
    int nstores = 0;

    for (int j = i + 1; j < n && nstores < 256; j++)
    {
      IRQuadCompact *sq = &ir->compact_instructions[j];
      if (sq->op == TCCIR_OP_NOP)
        continue;
      if (sq->op != TCCIR_OP_STORE)
        break;

      IROperand st_dest = tcc_ir_op_get_dest(ir, sq);
      IROperand st_src = tcc_ir_op_get_src1(ir, sq);

      if (irop_get_tag(st_dest) != IROP_TAG_STACKOFF || !st_dest.is_local)
        break;

      int st_off = (int)irop_get_imm64_ex(ir, st_dest);
      int rel_off = st_off - base_offset;
      int is_wide = irop_is_64bit(st_dest);
      int st_size = is_wide ? 8 : 4;

      if (rel_off < 0 || rel_off + st_size > total_size || (rel_off & 3))
        break;

      int src_tag = irop_get_tag(st_src);
      if (src_tag != IROP_TAG_SYMREF && src_tag != IROP_TAG_IMM32 && src_tag != IROP_TAG_I64 &&
          src_tag != IROP_TAG_F32 && src_tag != IROP_TAG_F64)
        break;

      store_indices[nstores] = j;
      store_offsets[nstores] = rel_off;
      store_sizes[nstores] = st_size;
      store_values[nstores] = st_src;
      nstores++;
    }

    if (nstores < 2)
      continue;

    /* A block holding a symref needs a relocation, so it cannot go in shared
     * read-only .rodata; place it in the writable data segment instead. */
    int block_has_symref = 0;
    for (int s = 0; s < nstores; s++)
    {
      if (irop_get_tag(store_values[s]) == IROP_TAG_SYMREF)
      {
        block_has_symref = 1;
        break;
      }
    }
    Section *block_sec =
        (block_has_symref && tcc_state->share_rodata) ? data_section : rodata_section;

    size_t rodata_offset = section_add(block_sec, total_size, 4);
    uint8_t *rodata_ptr = block_sec->data + rodata_offset;
    memset(rodata_ptr, 0, total_size);

    for (int s = 0; s < nstores; s++)
    {
      int src_tag = irop_get_tag(store_values[s]);
      if (src_tag == IROP_TAG_SYMREF)
      {
        IRPoolSymref *symref = irop_get_symref_ex(ir, store_values[s]);
        if (symref && symref->sym)
        {
          write32le(rodata_ptr + store_offsets[s], symref->addend);
          greloc(block_sec, symref->sym, rodata_offset + store_offsets[s], R_DATA_PTR);
        }
      }
      else if (store_sizes[s] == 8)
      {
        int64_t val = irop_get_imm64_ex(ir, store_values[s]);
        write64le(rodata_ptr + store_offsets[s], (uint64_t)val);
      }
      else
      {
        int32_t val = (int32_t)irop_get_imm64_ex(ir, store_values[s]);
        write32le(rodata_ptr + store_offsets[s], val);
      }
    }

    CType ctype;
    ctype.t = VT_PTR | VT_CONST;
    ctype.ref = NULL;
    Sym *rodata_sym = get_sym_ref(&ctype, block_sec, rodata_offset, total_size);

    IROperand bc_dest = irop_make_stackoff(-1, base_offset, 1, 0, 0, IROP_BTYPE_INT32);
    uint32_t sym_pool_idx = tcc_ir_pool_add_symref(ir, rodata_sym, 0, 0);
    IROperand bc_src = irop_make_symref(-1, sym_pool_idx, 0, 0, 1, IROP_BTYPE_INT32);
    IROperand bc_size = irop_make_imm32(-1, total_size, VT_INT);

    int pool_base = tcc_ir_iroperand_pool_add(ir, bc_dest);
    tcc_ir_iroperand_pool_add(ir, bc_src);
    tcc_ir_iroperand_pool_add(ir, bc_size);

    IRQuadCompact *first_store = &ir->compact_instructions[store_indices[0]];
    first_store->op = TCCIR_OP_BLOCK_COPY;
    first_store->operand_base = pool_base;

    for (int s = 1; s < nstores; s++)
      ir->compact_instructions[store_indices[s]].op = TCCIR_OP_NOP;

    ir_opt_nop_call_params(ir, i);
    q->op = TCCIR_OP_NOP;

    changes++;
  }

  return changes;
}

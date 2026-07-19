/*
 *  TCC IR - Constant and constant-string operand evaluation
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

int is_power_of_2(int64_t n)
{
  if (n <= 0)
    return -1;
  if ((n & (n - 1)) != 0)
    return -1;
  int log = 0;
  while (n > 1)
  {
    n >>= 1;
    log++;
  }
  return log;
}

int ir_opt_eval_const_u64(TCCIRState *ir, IROperand op, int use_idx, uint64_t *out, int depth)
{
  int32_t vr;
  int def_idx;
  IRQuadCompact *q;

  if (!ir || !out || depth > 12)
    return 0;

  if (irop_is_immediate(op))
  {
    *out = (uint64_t)irop_get_imm64_ex(ir, op);
    return 1;
  }

  vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;

  if (ir_opt_vreg_address_taken_between(ir, vr, 0, use_idx))
    return 0;

  /* Multi-def vregs are unsound to trace: a back-edge def the linear scan never sees may be the one reaching use_idx. */
  if (!tcc_ir_vreg_has_single_def(ir, vr))
    return 0;

  def_idx = tcc_ir_find_defining_instruction(ir, vr, use_idx);
  if (def_idx < 0)
    return 0;

  q = &ir->compact_instructions[def_idx];
  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
    return ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, out, depth + 1);
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  case TCCIR_OP_ROR:
  {
    uint64_t v1, v2;
    if (!ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, &v1, depth + 1))
      return 0;
    if (!ir_opt_eval_const_u64(ir, tcc_ir_op_get_src2(ir, q), def_idx, &v2, depth + 1))
      return 0;
    /* Shifts must be evaluated at the operand width: a 64-bit shift of a sign-extended 32-bit value gives a different result than the runtime op. */
    IROperand shift_src1 = tcc_ir_op_get_src1(ir, q);
    int shift_btype = irop_get_btype(shift_src1);
    int shift_is_64 = (shift_btype == IROP_BTYPE_INT64 || shift_btype == IROP_BTYPE_FLOAT64);
    switch (q->op)
    {
    case TCCIR_OP_ADD:
      *out = v1 + v2;
      break;
    case TCCIR_OP_SUB:
      *out = v1 - v2;
      break;
    case TCCIR_OP_MUL:
      *out = v1 * v2;
      break;
    case TCCIR_OP_AND:
      *out = v1 & v2;
      break;
    case TCCIR_OP_OR:
      *out = v1 | v2;
      break;
    case TCCIR_OP_XOR:
      *out = v1 ^ v2;
      break;
    case TCCIR_OP_SHL:
      *out = v1 << v2;
      break;
    case TCCIR_OP_SHR:
      if (shift_is_64)
        *out = v1 >> v2;
      else
        *out = (uint64_t)((uint32_t)v1 >> (v2 & 31));
      break;
    case TCCIR_OP_SAR:
      if (shift_is_64)
        *out = (uint64_t)((int64_t)v1 >> v2);
      else
        *out = (uint64_t)((int64_t)(int32_t)(uint32_t)v1 >> (v2 & 31));
      break;
    case TCCIR_OP_ROR:
    {
      uint32_t v = (uint32_t)v1;
      uint32_t n = (uint32_t)v2 & 31;
      *out = (v >> n) | (v << (32 - n));
      break;
    }
    default:
      return 0;
    }
    return 1;
  }
  case TCCIR_OP_ZEXT:
  {
    uint64_t v;
    if (!ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, &v, depth + 1))
      return 0;
    IROperand sop = tcc_ir_op_get_src1(ir, q);
    int sb = irop_get_btype(sop);
    uint64_t mask;
    switch (sb)
    {
    case IROP_BTYPE_INT8:  mask = 0xFFULL; break;
    case IROP_BTYPE_INT16: mask = 0xFFFFULL; break;
    case IROP_BTYPE_INT32: mask = 0xFFFFFFFFULL; break;
    default:               mask = ~0ULL; break;
    }
    *out = v & mask;
    return 1;
  }
  case TCCIR_OP_CLZ:
  case TCCIR_OP_RBIT:
  case TCCIR_OP_REV:
  case TCCIR_OP_REV16:
  {
    uint64_t v;
    if (!ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, &v, depth + 1))
      return 0;
    uint32_t x = (uint32_t)v;
    switch (q->op)
    {
    case TCCIR_OP_CLZ:
      /* The hardware defines clz(0) == 32, so this needs no zero guard. */
      *out = x ? (uint64_t)__builtin_clz(x) : 32ULL;
      break;
    case TCCIR_OP_RBIT:
    {
      uint32_t r = 0;
      for (int b = 0; b < 32; b++)
        r |= ((x >> b) & 1u) << (31 - b);
      *out = r;
      break;
    }
    case TCCIR_OP_REV:
      *out = (uint64_t)__builtin_bswap32(x);
      break;
    case TCCIR_OP_REV16:
      *out = (uint64_t)(((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu));
      break;
    default:
      return 0;
    }
    return 1;
  }
  default:
    return 0;
  }
}

int ir_opt_eval_const_string(TCCIRState *ir, IROperand op, int use_idx, const char **out, int depth)
{
  const char *base;
  int32_t vr;
  int def_idx;
  IRQuadCompact *q;

  if (!ir || !out || depth > 16)
    return 0;

  if (op.is_lval && op.vreg_type == TCCIR_VREG_TYPE_TEMP)
    return 0;

  base = ir_opt_get_constant_string_from_symref(ir, op);
  if (base)
  {
    *out = base;
    return 1;
  }

  vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;

  if (ir_opt_vreg_address_taken_between(ir, vr, 0, use_idx))
    return 0;

  if (!tcc_ir_vreg_has_single_def(ir, vr))
    return 0;

  def_idx = tcc_ir_find_defining_instruction(ir, vr, use_idx);
  if (def_idx < 0)
    return 0;

  q = &ir->compact_instructions[def_idx];
  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
    return ir_opt_eval_const_string(ir, tcc_ir_op_get_src1(ir, q), def_idx, out, depth + 1);
  case TCCIR_OP_ADD:
  {
    uint64_t addend;
    if (ir_opt_eval_const_string(ir, tcc_ir_op_get_src1(ir, q), def_idx, out, depth + 1) &&
        ir_opt_eval_const_u64(ir, tcc_ir_op_get_src2(ir, q), def_idx, &addend, depth + 1))
    {
      *out += addend;
      return 1;
    }
    if (ir_opt_eval_const_string(ir, tcc_ir_op_get_src2(ir, q), def_idx, out, depth + 1) &&
        ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, &addend, depth + 1))
    {
      *out += addend;
      return 1;
    }
    return 0;
  }
  default:
    return 0;
  }
}

const char *ir_opt_get_constant_string_from_symref(TCCIRState *ir, IROperand op)
{
  IRPoolSymref *symref;
  Sym *sym;
  ElfSym *esym;
  Section *sec;
  const char *str;
  const char *nul;
  addr_t offset;
  size_t remaining;

  if (!ir || irop_get_tag(op) != IROP_TAG_SYMREF)
    return NULL;

  symref = irop_get_symref_ex(ir, op);
  if (!symref || symref->addend < 0)
    return NULL;
  if (symref->flags & IRPOOL_SYMREF_LVAL)
    return NULL;

  sym = symref->sym;
  if (!sym)
    return NULL;

  esym = elfsym(sym);
  if (!esym)
    return NULL;
  if (esym->st_shndx == SHN_UNDEF || esym->st_shndx >= (unsigned)tcc_state->nb_sections)
    return NULL;

  sec = tcc_state->sections[esym->st_shndx];
  if (!sec || !sec->data)
    return NULL;
  if (sec->sh_flags & SHF_WRITE)
    return NULL;
  if (esym->st_size == 0 || (addr_t)symref->addend >= esym->st_size)
    return NULL;

  offset = esym->st_value + (addr_t)symref->addend;
  if (offset >= sec->data_offset)
    return NULL;

  str = (const char *)(sec->data + offset);
  remaining = (size_t)(esym->st_size - (addr_t)symref->addend);
  nul = memchr(str, '\0', remaining);
  if (!nul)
    return NULL;

  return str;
}

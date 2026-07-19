/*
 *  TCC IR - Constant-string evaluators shared by the string-builtin folders
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Address/length evaluation over flat IR: resolve a call argument to a constant
 * string (literal symref or a stack buffer built by preceding STOREs) and to the
 * symref it is based on.  Consumed by source/opt/ssa/string (ssa:const_string_fold). */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

int ir_opt_eval_const_string_operand(TCCIRState *ir, IROperand op, int use_idx, IROperand *out, int depth)
{
  int32_t vr;
  int def_idx;
  IRQuadCompact *q;

  if (!ir || !out || depth > 16)
    return 0;

  if (op.is_lval && op.vreg_type == TCCIR_VREG_TYPE_TEMP)
    return 0;

  if (ir_opt_get_constant_string_from_symref(ir, op))
  {
    *out = op;
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
    return ir_opt_eval_const_string_operand(ir, tcc_ir_op_get_src1(ir, q), def_idx, out, depth + 1);
  case TCCIR_OP_ADD:
  {
    IROperand base_op;
    uint64_t addend;
    IRPoolSymref *symref;
    uint32_t new_idx;

    if (!ir_opt_eval_const_string_operand(ir, tcc_ir_op_get_src1(ir, q), def_idx, &base_op, depth + 1) ||
        !ir_opt_eval_const_u64(ir, tcc_ir_op_get_src2(ir, q), def_idx, &addend, depth + 1))
    {
      if (!ir_opt_eval_const_string_operand(ir, tcc_ir_op_get_src2(ir, q), def_idx, &base_op, depth + 1) ||
          !ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, &addend, depth + 1))
        return 0;
    }

    if (irop_get_tag(base_op) != IROP_TAG_SYMREF)
      return 0;

    symref = irop_get_symref_ex(ir, base_op);
    if (!symref)
      return 0;

    new_idx = tcc_ir_pool_add_symref(ir, symref->sym, symref->addend + (int32_t)addend, symref->flags);
    *out = irop_make_symref(irop_get_vreg(base_op), new_idx, base_op.is_lval, base_op.is_local, base_op.is_const,
                            irop_get_btype(base_op));
    return 1;
  }
  default:
    return 0;
  }
}

int ir_opt_fold_strcmp_result(const char *s1, const char *s2)
{
  while ((unsigned char)*s1 == (unsigned char)*s2)
  {
    if (*s1 == '\0')
      return 0;
    ++s1;
    ++s2;
  }

  return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

int ir_opt_fold_strncmp_result(const char *s1, const char *s2, uint64_t n)
{
  if (n == 0)
    return 0;

  while (n-- > 0)
  {
    unsigned char c1 = (unsigned char)*s1++;
    unsigned char c2 = (unsigned char)*s2++;
    if (c1 != c2 || c1 == '\0')
      return (int)c1 - (int)c2;
  }

  return 0;
}

int ir_opt_fold_memcmp_result(const char *s1, const char *s2, uint64_t n)
{
  uint64_t i;

  for (i = 0; i < n; ++i)
  {
    unsigned char c1 = (unsigned char)s1[i];
    unsigned char c2 = (unsigned char)s2[i];
    if (c1 != c2)
      return (int)c1 - (int)c2;
  }

  return 0;
}

int ir_opt_fold_memchr_offset(const char *s, unsigned char c, uint64_t n, int *out_offset)
{
  uint64_t i;

  if (!out_offset)
    return 0;

  for (i = 0; i < n; ++i)
  {
    if ((unsigned char)s[i] == c)
    {
      *out_offset = (int)i;
      return 1;
    }
  }

  *out_offset = -1;
  return 1;
}

static int ir_opt_btype_size(int btype)
{
  switch (btype)
  {
  case IROP_BTYPE_INT8:
    return 1;
  case IROP_BTYPE_INT16:
    return 2;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64:
    return 8;
  case IROP_BTYPE_STRUCT:
    return 0;
  default:
    return 4;
  }
}

static int ir_opt_stack_addr_offset(IROperand op, int *out_off)
{
  if (irop_get_tag(op) != IROP_TAG_STACKOFF || irop_get_vreg(op) != -1 || op.is_lval || !op.is_local)
    return 0;
  *out_off = (int)irop_get_stack_offset(op);
  return 1;
}

static int ir_opt_is_memcpy_like_name(const char *name)
{
  return name &&
         (strcmp(name, "memcpy") == 0 || strcmp(name, "memmove") == 0 ||
          strcmp(name, "__aeabi_memcpy") == 0 || strcmp(name, "__aeabi_memcpy4") == 0 ||
          strcmp(name, "__aeabi_memcpy8") == 0);
}

int ir_opt_eval_stack_strlen(TCCIRState *ir, IROperand arg, int call_idx, int *out_len)
{
  enum { MAX_TRACK = 256 };
  uint8_t bytes[MAX_TRACK];
  uint8_t known[MAX_TRACK];
  int base_off;

  if (!ir || !out_len || !ir_opt_stack_addr_offset(arg, &base_off))
    return 0;

  memset(bytes, 0, sizeof(bytes));
  memset(known, 0, sizeof(known));

  for (int i = 0; i < call_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
      continue;
    if (q->is_jump_target || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP)
      return 0;

    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dst = tcc_ir_op_get_dest(ir, q);
      IROperand src = tcc_ir_op_get_src1(ir, q);
      int dst_off;
      int size;
      int rel;
      uint64_t val;

      if (irop_get_tag(dst) != IROP_TAG_STACKOFF || !dst.is_lval || !dst.is_local || dst.is_llocal)
        return 0;

      dst_off = (int)irop_get_stack_offset(dst);
      size = ir_opt_btype_size(irop_get_btype(dst));
      if (size <= 0)
        return 0;
      rel = dst_off - base_off;
      if (rel + size <= 0 || rel >= MAX_TRACK)
        continue;

      if (!irop_is_immediate(src))
      {
        for (int b = 0; b < size; b++)
          if (rel + b >= 0 && rel + b < MAX_TRACK)
            known[rel + b] = 0;
        continue;
      }

      val = (uint64_t)irop_get_imm64_ex(ir, src);
      for (int b = 0; b < size; b++)
      {
        if (rel + b < 0 || rel + b >= MAX_TRACK)
          continue;
        bytes[rel + b] = (uint8_t)(val >> (b * 8));
        known[rel + b] = 1;
      }
      continue;
    }

    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      const char *name = callee ? get_tok_str(callee->v, NULL) : NULL;
      IROperand dst;
      IROperand src;
      IROperand len_op;
      const char *str;
      uint64_t n;
      int dst_off;
      int rel;

      if (!ir_opt_is_memcpy_like_name(name))
        return 0;
      if (!ir_opt_get_call_param_operand(ir, i, 0, &dst) ||
          !ir_opt_get_call_param_operand(ir, i, 1, &src) ||
          !ir_opt_get_call_param_operand(ir, i, 2, &len_op))
        return 0;
      if (!ir_opt_stack_addr_offset(dst, &dst_off) ||
          !ir_opt_eval_const_string(ir, src, i, &str, 0) ||
          !ir_opt_eval_const_u64(ir, len_op, i, &n, 0))
        return 0;
      if (n > (uint64_t)strlen(str) + 1)
        return 0;

      rel = dst_off - base_off;
      for (uint64_t b = 0; b < n; b++)
      {
        int pos = rel + (int)b;
        if (pos < 0 || pos >= MAX_TRACK)
          continue;
        bytes[pos] = (uint8_t)str[b];
        known[pos] = 1;
      }
      continue;
    }

    if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_BLOCK_COPY)
      return 0;
  }

  for (int i = 0; i < MAX_TRACK; i++)
  {
    if (!known[i])
      return 0;
    if (bytes[i] == 0)
    {
      *out_len = i;
      return 1;
    }
  }

  return 0;
}

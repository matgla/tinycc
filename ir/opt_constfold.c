/*
 *  TCC IR - Constant String/Call/Addrof Folding
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

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

static int ir_opt_fold_memchr_offset(const char *s, unsigned char c, uint64_t n, int *out_offset)
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

int tcc_ir_opt_const_string_calls(TCCIRState *ir)
{
  int changes = 0;

  if (!ir)
    return 0;

  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    Sym *callee;
    IROperand arg0;
    IROperand arg1;
    const char *s1;
    const char *s2;
    IROperand base_op;
    int folded_result;
    int arg0_is_const_string = 0;
    int arg1_is_const_string = 0;

    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;

    const char *name = get_tok_str(callee->v, NULL);
    const int id = resolve_str_builtin_id(callee->v, name);
    if (id == STRBI_UNKNOWN)
      continue;

    /* --- strlen: fold if arg is constant string, otherwise redirect --- */
    if (id == STRBI_STRLEN)
    {
      int stack_len;
      if (q->op == TCCIR_OP_FUNCCALLVAL && ir_opt_get_call_param_operand(ir, i, 0, &arg0) &&
          ir_opt_eval_const_string(ir, arg0, i, &s1, 0))
      {
        ir_opt_nop_call_params(ir, i);
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int)strlen(s1), VT_INT));
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
      else if (q->op == TCCIR_OP_FUNCCALLVAL && ir_opt_get_call_param_operand(ir, i, 0, &arg0) &&
               ir_opt_eval_stack_strlen(ir, arg0, i, &stack_len))
      {
        ir_opt_nop_call_params(ir, i);
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, stack_len, VT_INT));
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      }
      else
      {
        if (change_callee_sym_keep_type(ir, i, "__tcc_strlen"))
          changes++;
      }
      continue;
    }

    /* --- Simple redirects to __tcc_* helpers ---
       A static id->name table instead of an inline switch: the switch made
       this (large) function load ~14 distinct string-literal addresses via
       pc-relative pooled loads plus a jump table, which tripped a literal-pool
       placement miscompile in the self-hosted (cross-built) backend (a later
       case's pool reference fell into code -> garbage helper pointer ->
       strlen() crash). One array-base load + an indexed access sidesteps it. */
    {
      static const char *const strbi_helper[] = {
          [STRBI_MEMMOVE] = "__tcc_memmove", [STRBI_BCOPY] = "__tcc_bcopy",
          [STRBI_MEMPCPY] = "__tcc_mempcpy", [STRBI_STRCAT] = "__tcc_strcat",
          [STRBI_STRCHR] = "__tcc_strchr",   [STRBI_INDEX] = "__tcc_strchr",
          [STRBI_STRCPY] = "__tcc_strcpy",   [STRBI_STPCPY] = "__tcc_stpcpy",
          [STRBI_STPNCPY] = "__tcc_stpncpy", [STRBI_STRNLEN] = "__tcc_strnlen",
          [STRBI_STRPBRK] = "__tcc_strpbrk", [STRBI_STRRCHR] = "__tcc_strrchr",
          [STRBI_RINDEX] = "__tcc_strrchr",  [STRBI_STRSTR] = "__tcc_strstr",
          [STRBI_STRCSPN] = "__tcc_strcspn", [STRBI_STRNCPY] = "__tcc_strncpy",
          [STRBI_STRNCAT] = "__tcc_strncat",
      };
      const char *helper = NULL;
      if (id >= 0 && id < (int)(sizeof(strbi_helper) / sizeof(strbi_helper[0])))
        helper = strbi_helper[id];
      if (helper)
      {
        if (change_callee_sym_keep_type(ir, i, helper))
          changes++;
        continue;
      }
    }

    /* --- Functions that need argument analysis for folding --- */

    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    if (!ir_opt_get_call_param_operand(ir, i, 0, &arg0) || !ir_opt_get_call_param_operand(ir, i, 1, &arg1))
      continue;

    if (id == STRBI_MEMCHR)
    {
      IROperand arg2;
      uint64_t n;
      int match_offset;
      uint64_t needle_u64;
      if (!ir_opt_get_call_param_operand(ir, i, 2, &arg2) || !ir_opt_eval_const_u64(ir, arg2, i, &n, 0) ||
          !ir_opt_eval_const_string(ir, arg0, i, &s1, 0) ||
          !ir_opt_eval_const_string_operand(ir, arg0, i, &base_op, 0) ||
          !ir_opt_eval_const_u64(ir, arg1, i, &needle_u64, 0))
        continue;
      if (n > (uint64_t)strlen(s1) + 1)
        continue;

      if (!ir_opt_fold_memchr_offset(s1, (unsigned char)needle_u64, n, &match_offset))
        continue;

      ir_opt_nop_call_params(ir, i);
      q->op = TCCIR_OP_ASSIGN;
      if (match_offset < 0)
      {
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
      }
      else
      {
        IRPoolSymref *symref = irop_get_symref_ex(ir, base_op);
        uint32_t new_idx = tcc_ir_pool_add_symref(ir, symref->sym, symref->addend + match_offset, symref->flags);
        tcc_ir_set_src1(ir, i,
                        irop_make_symref(irop_get_vreg(base_op), new_idx, base_op.is_lval, base_op.is_local,
                                         base_op.is_const, irop_get_btype(base_op)));
      }
      tcc_ir_set_src2(ir, i, IROP_NONE);
      changes++;
      continue;
    }

    if (id == STRBI_MEMCMP)
    {
      IROperand arg2;
      uint64_t n;

      if (!ir_opt_get_call_param_operand(ir, i, 2, &arg2) || !ir_opt_eval_const_u64(ir, arg2, i, &n, 0))
        continue;

      if (n == 0)
      {
        ir_opt_nop_call_params(ir, i);
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, 0, VT_INT));
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
        continue;
      }

      if (n == 1)
      {
        ir_opt_nop_call_param(ir, i, 2);
        if (!change_callee_sym(ir, i, "__tcc_memcmp1", VT_INT))
          continue;
        ir_opt_change_call_argc(ir, i, 2);
        changes++;
        continue;
      }
    }

    if (id == STRBI_STRNCMP)
    {
      IROperand arg2;
      uint64_t n;

      if (!ir_opt_get_call_param_operand(ir, i, 2, &arg2) || !ir_opt_eval_const_u64(ir, arg2, i, &n, 0))
        continue;

      if (n == 0)
      {
        ir_opt_nop_call_params(ir, i);
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, 0, VT_INT));
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
        continue;
      }

      arg0_is_const_string = ir_opt_eval_const_string(ir, arg0, i, &s1, 0);
      arg1_is_const_string = ir_opt_eval_const_string(ir, arg1, i, &s2, 0);

      if (!(arg0_is_const_string && arg1_is_const_string))
      {
        if (!change_callee_sym(ir, i, "__tcc_strncmp", VT_INT))
          continue;
        changes++;
        continue;
      }
    }

    if (!arg0_is_const_string)
      arg0_is_const_string = ir_opt_eval_const_string(ir, arg0, i, &s1, 0);
    if (!arg1_is_const_string)
      arg1_is_const_string = ir_opt_eval_const_string(ir, arg1, i, &s2, 0);

    if (id == STRBI_STRCMP && !(arg0_is_const_string && arg1_is_const_string))
    {
      if (change_callee_sym_keep_type(ir, i, "__tcc_strcmp"))
        changes++;
      continue;
    }

    if (!arg0_is_const_string || !arg1_is_const_string)
      continue;

    if (id == STRBI_STRCMP)
      folded_result = ir_opt_fold_strcmp_result(s1, s2);
    else
    {
      IROperand arg2;
      uint64_t n;
      if (!ir_opt_get_call_param_operand(ir, i, 2, &arg2) || !ir_opt_eval_const_u64(ir, arg2, i, &n, 0))
        continue;
      if (n > (uint64_t)strlen(s1) + 1 || n > (uint64_t)strlen(s2) + 1)
        continue;
      if (id == STRBI_STRNCMP)
        folded_result = ir_opt_fold_strncmp_result(s1, s2, n);
      else
        folded_result = ir_opt_fold_memcmp_result(s1, s2, n);
    }

    ir_opt_nop_call_params(ir, i);
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, irop_make_imm32(-1, folded_result, VT_INT));
    tcc_ir_set_src2(ir, i, IROP_NONE);
    changes++;
  }

  return changes;
}

/* Eliminate memcpy/memmove calls whose dst and src arguments compute the
 * same value — the copy is a no-op regardless of length or overlap.
 *
 * Triggered notably by `*p = *p` aggregate self-assignments (e.g. an
 * identity-shuffle result stored back to its own source), where struct/
 * vector lowering already emits a memmove(p, p, sizeof). */
int tcc_ir_opt_self_copy_elim(TCCIRState *ir)
{
  int changes = 0;

  if (!ir)
    return 0;

  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;
    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
      continue;

    /* memcpy(dst,src,n), memmove(dst,src,n) and the AAPCS aligned variants
     * all have the same dst,src,n argument layout and return dst. */
    int is_memcpy_like =
      strcmp(name, "memmove") == 0 || strcmp(name, "memcpy") == 0 ||
      strcmp(name, "__tcc_memmove") == 0 ||
      strcmp(name, "__aeabi_memmove") == 0 ||
      strcmp(name, "__aeabi_memmove4") == 0 ||
      strcmp(name, "__aeabi_memmove8") == 0 ||
      strcmp(name, "__aeabi_memcpy") == 0 ||
      strcmp(name, "__aeabi_memcpy4") == 0 ||
      strcmp(name, "__aeabi_memcpy8") == 0;
    if (!is_memcpy_like)
      continue;

    IROperand p0, p1;
    if (!ir_opt_get_call_param_operand(ir, i, 0, &p0) ||
        !ir_opt_get_call_param_operand(ir, i, 1, &p1))
      continue;

    /* Resolve each param's source at its own marshalling site, not at the call
     * index.  If the source temp is redefined between param0 and param1, using
     * the call index as the use-site for both collapses them to the same (last)
     * reaching definition and the self-copy fold fires incorrectly. */
    int p0_idx = ir_opt_get_call_param_index(ir, i, 0);
    int p1_idx = ir_opt_get_call_param_index(ir, i, 1);
    if (p0_idx < 0 || p1_idx < 0)
      continue;

    if (!ir_opt_pure_expr_equal(ir, p0, p0_idx, p1, p1_idx, 0))
      continue;

    /* Self-copy: NOP the param marshalling and the call itself.
     * For FUNCCALLVAL, the result is dst (== src) — rewrite as ASSIGN. */
    ir_opt_nop_call_params(ir, i);
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, p0);
      tcc_ir_set_src2(ir, i, IROP_NONE);
    }
    else
    {
      q->op = TCCIR_OP_NOP;
    }
    changes++;
  }

  return changes;
}


/* ir_opt_pure_def_equal, ir_opt_pure_expr_equal, ir_opt_is_pure_fallthrough_instruction
 * moved to opt_utils.c */


typedef struct
{
  const char *double_name;
  const char *float_name;
} FloatNarrowEntry;

static const FloatNarrowEntry float_narrow_table[] = {
    {"floor", "floorf"}, {"ceil", "ceilf"},           {"trunc", "truncf"}, {"round", "roundf"},
    {"fabs", "fabsf"},   {"nearbyint", "nearbyintf"}, {"rint", "rintf"},
};
#define NUM_FLOAT_NARROW (sizeof(float_narrow_table) / sizeof(float_narrow_table[0]))

/* Tracking structure for f2d / d2f calls */
typedef struct
{
  int param_idx;  /* instruction index of the FUNCPARAMVAL */
  int call_idx;   /* instruction index of the FUNCCALLVAL */
  int32_t src_vr; /* original source vreg (float for f2d, double for d2f) */
  int32_t dst_vr; /* result vreg */
  int call_id;    /* IR call_id */
} ConvCallInfo;

#define MAX_CONV_CALLS 32

int tcc_ir_opt_float_narrowing(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 4)
    return 0;

  /* Phase 1: Collect f2d and d2f conversion calls */
  ConvCallInfo f2d_calls[MAX_CONV_CALLS];
  ConvCallInfo d2f_calls[MAX_CONV_CALLS];
  int num_f2d = 0, num_d2f = 0;

  /* Also track: for each instruction that is a FUNCPARAMVAL, record the
   * instruction index and the source vreg, keyed by (call_id, param_idx).
   * We do this in a linear scan. */

  int pending_param_idx = -1;
  int32_t pending_param_src_vr = -1;
  int pending_param_call_id = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, src2);
      int param_idx_val = TCCIR_DECODE_PARAM_IDX(encoded);

      if (param_idx_val == 0)
      {
        /* Track the most recent param 0 */
        pending_param_idx = i;
        pending_param_src_vr = irop_is_immediate(src1) ? -1 : irop_get_vreg(src1);
        pending_param_call_id = TCCIR_DECODE_CALL_ID(encoded);
      }
      continue;
    }

    if (q->op == TCCIR_OP_FUNCCALLVAL && pending_param_idx >= 0)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      Sym *callee = irop_get_sym_ex(ir, src1);
      if (!callee)
      {
        pending_param_idx = -1;
        continue;
      }

      const char *name = get_tok_str(callee->v, NULL);
      if (!name)
      {
        pending_param_idx = -1;
        continue;
      }

      uint32_t call_encoded = (uint32_t)irop_get_imm64_ex(ir, src2);
      int this_call_id = TCCIR_DECODE_CALL_ID(call_encoded);

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dst_vr = irop_get_vreg(dest);

      if (strcmp(name, "__aeabi_f2d") == 0 && this_call_id == pending_param_call_id)
      {
        if (num_f2d < MAX_CONV_CALLS)
        {
          f2d_calls[num_f2d].param_idx = pending_param_idx;
          f2d_calls[num_f2d].call_idx = i;
          f2d_calls[num_f2d].src_vr = pending_param_src_vr;
          f2d_calls[num_f2d].dst_vr = dst_vr;
          f2d_calls[num_f2d].call_id = this_call_id;
          num_f2d++;
        }
      }
      else if (strcmp(name, "__aeabi_d2f") == 0 && this_call_id == pending_param_call_id)
      {
        if (num_d2f < MAX_CONV_CALLS)
        {
          d2f_calls[num_d2f].param_idx = pending_param_idx;
          d2f_calls[num_d2f].call_idx = i;
          d2f_calls[num_d2f].src_vr = pending_param_src_vr;
          d2f_calls[num_d2f].dst_vr = dst_vr;
          d2f_calls[num_d2f].call_id = this_call_id;
          num_d2f++;
        }
      }

      pending_param_idx = -1;
      continue;
    }

    /* Reset pending param tracking on non-param, non-call instructions */
    if (q->op != TCCIR_OP_NOP)
      pending_param_idx = -1;
  }

  if (num_f2d == 0)
    return 0;

  /* Phase 2: For each narrowable function call, check if:
   * - Its parameter is an f2d result
   * - Its result feeds into a d2f (Case 1) or not (Case 2) */

  /* Re-scan for function calls with matching f2d parameters */
  pending_param_idx = -1;
  pending_param_src_vr = -1;
  pending_param_call_id = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, src2);
      int param_idx_val = TCCIR_DECODE_PARAM_IDX(encoded);

      if (param_idx_val == 0)
      {
        pending_param_idx = i;
        pending_param_src_vr = irop_is_immediate(src1) ? -1 : irop_get_vreg(src1);
        pending_param_call_id = TCCIR_DECODE_CALL_ID(encoded);
      }
      continue;
    }

    if (q->op != TCCIR_OP_FUNCCALLVAL || pending_param_idx < 0)
    {
      if (q->op != TCCIR_OP_NOP && q->op != TCCIR_OP_FUNCPARAMVOID)
        pending_param_idx = -1;
      continue;
    }

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    Sym *callee = irop_get_sym_ex(ir, src1);
    if (!callee)
    {
      pending_param_idx = -1;
      continue;
    }

    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
    {
      pending_param_idx = -1;
      continue;
    }

    /* Check if this is a narrowable function */
    const char *float_name = NULL;
    for (size_t j = 0; j < NUM_FLOAT_NARROW; j++)
    {
      if (strcmp(name, float_narrow_table[j].double_name) == 0)
      {
        float_name = float_narrow_table[j].float_name;
        break;
      }
    }

    if (!float_name)
    {
      pending_param_idx = -1;
      continue;
    }

    /* Check if param 0 comes from an f2d result */
    ConvCallInfo *f2d_info = NULL;
    for (int k = 0; k < num_f2d; k++)
    {
      if (f2d_calls[k].dst_vr == pending_param_src_vr)
      {
        f2d_info = &f2d_calls[k];
        break;
      }
    }

    if (!f2d_info)
    {
      pending_param_idx = -1;
      continue;
    }

    uint32_t call_encoded = (uint32_t)irop_get_imm64_ex(ir, src2);
    (void)call_encoded;
    IROperand func_dest = tcc_ir_op_get_dest(ir, q);
    int32_t func_result_vr = irop_get_vreg(func_dest);
    int func_call_idx = i;
    int func_param_idx = pending_param_idx;

    /* Check if result feeds a d2f (Case 1) */
    ConvCallInfo *d2f_info = NULL;
    for (int k = 0; k < num_d2f; k++)
    {
      if (d2f_calls[k].src_vr == func_result_vr)
      {
        d2f_info = &d2f_calls[k];
        break;
      }
    }

    if (d2f_info)
    {
      /* ===== Case 1: f2d → func → d2f =====
       * Transform to: floorf(original_float) → T_float_result
       * NOP out the f2d and d2f conversion calls. */

      /* 1. Change func's FUNCPARAMVAL to use the original float arg */
      IROperand orig_float_param = tcc_ir_op_get_src1(ir, &ir->compact_instructions[f2d_info->param_idx]);
      tcc_ir_set_src1(ir, func_param_idx, orig_float_param);

      /* 2. Change func's FUNCCALLVAL callee to float variant */
      change_callee_sym(ir, func_call_idx, float_name, VT_FLOAT);

      /* 3. Change func's FUNCCALLVAL dest to d2f's result vreg */
      IROperand d2f_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[d2f_info->call_idx]);
      tcc_ir_set_dest(ir, func_call_idx, d2f_dest);

      /* 4. NOP out f2d (param + call) */
      ir->compact_instructions[f2d_info->param_idx].op = TCCIR_OP_NOP;
      ir->compact_instructions[f2d_info->call_idx].op = TCCIR_OP_NOP;

      /* 5. NOP out d2f (param + call) */
      ir->compact_instructions[d2f_info->param_idx].op = TCCIR_OP_NOP;
      ir->compact_instructions[d2f_info->call_idx].op = TCCIR_OP_NOP;

      LOG_IR_GEN("FLOAT NARROW (Case 1): %s → %s at i=%d, NOP'd f2d@%d and d2f@%d", name, float_name, func_call_idx,
                 f2d_info->call_idx, d2f_info->call_idx);
      changes++;
    }
    else
    {
      /* ===== Case 2: f2d → func, result stays double =====
       * Swap callees: f2d becomes floorf, func becomes f2d.
       * Before: f2d(float) → T_double → func(T_double) → T_result
       * After:  floorf(float) → T_float → f2d(T_float) → T_result */

      /* 1. Change f2d's callee to the float variant */
      change_callee_sym(ir, f2d_info->call_idx, float_name, VT_FLOAT);

      /* 2. Change func's callee to __aeabi_f2d */
      change_callee_sym(ir, func_call_idx, "__aeabi_f2d", VT_INT);

      LOG_IR_GEN("FLOAT NARROW (Case 2): swapped %s↔f2d at i=%d,%d", name, f2d_info->call_idx, func_call_idx);
      changes++;
    }

    /* Invalidate modified f2d entry to prevent double-processing */
    f2d_info->dst_vr = -1;

    pending_param_idx = -1;
  }

  return changes;
}

/* ============================================================================
 * Stack Address CSE (Common Subexpression Elimination) Optimization
 * ============================================================================
 *
 * Eliminates redundant stack-address computations across loops.
 *
 * After IV strength reduction, array loops use pointer-based iteration with
 * an end-of-array bound:
 *   T_end = Addr[StackLoc[X]]    ; ASSIGN
 *   T_end = T_end ADD #C         ; ADD constant
 *
 * When multiple loops access the same array, each loop recomputes the same
 * end pointer.  This pass detects duplicates and replaces later occurrences
 * with the first computation, provided the result vreg is not redefined
 * in between.
 *
 * Before (bench_array_sum):
 *   T16 = Addr[StackLoc[-1024]]    ; init loop end ptr
 *   T16 = T16 ADD #1024
 *   ... init loop (reads T16) ...
 *   T18 = Addr[StackLoc[-1024]]    ; sum loop end ptr (redundant!)
 *   T18 = T18 ADD #1024
 *   ... sum loop (reads T18) ...
 *
 * After:
 *   T16 = Addr[StackLoc[-1024]]    ; computed once
 *   T16 = T16 ADD #1024
 *   ... init loop (reads T16) ...
 *   NOP                            ; eliminated
 *   NOP                            ; eliminated
 *   ... sum loop (reads T16) ...   ; T18 replaced with T16
 */

int tcc_ir_detect_const_result(TCCIRState *ir, int64_t *value, int *btype)
{
  int n = ir->next_instruction_index;
  if (n == 0 || ir->parameters_count > 0)
    return 0;

  int non_nop_count = 0;
  int ret_idx = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    non_nop_count++;

    switch (q->op)
    {
    case TCCIR_OP_ASSIGN:
    case TCCIR_OP_RETURNVALUE:
      break;
    default:
      return 0;
    }

    if (q->op == TCCIR_OP_RETURNVALUE)
      ret_idx = i;
  }

  if (ret_idx < 0 || non_nop_count > 4)
    return 0;

  IRQuadCompact *ret_q = &ir->compact_instructions[ret_idx];
  IROperand src1 = tcc_ir_op_get_src1(ir, ret_q);

  if (irop_is_immediate(src1))
  {
    *value = irop_get_imm64_ex(ir, src1);
    *btype = irop_get_btype(src1);
    return 1;
  }

  int32_t ret_vr = irop_get_vreg(src1);
  if (ret_vr < 0)
    return 0;

  for (int i = ret_idx - 1; i >= 0; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (irop_get_vreg(dest) == ret_vr)
      {
        IROperand as1 = tcc_ir_op_get_src1(ir, q);
        if (irop_is_immediate(as1))
        {
          *value = irop_get_imm64_ex(ir, as1);
          *btype = irop_get_btype(as1);
          return 1;
        }
        return 0;
      }
    }
    break;
  }

  return 0;
}

void tcc_ir_cache_const_result(TCCState *s, int func_token, int64_t value, int btype)
{
  if (s->func_const_result_cache_count >= FUNC_CONST_RESULT_CACHE_SIZE)
    return;
  for (int i = 0; i < s->func_const_result_cache_count; i++)
  {
    if (s->func_const_result_cache[i].token == func_token)
      return;
  }
  int idx = s->func_const_result_cache_count++;
  s->func_const_result_cache[idx].token = func_token;
  s->func_const_result_cache[idx].value = value;
  s->func_const_result_cache[idx].btype = btype;
}

int tcc_ir_lookup_const_result(TCCState *s, int func_token, int64_t *value, int *btype)
{
  for (int i = 0; i < s->func_const_result_cache_count; i++)
  {
    if (s->func_const_result_cache[i].token == func_token)
    {
      *value = s->func_const_result_cache[i].value;
      *btype = s->func_const_result_cache[i].btype;
      return 1;
    }
  }
  return 0;
}

int tcc_ir_opt_const_call_replace(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0 || !tcc_state || tcc_state->func_const_result_cache_count == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    IROperand callee_op = tcc_ir_op_get_src1(ir, q);
    Sym *callee = irop_get_sym_ex(ir, callee_op);
    if (!callee)
      continue;

    int64_t val;
    int btype;
    if (!tcc_ir_lookup_const_result(tcc_state, callee->v, &val, &btype))
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand call_info = tcc_ir_op_get_src2(ir, q);
    int call_id = TCCIR_DECODE_CALL_ID((int)irop_get_imm64_ex(ir, call_info));
    int32_t dest_vr = irop_get_vreg(dest);

    LOG_IR_GEN("OPTIMIZE: IPC replace call to %s with #%lld at i=%d", get_tok_str(callee->v, NULL), (long long)val, i);

    /* If the return value has no allocated vreg, the result is discarded.
     * Functions in the const-result cache are pure (ASSIGN+RETURNVALUE only),
     * so the call has no side effects — NOP it instead of leaving a dead
     * ASSIGN-to-nowhere that DCE can't always remove. */
    if (dest_vr < 0)
    {
      q->op = TCCIR_OP_NOP;
    }
    else
    {
      q->op = TCCIR_OP_ASSIGN;
      if (val == (int32_t)val)
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)val, btype));
      else
      {
        uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
        tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, btype));
      }
      tcc_ir_set_src2(ir, i, IROP_NONE);
      tcc_ir_set_dest(ir, i, dest);
    }

    for (int j = i - 1; j >= 0; j--)
    {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      if (pq->op == TCCIR_OP_NOP)
        continue;
      if (pq->op == TCCIR_OP_FUNCPARAMVAL || pq->op == TCCIR_OP_FUNCPARAMVOID)
      {
        IROperand ps2 = tcc_ir_op_get_src2(ir, pq);
        int p_call_id = TCCIR_DECODE_CALL_ID((int)irop_get_imm64_ex(ir, ps2));
        if (p_call_id == call_id)
          pq->op = TCCIR_OP_NOP;
        continue;
      }
      break;
    }

    changes++;
  }

  return changes;
}

/* ============================================================================
 * Switch-Value Function IPCP
 * ----------------------------------------------------------------------------
 * Some single-parameter static functions have the shape
 *   T f(int x) { switch (x) { case K1: return C1; ...; default: return CD; } }
 * After optimization, the body lowers to a pure dispatcher (NOP/ASSIGN/CMP/
 * JUMP/JUMPIF/RETURNVALUE) with no stores, calls, or loads.  When a caller
 * passes a constant, we can fold the call to a constant by replaying the
 * dispatcher.  Sister of tcc_ir_opt_const_call_replace, which handles the
 * stronger "always returns the same constant" case.
 * ============================================================================ */

#define SWITCH_FUNC_MAX_OPS 512
#define SWITCH_FUNC_SIM_MAX_VREGS 64
#define SWITCH_FUNC_SIM_MAX_REPLAY 64

/* Compact replayable op.
 * flags bit 0: src1 is immediate; bit 1: src2 is immediate;
 *       bit 2: src1 is an lval-symref (LOAD-from-global form of ASSIGN);
 *       bit 3: dest is an lval-symref (STORE-to-global). */
typedef struct SwitchSimOp
{
  uint8_t op;
  uint8_t flags;
  int32_t dest_vreg;
  int32_t src1_vreg;
  int64_t src1_imm;
  int32_t src2_vreg;
  int64_t src2_imm;
  int32_t target;
  /* lval-symref payload (used when flags bit 2 or 3 set). The sym pointer
   * persists across compilation units (Syms outlive their IR), so caching the
   * snapshot across functions is safe. */
  struct Sym *sym;
  int32_t sym_addend;
  uint16_t sym_flags;
  int8_t sym_btype;
} SwitchSimOp;

struct TCCFuncSwitchSnapshot
{
  int token;
  int param_vreg;
  int btype;
  int op_count;
  SwitchSimOp *ops;
};

void tcc_ir_switch_func_snapshot_free(TCCFuncSwitchSnapshot *snap)
{
  if (!snap)
    return;
  tcc_free(snap->ops);
  tcc_free(snap);
}

/* Return-type btypes we know how to fold into the caller. */
static int switch_func_is_supported_btype(int btype)
{
  return btype == IROP_BTYPE_INT32 || btype == IROP_BTYPE_INT8 || btype == IROP_BTYPE_INT16;
}

/* Decode an operand as either an immediate (returns 1, fills *out_imm and sets
 * *out_vreg=-1) or as a vreg-ref (returns 1, fills *out_vreg).  Returns 0 if
 * the operand is anything else (lval, addrof, symref, etc.).
 */
static int switch_func_decode_operand(TCCIRState *ir, IROperand op, int32_t *out_vreg, int64_t *out_imm)
{
  if (op.is_lval || op.is_llocal)
    return 0;
  /* Reject 64-bit and FP/complex operands — our simulator runs as int64,
   * which is fine for ≤32-bit values but doesn't model the paired-register
   * lowering that 64-bit ops require at the CMP/ASSIGN level. */
  int btype = irop_get_btype(op);
  if (btype != IROP_BTYPE_INT32 && btype != IROP_BTYPE_INT8 && btype != IROP_BTYPE_INT16)
    return 0;
  if (irop_is_immediate(op))
  {
    *out_vreg = -1;
    *out_imm = irop_get_imm64_ex(ir, op);
    return 1;
  }
  int tag = irop_get_tag(op);
  if (tag != IROP_TAG_VREG)
    return 0;
  int32_t v = irop_get_vreg(op);
  if (v < 0)
    return 0;
  *out_vreg = v;
  *out_imm = 0;
  return 1;
}

/* Detect an lval-symref operand: a load-from-global (when used as src) or a
 * store-to-global (when used as dest). Fills *snap_op's sym fields on success.
 * Rejects llocal (double-indirection), pointer arithmetic with a non-constant
 * addend (those would arrive as non-SYMREF tags anyway), and FP/64-bit btypes.
 * Returns 1 on success. */
static int switch_func_decode_lval_sym(TCCIRState *ir, IROperand op, SwitchSimOp *snap_op)
{
  if (!op.is_lval || op.is_llocal)
    return 0;
  if (irop_get_tag(op) != IROP_TAG_SYMREF)
    return 0;
  int btype = irop_get_btype(op);
  if (btype != IROP_BTYPE_INT32 && btype != IROP_BTYPE_INT8 && btype != IROP_BTYPE_INT16)
    return 0;
  IRPoolSymref *sr = irop_get_symref_ex(ir, op);
  if (!sr || !sr->sym)
    return 0;
  /* Refuse stack-local symrefs — those alias the caller's frame in ways we
   * can't replay safely. Global syms have c != 0 (data section offset). */
  snap_op->sym = sr->sym;
  snap_op->sym_addend = sr->addend;
  snap_op->sym_flags = (uint16_t)sr->flags;
  snap_op->sym_btype = (int8_t)btype;
  return 1;
}

int tcc_ir_detect_switch_func(TCCIRState *ir, TCCFuncSwitchSnapshot **out)
{
  if (!ir || !out)
    return 0;
  if (ir->parameters_count != 1)
    return 0;

  /* Only accept 32-bit-or-smaller scalar parameters.  64-bit/FP/complex
   * arguments lower to pairs of physical registers and our simple
   * single-vreg simulator can't reason about them — be conservative and
   * skip them rather than risk a wrong fold. */
  if (ir->parameters_live_intervals_size < 1 || !ir->parameters_live_intervals)
    return 0;
  const IRLiveInterval *piv = &ir->parameters_live_intervals[0];
  if (piv->is_llong || piv->is_float || piv->is_double || piv->is_complex)
    return 0;
  if (piv->addrtaken)
    return 0;

  int n = ir->next_instruction_index;
  if (n == 0 || n > SWITCH_FUNC_MAX_OPS)
    return 0;

  int param_vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0);

  SwitchSimOp *ops = tcc_mallocz(n * sizeof(SwitchSimOp));
  int return_btype = -1;
  int has_return = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    SwitchSimOp *o = &ops[i];
    o->op = q->op;
    o->dest_vreg = -1;
    o->src1_vreg = -1;
    o->src2_vreg = -1;
    o->target = -1;

    switch (q->op)
    {
    case TCCIR_OP_NOP:
      break;
    case TCCIR_OP_ASSIGN:
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (d.is_lval || d.is_llocal)
        goto fail;
      int32_t dvr = irop_get_vreg(d);
      if (dvr < 0)
        goto fail;
      o->dest_vreg = dvr;
      /* src1 may be: immediate, plain vreg, or an lval-symref (LOAD form). */
      if (switch_func_decode_lval_sym(ir, s, o))
      {
        o->flags |= 4; /* src1 is lval-sym load */
        break;
      }
      int64_t imm;
      int32_t svr;
      if (!switch_func_decode_operand(ir, s, &svr, &imm))
        goto fail;
      if (svr < 0)
      {
        o->flags |= 1;
        o->src1_imm = imm;
      }
      else
      {
        o->src1_vreg = svr;
      }
      break;
    }
    case TCCIR_OP_STORE:
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      IROperand s = tcc_ir_op_get_src1(ir, q);
      /* STORE to a global: dest must be lval-symref. */
      if (!switch_func_decode_lval_sym(ir, d, o))
        goto fail;
      o->flags |= 8; /* dest is lval-sym store */
      /* Value being stored may be immediate or vreg (must be 32-bit-or-smaller). */
      int64_t imm;
      int32_t svr;
      if (!switch_func_decode_operand(ir, s, &svr, &imm))
        goto fail;
      if (svr < 0)
      {
        o->flags |= 1;
        o->src1_imm = imm;
      }
      else
      {
        o->src1_vreg = svr;
      }
      break;
    }
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      if (d.is_lval || d.is_llocal)
        goto fail;
      int32_t dvr = irop_get_vreg(d);
      if (dvr < 0)
        goto fail;
      o->dest_vreg = dvr;
      int64_t imm;
      int32_t vr;
      if (!switch_func_decode_operand(ir, s1, &vr, &imm))
        goto fail;
      if (vr < 0) { o->flags |= 1; o->src1_imm = imm; } else o->src1_vreg = vr;
      if (!switch_func_decode_operand(ir, s2, &vr, &imm))
        goto fail;
      if (vr < 0) { o->flags |= 2; o->src2_imm = imm; } else o->src2_vreg = vr;
      break;
    }
    case TCCIR_OP_CMP:
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      int64_t imm;
      int32_t vr;
      if (!switch_func_decode_operand(ir, s1, &vr, &imm))
        goto fail;
      if (vr < 0) { o->flags |= 1; o->src1_imm = imm; } else o->src1_vreg = vr;
      if (!switch_func_decode_operand(ir, s2, &vr, &imm))
        goto fail;
      if (vr < 0) { o->flags |= 2; o->src2_imm = imm; } else o->src2_vreg = vr;
      break;
    }
    case TCCIR_OP_JUMP:
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int64_t t = irop_get_imm64_ex(ir, d);
      if (t < 0 || t >= n)
        goto fail;
      o->target = (int32_t)t;
      break;
    }
    case TCCIR_OP_JUMPIF:
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand d = tcc_ir_op_get_dest(ir, q);
      o->src1_imm = irop_get_imm64_ex(ir, s1); /* condition code (TOK_*) */
      int64_t t = irop_get_imm64_ex(ir, d);
      if (t < 0 || t >= n)
        goto fail;
      o->target = (int32_t)t;
      break;
    }
    case TCCIR_OP_RETURNVALUE:
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      int64_t imm;
      int32_t vr;
      if (!switch_func_decode_operand(ir, s1, &vr, &imm))
        goto fail;
      if (vr < 0) { o->flags |= 1; o->src1_imm = imm; } else o->src1_vreg = vr;
      int b = irop_get_btype(s1);
      if (return_btype < 0)
        return_btype = b;
      else if (return_btype != b)
        goto fail;
      has_return = 1;
      break;
    }
    default:
      goto fail;
    }
  }

  if (!has_return)
    goto fail;
  if (!switch_func_is_supported_btype(return_btype))
    goto fail;

  TCCFuncSwitchSnapshot *snap = tcc_mallocz(sizeof(*snap));
  snap->token = 0;
  snap->param_vreg = param_vreg;
  snap->btype = return_btype;
  snap->op_count = n;
  snap->ops = ops;
  *out = snap;
  return 1;

fail:
  tcc_free(ops);
  return 0;
}

void tcc_ir_cache_switch_func(TCCState *s, int func_token, TCCFuncSwitchSnapshot *snap)
{
  if (!s || !snap)
  {
    tcc_ir_switch_func_snapshot_free(snap);
    return;
  }
  for (int i = 0; i < s->func_switch_cache_count; i++)
  {
    if (s->func_switch_cache[i] && s->func_switch_cache[i]->token == func_token)
    {
      tcc_ir_switch_func_snapshot_free(snap);
      return;
    }
  }
  if (s->func_switch_cache_count >= FUNC_SWITCH_CACHE_SIZE)
  {
    tcc_ir_switch_func_snapshot_free(snap);
    return;
  }
  snap->token = func_token;
  s->func_switch_cache[s->func_switch_cache_count++] = snap;
}

const TCCFuncSwitchSnapshot *tcc_ir_lookup_switch_func(TCCState *s, int func_token)
{
  if (!s)
    return NULL;
  for (int i = 0; i < s->func_switch_cache_count; i++)
  {
    const TCCFuncSwitchSnapshot *snap = s->func_switch_cache[i];
    if (snap && snap->token == func_token)
      return snap;
  }
  return NULL;
}

void tcc_ir_free_switch_func_cache(TCCState *s)
{
  if (!s)
    return;
  for (int i = 0; i < s->func_switch_cache_count; i++)
    tcc_ir_switch_func_snapshot_free(s->func_switch_cache[i]);
  s->func_switch_cache_count = 0;
}

/* Small linear-search vreg→value map.  Functions we accept are small
 * (≤ SWITCH_FUNC_MAX_OPS instructions, typically a few dozen distinct vregs),
 * so linear scan is fine.  A vreg may be "known" (concrete int64) or
 * "tracked-unknown" (the simulator saw an assignment to it from a value it
 * couldn't compute, e.g. a load from a global).  Tracked-unknown values
 * propagate through arithmetic and force the producing op to be replayed at
 * the caller. */
typedef struct SwitchSimEnv
{
  int32_t vregs[SWITCH_FUNC_SIM_MAX_VREGS];
  int64_t values[SWITCH_FUNC_SIM_MAX_VREGS];
  uint8_t known[SWITCH_FUNC_SIM_MAX_VREGS]; /* 1 if `values[i]` is concrete */
  int count;
} SwitchSimEnv;

static int switch_sim_set(SwitchSimEnv *env, int32_t vreg, int64_t value, int known)
{
  for (int i = 0; i < env->count; i++)
  {
    if (env->vregs[i] == vreg)
    {
      env->values[i] = value;
      env->known[i] = (uint8_t)known;
      return 1;
    }
  }
  if (env->count >= SWITCH_FUNC_SIM_MAX_VREGS)
    return 0;
  env->vregs[env->count] = vreg;
  env->values[env->count] = value;
  env->known[env->count] = (uint8_t)known;
  env->count++;
  return 1;
}

/* Get a vreg's value.  Returns: 1=found and known (writes *out), 2=found but
 * tracked-unknown, 0=not in env. */
static int switch_sim_get(const SwitchSimEnv *env, int32_t vreg, int64_t *out)
{
  for (int i = 0; i < env->count; i++)
  {
    if (env->vregs[i] == vreg)
    {
      if (env->known[i])
      {
        *out = env->values[i];
        return 1;
      }
      return 2;
    }
  }
  return 0;
}

/* Read the value of src1 from `o`.  Returns 1=known (writes *out), 2=unknown
 * (tracked vreg with no concrete value yet), 0=hard fail (operand refers to a
 * vreg the simulator never saw). */
static int switch_sim_read_src(const SwitchSimEnv *env, const SwitchSimOp *o,
                               int which /* 1 or 2 */, int64_t *out)
{
  if (which == 1)
  {
    if (o->flags & 1) { *out = o->src1_imm; return 1; }
    return switch_sim_get(env, o->src1_vreg, out);
  }
  if (o->flags & 2) { *out = o->src2_imm; return 1; }
  return switch_sim_get(env, o->src2_vreg, out);
}

/* Record an op index for replay at the caller; cap protects unbounded growth. */
static int switch_sim_record_replay(int *replay, int *count, int op_idx)
{
  if (*count >= SWITCH_FUNC_SIM_MAX_REPLAY)
    return 0;
  replay[(*count)++] = op_idx;
  return 1;
}

/* Simulate the snapshot for one constant `arg_value`.  On success returns 1
 * and writes:
 *   *out_value, *out_btype  — the constant return value (must be known)
 *   replay_indices[0..*replay_count]  — snapshot op indices the caller must
 *      emit (in order) to preserve side effects.  Pass replay_indices=NULL to
 *      bail on any function whose execution would require replay (preserves
 *      the original pure-folding contract). */
int tcc_ir_simulate_switch_func_ex(const TCCFuncSwitchSnapshot *snap, int64_t arg_value,
                                   int64_t *out_value, int *out_btype,
                                   int *replay_indices, int *replay_count)
{
  if (!snap || !out_value)
    return 0;

  SwitchSimEnv env;
  env.count = 0;
  if (!switch_sim_set(&env, snap->param_vreg, arg_value, 1))
    return 0;

  int local_replay_count = 0;
  if (replay_count)
    *replay_count = 0;

  int64_t flag_lhs = 0, flag_rhs = 0;
  int has_flags = 0;
  int pc = 0;
  int step_limit = 4 * snap->op_count + 64;

  while (pc >= 0 && pc < snap->op_count)
  {
    if (--step_limit < 0)
      return 0;
    const SwitchSimOp *o = &snap->ops[pc];
    switch (o->op)
    {
    case TCCIR_OP_NOP:
      pc++;
      break;
    case TCCIR_OP_ASSIGN:
    {
      /* LOAD form: src1 is an lval-symref.  Dest becomes tracked-unknown and
       * the op must be replayed at the caller. */
      if (o->flags & 4)
      {
        if (!replay_indices)
          return 0;
        if (!switch_sim_set(&env, o->dest_vreg, 0, 0))
          return 0;
        if (!switch_sim_record_replay(replay_indices, &local_replay_count, pc))
          return 0;
        pc++;
        break;
      }
      int64_t v;
      int r = switch_sim_read_src(&env, o, 1, &v);
      if (r == 0)
        return 0;
      if (r == 2)
      {
        /* src1 tracked-unknown → dest tracked-unknown, replay op. */
        if (!replay_indices)
          return 0;
        if (!switch_sim_set(&env, o->dest_vreg, 0, 0))
          return 0;
        if (!switch_sim_record_replay(replay_indices, &local_replay_count, pc))
          return 0;
      }
      else
      {
        if (!switch_sim_set(&env, o->dest_vreg, v, 1))
          return 0;
      }
      pc++;
      break;
    }
    case TCCIR_OP_STORE:
    {
      if (!replay_indices)
        return 0;
      /* Verify src1 is at least readable (known or unknown); we don't need its
       * value here — the replayed op will refer to it.  But if it's a vreg we
       * never saw, that's a structural problem and we bail. */
      int64_t v;
      int r = switch_sim_read_src(&env, o, 1, &v);
      if (r == 0)
        return 0;
      if (!switch_sim_record_replay(replay_indices, &local_replay_count, pc))
        return 0;
      pc++;
      break;
    }
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    {
      int64_t l = 0, r1 = 0;
      int rl = switch_sim_read_src(&env, o, 1, &l);
      int rr = switch_sim_read_src(&env, o, 2, &r1);
      if (rl == 0 || rr == 0)
        return 0;
      if (rl == 1 && rr == 1)
      {
        int64_t v = (o->op == TCCIR_OP_ADD) ? (l + r1) : (l - r1);
        if (!switch_sim_set(&env, o->dest_vreg, v, 1))
          return 0;
      }
      else
      {
        if (!replay_indices)
          return 0;
        if (!switch_sim_set(&env, o->dest_vreg, 0, 0))
          return 0;
        if (!switch_sim_record_replay(replay_indices, &local_replay_count, pc))
          return 0;
      }
      pc++;
      break;
    }
    case TCCIR_OP_CMP:
    {
      int64_t l, r;
      int rl = switch_sim_read_src(&env, o, 1, &l);
      int rr = switch_sim_read_src(&env, o, 2, &r);
      /* Both operands must be concrete to decide an upcoming JUMPIF. */
      if (rl != 1 || rr != 1)
        return 0;
      flag_lhs = l;
      flag_rhs = r;
      has_flags = 1;
      pc++;
      break;
    }
    case TCCIR_OP_JUMP:
      pc = o->target;
      break;
    case TCCIR_OP_JUMPIF:
    {
      if (!has_flags)
        return 0;
      int32_t ls = (int32_t)flag_lhs;
      int32_t rs = (int32_t)flag_rhs;
      uint32_t lu = (uint32_t)flag_lhs;
      uint32_t ru = (uint32_t)flag_rhs;
      int taken;
      switch ((int)o->src1_imm)
      {
      case TOK_EQ:  taken = (ls == rs); break;
      case TOK_NE:  taken = (ls != rs); break;
      case TOK_LT:  taken = (ls <  rs); break;
      case TOK_LE:  taken = (ls <= rs); break;
      case TOK_GT:  taken = (ls >  rs); break;
      case TOK_GE:  taken = (ls >= rs); break;
      case TOK_ULT: taken = (lu <  ru); break;
      case TOK_ULE: taken = (lu <= ru); break;
      case TOK_UGT: taken = (lu >  ru); break;
      case TOK_UGE: taken = (lu >= ru); break;
      default: return 0;
      }
      pc = taken ? o->target : pc + 1;
      break;
    }
    case TCCIR_OP_RETURNVALUE:
    {
      int64_t v;
      int r = switch_sim_read_src(&env, o, 1, &v);
      /* Return value must be a concrete constant — we can't substitute a
       * tracked-unknown into the caller's ASSIGN destination. */
      if (r != 1)
        return 0;
      *out_value = v;
      if (out_btype)
        *out_btype = snap->btype;
      if (replay_count)
        *replay_count = local_replay_count;
      return 1;
    }
    default:
      return 0;
    }
  }
  return 0;
}

/* Backwards-compatible wrapper: pure-fold only, no replay. */
int tcc_ir_simulate_switch_func(const TCCFuncSwitchSnapshot *snap, int64_t arg_value,
                                int64_t *out_value, int *out_btype)
{
  return tcc_ir_simulate_switch_func_ex(snap, arg_value, out_value, out_btype, NULL, NULL);
}

/* Map of callee vreg → caller tmp vreg, used while emitting a replayed case
 * body at the call site.  Each unique callee vreg gets one fresh caller TEMP
 * vreg; the simulator-known values are inlined as immediates instead. */
typedef struct VregMap
{
  int32_t callee_vreg[SWITCH_FUNC_SIM_MAX_VREGS];
  int32_t caller_vreg[SWITCH_FUNC_SIM_MAX_VREGS];
  int count;
} VregMap;

static int32_t map_callee_vreg(VregMap *m, TCCIRState *ir, int32_t callee_vr)
{
  for (int k = 0; k < m->count; k++)
    if (m->callee_vreg[k] == callee_vr)
      return m->caller_vreg[k];
  if (m->count >= SWITCH_FUNC_SIM_MAX_VREGS)
    return -1;
  int32_t fresh = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, ir->next_temporary_variable++);
  m->callee_vreg[m->count] = callee_vr;
  m->caller_vreg[m->count] = fresh;
  m->count++;
  return fresh;
}

/* Insert `new_q` into the caller's IR just before `before_idx`, fixing up all
 * jump targets that pointed at or past `before_idx`.  Returns 1 on success. */
static int switch_insert_before(TCCIRState *ir, int before_idx, const IRQuadCompact *new_q)
{
  if (ir->next_instruction_index + 1 >= ir->compact_instructions_size)
  {
    int new_size = ir->compact_instructions_size << 1;
    ir->compact_instructions =
        (IRQuadCompact *)tcc_realloc(ir->compact_instructions, sizeof(IRQuadCompact) * new_size);
    if (!ir->compact_instructions)
      return 0;
    ir->compact_instructions_size = new_size;
  }
  for (int k = ir->next_instruction_index; k > before_idx; k--)
    ir->compact_instructions[k] = ir->compact_instructions[k - 1];
  ir->compact_instructions[before_idx] = *new_q;
  ir->next_instruction_index++;
  for (int k = 0; k < ir->next_instruction_index; k++)
  {
    IRQuadCompact *q = &ir->compact_instructions[k];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);
      if (target >= before_idx && k != before_idx)
      {
        IROperand new_dest = irop_make_imm32(-1, target + 1, IROP_BTYPE_INT32);
        tcc_ir_op_set_dest(ir, q, new_dest);
      }
    }
  }
  return 1;
}

/* Build the caller-side operand for a snapshot op's value source.
 * If src1 of the snapshot op is an immediate, returns an imm32 operand.
 * If it's a callee vreg that the simulator left tracked-unknown, returns a
 * mapped caller vreg.  If known, inlines the constant as imm32.
 * Returns 1 on success. */
static int build_caller_src1(TCCIRState *caller_ir, VregMap *m, const SwitchSimOp *o,
                             const SwitchSimEnv *sim_env, int btype, IROperand *out)
{
  if (o->flags & 1)
  {
    *out = irop_make_imm32(-1, (int32_t)o->src1_imm, btype);
    return 1;
  }
  /* Vreg source: prefer the simulator-known value if any (folds the operand
   * to a constant), else allocate a caller tmp via the vreg map. */
  int64_t v;
  if (switch_sim_get(sim_env, o->src1_vreg, &v) == 1)
  {
    *out = irop_make_imm32(-1, (int32_t)v, btype);
    return 1;
  }
  int32_t cvr = map_callee_vreg(m, caller_ir, o->src1_vreg);
  if (cvr < 0)
    return 0;
  *out = irop_make_vreg(cvr, btype);
  return 1;
}

static int build_caller_src2(TCCIRState *caller_ir, VregMap *m, const SwitchSimOp *o,
                             const SwitchSimEnv *sim_env, int btype, IROperand *out)
{
  if (o->flags & 2)
  {
    *out = irop_make_imm32(-1, (int32_t)o->src2_imm, btype);
    return 1;
  }
  int64_t v;
  if (switch_sim_get(sim_env, o->src2_vreg, &v) == 1)
  {
    *out = irop_make_imm32(-1, (int32_t)v, btype);
    return 1;
  }
  int32_t cvr = map_callee_vreg(m, caller_ir, o->src2_vreg);
  if (cvr < 0)
    return 0;
  *out = irop_make_vreg(cvr, btype);
  return 1;
}

/* Emit one replay op at the caller, just before `*pcall_idx`.  Bumps
 * *pcall_idx by 1 on success so subsequent inserts land in order.
 * Re-runs a per-call mini-simulation to recover the known/unknown state of
 * each vreg at the emit point (so constant inlining matches what the main
 * simulator saw). */
static int emit_replay_op(TCCIRState *ir, VregMap *m, const TCCFuncSwitchSnapshot *snap,
                          const SwitchSimEnv *sim_env, int snap_op_idx, int *pcall_idx)
{
  const SwitchSimOp *o = &snap->ops[snap_op_idx];
  IRQuadCompact nq = {0};
  nq.op = o->op;

  switch (o->op)
  {
  case TCCIR_OP_ASSIGN:
  {
    int btype = (o->flags & 4) ? o->sym_btype : IROP_BTYPE_INT32;
    int32_t dvr = map_callee_vreg(m, ir, o->dest_vreg);
    if (dvr < 0) return 0;
    IROperand dest = irop_make_vreg(dvr, btype);
    IROperand src1;
    if (o->flags & 4)
    {
      /* LOAD form: build a fresh symref operand in the caller's pool. */
      uint32_t sidx = tcc_ir_pool_add_symref(ir, o->sym, o->sym_addend, o->sym_flags);
      src1 = irop_make_symref(-1, sidx, 1 /* is_lval */, 0, 0, o->sym_btype);
    }
    else if (!build_caller_src1(ir, m, o, sim_env, btype, &src1))
      return 0;
    nq.operand_base = tcc_ir_pool_add(ir, dest);
    tcc_ir_pool_add(ir, src1);
    break;
  }
  case TCCIR_OP_STORE:
  {
    /* dest is lval-symref; src1 is value (vreg or imm). */
    int btype = o->sym_btype;
    uint32_t sidx = tcc_ir_pool_add_symref(ir, o->sym, o->sym_addend, o->sym_flags);
    IROperand dest = irop_make_symref(-1, sidx, 1, 0, 0, btype);
    IROperand src1;
    if (!build_caller_src1(ir, m, o, sim_env, btype, &src1))
      return 0;
    nq.operand_base = tcc_ir_pool_add(ir, dest);
    tcc_ir_pool_add(ir, src1);
    break;
  }
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  {
    int btype = IROP_BTYPE_INT32;
    int32_t dvr = map_callee_vreg(m, ir, o->dest_vreg);
    if (dvr < 0) return 0;
    IROperand dest = irop_make_vreg(dvr, btype);
    IROperand src1, src2;
    if (!build_caller_src1(ir, m, o, sim_env, btype, &src1)) return 0;
    if (!build_caller_src2(ir, m, o, sim_env, btype, &src2)) return 0;
    nq.operand_base = tcc_ir_pool_add(ir, dest);
    tcc_ir_pool_add(ir, src1);
    tcc_ir_pool_add(ir, src2);
    break;
  }
  default:
    return 0;
  }

  if (!switch_insert_before(ir, *pcall_idx, &nq))
    return 0;
  (*pcall_idx)++;
  return 1;
}

/* Re-run the simulation to repopulate the env up to (but not including) the
 * given replay point.  Needed because `tcc_ir_simulate_switch_func_ex`
 * doesn't expose its env, and we want the same known/unknown state when
 * emitting each replay op so that constant inlining is consistent. */
static int rebuild_sim_env(const TCCFuncSwitchSnapshot *snap, int64_t arg_value,
                           SwitchSimEnv *env)
{
  env->count = 0;
  if (!switch_sim_set(env, snap->param_vreg, arg_value, 1))
    return 0;
  int64_t flag_lhs = 0, flag_rhs = 0;
  int has_flags = 0;
  int pc = 0;
  int step_limit = 4 * snap->op_count + 64;
  while (pc >= 0 && pc < snap->op_count)
  {
    if (--step_limit < 0) return 0;
    const SwitchSimOp *o = &snap->ops[pc];
    switch (o->op)
    {
    case TCCIR_OP_NOP: pc++; break;
    case TCCIR_OP_ASSIGN:
      if (o->flags & 4)
        switch_sim_set(env, o->dest_vreg, 0, 0);
      else
      {
        int64_t v;
        int r = switch_sim_read_src(env, o, 1, &v);
        if (r == 1) switch_sim_set(env, o->dest_vreg, v, 1);
        else if (r == 2) switch_sim_set(env, o->dest_vreg, 0, 0);
        else return 0;
      }
      pc++;
      break;
    case TCCIR_OP_STORE: pc++; break;
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    {
      int64_t l = 0, r1 = 0;
      int rl = switch_sim_read_src(env, o, 1, &l);
      int rr = switch_sim_read_src(env, o, 2, &r1);
      if (rl == 0 || rr == 0) return 0;
      if (rl == 1 && rr == 1)
      {
        int64_t v = (o->op == TCCIR_OP_ADD) ? (l + r1) : (l - r1);
        switch_sim_set(env, o->dest_vreg, v, 1);
      }
      else switch_sim_set(env, o->dest_vreg, 0, 0);
      pc++;
      break;
    }
    case TCCIR_OP_CMP:
    {
      int64_t l, r;
      int rl = switch_sim_read_src(env, o, 1, &l);
      int rr = switch_sim_read_src(env, o, 2, &r);
      if (rl != 1 || rr != 1) return 0;
      flag_lhs = l; flag_rhs = r; has_flags = 1; pc++;
      break;
    }
    case TCCIR_OP_JUMP: pc = o->target; break;
    case TCCIR_OP_JUMPIF:
    {
      if (!has_flags) return 0;
      int32_t ls = (int32_t)flag_lhs, rs = (int32_t)flag_rhs;
      uint32_t lu = (uint32_t)flag_lhs, ru = (uint32_t)flag_rhs;
      int taken;
      switch ((int)o->src1_imm)
      {
      case TOK_EQ:  taken = (ls == rs); break;
      case TOK_NE:  taken = (ls != rs); break;
      case TOK_LT:  taken = (ls <  rs); break;
      case TOK_LE:  taken = (ls <= rs); break;
      case TOK_GT:  taken = (ls >  rs); break;
      case TOK_GE:  taken = (ls >= rs); break;
      case TOK_ULT: taken = (lu <  ru); break;
      case TOK_ULE: taken = (lu <= ru); break;
      case TOK_UGT: taken = (lu >  ru); break;
      case TOK_UGE: taken = (lu >= ru); break;
      default: return 0;
      }
      pc = taken ? o->target : pc + 1;
      break;
    }
    case TCCIR_OP_RETURNVALUE: return 1;
    default: return 0;
    }
  }
  return 0;
}

/* Caller-side: try to fold each FUNCCALLVAL to a switch-value function whose
 * single arg is a constant.  Mirrors tcc_ir_opt_const_call_replace's NOP-the-
 * params + ASSIGN rewrite so the existing const-prop / DCE cascade picks the
 * folded value up.  For functions whose body has bounded side effects
 * (load/arith/store of constant-symref globals) the side-effecting ops are
 * replayed inline before the result assignment. */
int tcc_ir_opt_switch_call_replace(TCCIRState *ir)
{
  if (!ir || !tcc_state || tcc_state->func_switch_cache_count == 0)
    return 0;

  int changes = 0;

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    IROperand callee_op = tcc_ir_op_get_src1(ir, q);
    Sym *callee = irop_get_sym_ex(ir, callee_op);
    if (!callee)
      continue;

    const TCCFuncSwitchSnapshot *snap = tcc_ir_lookup_switch_func(tcc_state, callee->v);
    if (!snap)
      continue;

    IROperand call_info = tcc_ir_op_get_src2(ir, q);
    int encoded = (int)irop_get_imm64_ex(ir, call_info);
    int call_id = TCCIR_DECODE_CALL_ID(encoded);
    int argc = TCCIR_DECODE_CALL_ARGC(encoded);
    if (argc != 1)
      continue;

    /* Locate the FUNCPARAMVAL for this call_id, param 0. */
    int param_idx = -1;
    IROperand arg_val = IROP_NONE;
    for (int j = i - 1; j >= 0; j--)
    {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      if (pq->op == TCCIR_OP_NOP)
        continue;
      if (pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID)
        break;
      IROperand ps2 = tcc_ir_op_get_src2(ir, pq);
      int enc = (int)irop_get_imm64_ex(ir, ps2);
      if (TCCIR_DECODE_CALL_ID(enc) != call_id)
        break;
      if (TCCIR_DECODE_PARAM_IDX(enc) == 0 && pq->op == TCCIR_OP_FUNCPARAMVAL)
      {
        param_idx = j;
        arg_val = tcc_ir_op_get_src1(ir, pq);
        break;
      }
    }

    if (param_idx < 0)
      continue;
    if (!irop_is_immediate(arg_val))
      continue;

    int64_t arg = irop_get_imm64_ex(ir, arg_val);
    int64_t ret_val;
    int ret_btype;
    int replay_indices[SWITCH_FUNC_SIM_MAX_REPLAY];
    int replay_count = 0;
    if (!tcc_ir_simulate_switch_func_ex(snap, arg, &ret_val, &ret_btype,
                                        replay_indices, &replay_count))
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);

    LOG_IR_GEN("OPTIMIZE: switch-IPC fold call %s(%lld) -> #%lld at i=%d (replay=%d)",
               get_tok_str(callee->v, NULL), (long long)arg, (long long)ret_val, i, replay_count);

    /* Emit replay ops just before the call site, in order.  Each insert
     * shifts the call site forward; track it explicitly. */
    int call_idx = i;
    if (replay_count > 0)
    {
      SwitchSimEnv sim_env;
      if (!rebuild_sim_env(snap, arg, &sim_env))
        continue;
      VregMap vmap = {0};
      vmap.count = 0;
      int ok = 1;
      for (int r = 0; r < replay_count; r++)
      {
        if (!emit_replay_op(ir, &vmap, snap, &sim_env, replay_indices[r], &call_idx))
        {
          ok = 0;
          break;
        }
      }
      if (!ok)
      {
        /* Partial-emit recovery is non-trivial — bail loudly so a buggy emit
         * doesn't silently corrupt the IR. */
        continue;
      }
    }

    /* Rewrite the (now-shifted) FUNCCALLVAL into the result ASSIGN. */
    q = &ir->compact_instructions[call_idx];
    q->op = TCCIR_OP_ASSIGN;
    if (ret_val == (int32_t)ret_val)
      tcc_ir_set_src1(ir, call_idx, irop_make_imm32(-1, (int32_t)ret_val, ret_btype));
    else
    {
      uint32_t pool_idx = tcc_ir_pool_add_i64(ir, ret_val);
      tcc_ir_set_src1(ir, call_idx, irop_make_i64(-1, pool_idx, ret_btype));
    }
    tcc_ir_set_src2(ir, call_idx, IROP_NONE);
    tcc_ir_set_dest(ir, call_idx, dest);

    /* NOP the matching FUNCPARAMVAL(s).  Scan backward from the call. */
    for (int j = call_idx - 1; j >= 0; j--)
    {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      if (pq->op == TCCIR_OP_NOP)
        continue;
      if (pq->op == TCCIR_OP_FUNCPARAMVAL || pq->op == TCCIR_OP_FUNCPARAMVOID)
      {
        IROperand ps2 = tcc_ir_op_get_src2(ir, pq);
        int enc = (int)irop_get_imm64_ex(ir, ps2);
        if (TCCIR_DECODE_CALL_ID(enc) == call_id)
          pq->op = TCCIR_OP_NOP;
        continue;
      }
      break;
    }

    /* Advance outer loop past inserted ops + the call site so we don't
     * re-process replay ops. */
    i = call_idx;
    changes++;
  }

  return changes;
}

/* ============================================================================
 * Back-Edge Phi Hoisting
 *
 * After register allocation, rotated loops often have this pattern at the
 * bottom:
 *
 *   [i-1]:  CMP rX, #limit
 *   [i]:    JUMPIF exit_target if COND    (forward)
 *   [i+1]:  ASSIGN rA = rB               (phi copy)
 *   ...
 *   [i+k]:  ASSIGN rC = rD               (phi copy)
 *   [i+k+1]: JUMP body_target            (backward, unconditional)
 *
 * This wastes one instruction per iteration (the unconditional JUMP).
 * We rewrite it to:
 *
 *   [i-1]:  CMP rX, #limit               (unchanged)
 *   [i]:    ASSIGN rA = rB               (moved before branch)
 *   ...
 *   [i+k-1]: ASSIGN rC = rD
 *   [i+k]:  JUMPIF body_target if !COND  (inverted, backward)
 *   [i+k+1]: NOP                         (was JUMP, now dead)
 *
 * The phi copies are safe to execute unconditionally because on the exit
 * path their destinations are dead (overwritten before next use).
 * ============================================================================ */


int tcc_ir_opt_param_addrof_const_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int max_par = ir->next_parameter;
  int max_tmp = ir->next_temporary_variable;
  int max_var = ir->next_local_variable;
  int changes = 0;

  if (max_par <= 0 || n == 0)
    return 0;

  /* Single-BB restriction (prototype): no real dominance analysis. */
  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF || op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE)
      return 0;
    if (ir->compact_instructions[i].is_jump_target && i > 0)
      return 0;
  }

  typedef struct
  {
    int lea_idx;
    int lea_tmp_pos;
    int store_idx;
    IROperand store_val;
    int disqualified;
  } ParamInfo;

  ParamInfo *pi = tcc_mallocz(sizeof(ParamInfo) * (max_par + 1));
  /* TEMP/VAR position → PARAM position it aliases (&P), or -1. */
  int *tmp_lea_param = tcc_malloc(sizeof(int) * (max_tmp + 1));
  int *var_lea_param = tcc_malloc(sizeof(int) * (max_var + 1));
  /* Single-def VAR tracking — VARs with one write are safe to chase as aliases. */
  uint8_t *var_def_count = tcc_mallocz(max_var + 1);
  /* Mark instructions that participate in the LEA->VAR->TEMP propagation chain
   * so Phase 2's "other use" detection ignores them. */
  uint8_t *chain_instr = tcc_mallocz((n + 7) / 8);

  for (int p = 0; p <= max_par; p++)
  {
    pi[p].lea_idx = -1;
    pi[p].store_idx = -1;
  }
  for (int t = 0; t <= max_tmp; t++)
    tmp_lea_param[t] = -1;
  for (int v = 0; v <= max_var; v++)
    var_lea_param[v] = -1;

  /* Count VAR defs (cap at 2 — we only need the single-def bit). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int vp = TCCIR_DECODE_VREG_POSITION(dvr);
    if (vp <= max_var && var_def_count[vp] < 2)
      var_def_count[vp]++;
  }

  /* Phase 1: LEAs T = &P. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LEA)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    int32_t svr = irop_get_vreg(src1);

    if (!src1.is_local || src1.is_lval)
      continue;
    if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_PARAM)
      continue;
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    int p = TCCIR_DECODE_VREG_POSITION(svr);
    int t = TCCIR_DECODE_VREG_POSITION(dvr);
    if (p > max_par || t > max_tmp)
      continue;

    if (pi[p].lea_idx == -1 && tmp_lea_param[t] == -1)
    {
      pi[p].lea_idx = i;
      pi[p].lea_tmp_pos = t;
      tmp_lea_param[t] = p;
    }
    else
    {
      if (pi[p].lea_idx >= 0)
        pi[p].disqualified = 1;
      if (tmp_lea_param[t] >= 0)
        pi[tmp_lea_param[t]].disqualified = 1;
    }
  }

  /* Phase 1b: propagate aliasing through STORE V=T and ASSIGN T'=V chains
   * (single-def V only).  Iterate to a fixed point. */
  int chain_changed;
  do
  {
    chain_changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      int32_t svr = irop_get_vreg(src1);

      /* STORE V <-- T  (dest is VAR, src1 is TEMP holding LEA result).
       * Note: VAR STORE dests carry is_lval=1 (the VAR's slot deref). */
      if (q->op == TCCIR_OP_STORE && dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR &&
          svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP && !src1.is_lval)
      {
        int vp = TCCIR_DECODE_VREG_POSITION(dvr);
        int tp = TCCIR_DECODE_VREG_POSITION(svr);
        if (vp <= max_var && tp <= max_tmp && var_def_count[vp] == 1 && tmp_lea_param[tp] >= 0 &&
            var_lea_param[vp] == -1)
        {
          var_lea_param[vp] = tmp_lea_param[tp];
          chain_instr[i / 8] |= (1 << (i % 8));
          chain_changed = 1;
        }
      }
      /* ASSIGN T' <-- V  (dest is TEMP non-deref, src1 is VAR with lea alias) */
      else if (q->op == TCCIR_OP_ASSIGN && !dest.is_lval && dvr >= 0 &&
               TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && svr >= 0 &&
               TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR)
      {
        int tp = TCCIR_DECODE_VREG_POSITION(dvr);
        int vp = TCCIR_DECODE_VREG_POSITION(svr);
        if (tp <= max_tmp && vp <= max_var && var_lea_param[vp] >= 0 && tmp_lea_param[tp] == -1)
        {
          tmp_lea_param[tp] = var_lea_param[vp];
          chain_instr[i / 8] |= (1 << (i % 8));
          chain_changed = 1;
        }
      }
    }
  } while (chain_changed);

  /* Phase 2: find the unique constant STORE through each T, and detect any
   * disqualifying out-of-chain use of T/V or pre-STORE use of P. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_LEA)
      continue;
    /* Skip instructions that are part of the LEA->VAR->TEMP propagation chain. */
    if (chain_instr[i / 8] & (1 << (i % 8)))
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int store_p = -1;
    if (q->op == TCCIR_OP_STORE && dest.is_lval)
    {
      int32_t dvr = irop_get_vreg(dest);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
      {
        int t = TCCIR_DECODE_VREG_POSITION(dvr);
        if (t <= max_tmp && tmp_lea_param[t] >= 0)
        {
          int p = tmp_lea_param[t];
          if (!pi[p].disqualified)
          {
            /* Accept IMM32 / I64 / SYMREF (link-time constant addresses) — but
             * reject 64-bit values until 64-bit operand rewriting is wired. */
            IROperand effective_val = src1;
            int sv_tag = irop_get_tag(src1);

            /* If src1 is a TEMP, look back through up to one TEMP→VAR→IMM
             * indirection to find an underlying IMM32/SYMREF.  Handles the
             * inlined `*pp = sym; return *p` shape SL-FWD leaves as
             * `STORE V<--SYM; LOAD T<--V; STORE *Tlea<--T`.  DCE removes
             * the now-dead helper ops after Phase 3 NOPs the outer STORE. */
            if (sv_tag == IROP_TAG_VREG)
            {
              int32_t s1vr = irop_get_vreg(src1);
              if (s1vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1vr) == TCCIR_VREG_TYPE_TEMP)
              {
                int t1 = TCCIR_DECODE_VREG_POSITION(s1vr);
                int def_idx = -1;
                int def_count = 0;
                for (int j = 0; j < i && def_count <= 1; j++)
                {
                  IRQuadCompact *r = &ir->compact_instructions[j];
                  if (r->op == TCCIR_OP_NOP || !irop_config[r->op].has_dest)
                    continue;
                  IROperand rdest = tcc_ir_op_get_dest(ir, r);
                  int32_t rdvr = irop_get_vreg(rdest);
                  if (rdvr >= 0 && TCCIR_DECODE_VREG_TYPE(rdvr) == TCCIR_VREG_TYPE_TEMP &&
                      TCCIR_DECODE_VREG_POSITION(rdvr) == t1)
                  {
                    def_idx = j;
                    def_count++;
                  }
                }
                if (def_count == 1)
                {
                  IRQuadCompact *def_q = &ir->compact_instructions[def_idx];
                  if (def_q->op == TCCIR_OP_ASSIGN || def_q->op == TCCIR_OP_LOAD)
                  {
                    IROperand def_src = tcc_ir_op_get_src1(ir, def_q);
                    int def_tag = irop_get_tag(def_src);
                    if ((def_tag == IROP_TAG_IMM32 || def_tag == IROP_TAG_SYMREF) &&
                        !irop_is_64bit(def_src))
                    {
                      effective_val = def_src;
                      effective_val.btype = src1.btype;
                      sv_tag = def_tag;
                    }
                    /* Second hop: TEMP loaded from a single-def VAR whose
                     * stored value is an IMM/SYMREF.  The VAR ref carries a
                     * STACKOFF tag but `irop_get_vreg` still decodes the
                     * VAR vreg. */
                    else
                    {
                      int32_t def_svr = irop_get_vreg(def_src);
                      if (def_svr >= 0 && TCCIR_DECODE_VREG_TYPE(def_svr) == TCCIR_VREG_TYPE_VAR)
                      {
                        int v1 = TCCIR_DECODE_VREG_POSITION(def_svr);
                        if (v1 <= max_var && var_def_count[v1] == 1)
                        {
                          for (int j = 0; j < def_idx; j++)
                          {
                            IRQuadCompact *r = &ir->compact_instructions[j];
                            if (r->op != TCCIR_OP_STORE && r->op != TCCIR_OP_ASSIGN)
                              continue;
                            IROperand rdest = tcc_ir_op_get_dest(ir, r);
                            int32_t rdvr = irop_get_vreg(rdest);
                            if (rdvr < 0 || TCCIR_DECODE_VREG_TYPE(rdvr) != TCCIR_VREG_TYPE_VAR ||
                                TCCIR_DECODE_VREG_POSITION(rdvr) != v1)
                              continue;
                            IROperand rsrc = tcc_ir_op_get_src1(ir, r);
                            int rtag = irop_get_tag(rsrc);
                            if ((rtag == IROP_TAG_IMM32 || rtag == IROP_TAG_SYMREF) &&
                                !irop_is_64bit(rsrc))
                            {
                              effective_val = rsrc;
                              effective_val.btype = src1.btype;
                              sv_tag = rtag;
                            }
                            break;
                          }
                        }
                      }
                    }
                  }
                }
              }
            }

            int sv_ok = (sv_tag == IROP_TAG_IMM32 || sv_tag == IROP_TAG_SYMREF) && !irop_is_64bit(effective_val);
            if (pi[p].store_idx == -1 && sv_ok)
            {
              pi[p].store_idx = i;
              pi[p].store_val = effective_val;
              store_p = p;
            }
            else
            {
              pi[p].disqualified = 1;
            }
          }
        }
      }
    }

    IROperand ops_arr[3];
    ops_arr[0] = dest;
    ops_arr[1] = src1;
    ops_arr[2] = src2;
    for (int oi = 0; oi < 3; oi++)
    {
      int32_t vr = irop_get_vreg(ops_arr[oi]);
      if (vr < 0)
        continue;
      int vt = TCCIR_DECODE_VREG_TYPE(vr);
      int vp = TCCIR_DECODE_VREG_POSITION(vr);

      if (vt == TCCIR_VREG_TYPE_TEMP && vp <= max_tmp && tmp_lea_param[vp] >= 0)
      {
        int p = tmp_lea_param[vp];
        if (p == store_p && oi == 0)
          continue;
        pi[p].disqualified = 1;
      }
      if (vt == TCCIR_VREG_TYPE_VAR && vp <= max_var && var_lea_param[vp] >= 0)
      {
        pi[var_lea_param[vp]].disqualified = 1;
      }

      if (vt == TCCIR_VREG_TYPE_PARAM && vp <= max_par && pi[vp].lea_idx >= 0)
      {
        if (pi[vp].store_idx == -1)
          pi[vp].disqualified = 1;
      }
    }
  }

  /* Phase 3: rewrite reads of P past the STORE, then NOP the LEA + STORE + chain. */
  for (int p = 0; p <= max_par; p++)
  {
    if (pi[p].disqualified || pi[p].lea_idx < 0 || pi[p].store_idx < 0)
      continue;

    int rewrote = 0;
    int store_idx = pi[p].store_idx;
    IROperand store_val = pi[p].store_val;

    for (int i = store_idx + 1; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);

      /* Reference forms to replace:
       *  - (STACKOFF, vreg=P, is_lval=1)  — "value at P's spill slot" via LOAD
       *  - (VREG,     vreg=P, is_lval=0)  — direct PARAM ref left by var_tmp_fwd
       * Both denote the post-STORE value of P; replace with the stored value. */
      int32_t s1vr = irop_get_vreg(src1);
      if (s1vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1vr) == TCCIR_VREG_TYPE_PARAM &&
          TCCIR_DECODE_VREG_POSITION(s1vr) == p && !(src1.is_lval && !src1.is_local))
      {
        IROperand newop = store_val;
        newop.btype = src1.btype;
        newop.is_lval = 0;
        tcc_ir_op_set_src1(ir, q, newop);
        if (q->op == TCCIR_OP_LOAD)
          q->op = TCCIR_OP_ASSIGN;
        rewrote++;
      }

      int32_t s2vr = irop_get_vreg(src2);
      if (s2vr >= 0 && TCCIR_DECODE_VREG_TYPE(s2vr) == TCCIR_VREG_TYPE_PARAM &&
          TCCIR_DECODE_VREG_POSITION(s2vr) == p && !(src2.is_lval && !src2.is_local))
      {
        IROperand newop = store_val;
        newop.btype = src2.btype;
        newop.is_lval = 0;
        tcc_ir_op_set_src2(ir, q, newop);
        rewrote++;
      }
    }

    if (rewrote > 0)
    {
      ir->compact_instructions[pi[p].lea_idx].op = TCCIR_OP_NOP;
      ir->compact_instructions[store_idx].op = TCCIR_OP_NOP;
      changes += rewrote + 2;

      /* NOP the chain instructions (STORE V=T, ASSIGN T'=V) so the codegen
       * does not need to keep their results alive. */
      for (int i = 0; i < n; i++)
      {
        if (!(chain_instr[i / 8] & (1 << (i % 8))))
          continue;
        IRQuadCompact *q = &ir->compact_instructions[i];
        /* Verify this chain instruction belongs to this P. */
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t dvr = irop_get_vreg(dest);
        int32_t svr = irop_get_vreg(src1);
        int belongs = 0;
        if (q->op == TCCIR_OP_STORE && dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
        {
          int vp = TCCIR_DECODE_VREG_POSITION(dvr);
          if (vp <= max_var && var_lea_param[vp] == p)
            belongs = 1;
        }
        else if (q->op == TCCIR_OP_ASSIGN && svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR)
        {
          int vp = TCCIR_DECODE_VREG_POSITION(svr);
          if (vp <= max_var && var_lea_param[vp] == p)
            belongs = 1;
        }
        if (belongs)
        {
          q->op = TCCIR_OP_NOP;
          changes++;
        }
      }

      int32_t pvr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, p);
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, pvr);
      if (interval)
        interval->addrtaken = 0;
    }
  }

  tcc_free(pi);
  tcc_free(tmp_lea_param);
  tcc_free(var_lea_param);
  tcc_free(var_def_count);
  tcc_free(chain_instr);
  return changes;
}

/* Analogue of tcc_ir_opt_param_addrof_const_fold but for local VARs.
 *
 * Pattern recognized (single BB):
 *
 *   STORE V <-- C0           ; init V to a constant
 *   T = &V                   ; address-of
 *   [chain: V_a = T; T' = V_a; ... aliasing of &V]
 *   STORE *T (or *T') <-- C1 ; modify through pointer, C1 a constant/SYMREF
 *                            ;   (with one TEMP→VAR→IMM look-through)
 *   ... = V                  ; one or more reads of V
 *
 * Transformation:
 *   - rewrite each post-modify read of V with C1
 *   - NOP the init STORE, LEA, modify STORE, and chain instructions
 *
 * Disqualified if V is read before the LEA, written more than once before the
 * LEA, written via anything other than the tracked STORE-through-T between
 * LEA and the modify, multi-LEA'd, or used outside the tracked pattern.
 */
int tcc_ir_opt_local_addrof_const_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int max_tmp = ir->next_temporary_variable;
  int max_var = ir->next_local_variable;
  int changes = 0;

  if (max_var <= 0 || n == 0)
    return 0;

  /* Single-BB restriction (parallel to the PARAM pass). */
  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF || op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE)
      return 0;
    if (ir->compact_instructions[i].is_jump_target && i > 0)
      return 0;
  }

  typedef struct
  {
    int init_idx;
    IROperand init_val;
    int lea_idx;
    int lea_tmp_pos;
    int store_idx;
    IROperand store_val;
    int disqualified;
  } VarInfo;

  VarInfo *vi = tcc_mallocz(sizeof(VarInfo) * (max_var + 1));
  /* TEMP pos -> target VAR pos, or -1 (LEA result or chained alias). */
  int *tmp_lea_var = tcc_malloc(sizeof(int) * (max_tmp + 1));
  /* VAR pos -> aliased target VAR pos (chain), or -1.  An alias V_a holds
   * the address &V_target, so reads of V_a yield &V_target's value. */
  int *var_lea_var = tcc_malloc(sizeof(int) * (max_var + 1));
  /* Single-def VAR tracking for safe chain traversal. */
  uint8_t *var_def_count = tcc_mallocz(max_var + 1);
  /* Bitmap of chain instructions (STORE V_a=T, ASSIGN T'=V_a). */
  uint8_t *chain_instr = tcc_mallocz((n + 7) / 8);

  for (int v = 0; v <= max_var; v++)
  {
    vi[v].init_idx = -1;
    vi[v].lea_idx = -1;
    vi[v].store_idx = -1;
    var_lea_var[v] = -1;
  }
  for (int t = 0; t <= max_tmp; t++)
    tmp_lea_var[t] = -1;

  /* Count VAR defs (cap at 2). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int vp = TCCIR_DECODE_VREG_POSITION(dvr);
    if (vp <= max_var && var_def_count[vp] < 2)
      var_def_count[vp]++;
  }

  /* Phase 1: locate LEA T = &V for local V. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LEA)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    int32_t svr = irop_get_vreg(src1);

    if (!src1.is_local || src1.is_lval)
      continue;
    if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
      continue;
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    int v = TCCIR_DECODE_VREG_POSITION(svr);
    int t = TCCIR_DECODE_VREG_POSITION(dvr);
    if (v > max_var || t > max_tmp)
      continue;

    if (vi[v].lea_idx == -1 && tmp_lea_var[t] == -1)
    {
      vi[v].lea_idx = i;
      vi[v].lea_tmp_pos = t;
      tmp_lea_var[t] = v;
    }
    else
    {
      if (vi[v].lea_idx >= 0)
        vi[v].disqualified = 1;
      if (tmp_lea_var[t] >= 0)
        vi[tmp_lea_var[t]].disqualified = 1;
    }
  }

  /* Phase 1b: propagate alias through STORE V_a=T and ASSIGN T'=V_a chains
   * (single-def V_a only).  V_a here is a chain alias holding &V_target,
   * distinct from V_target itself. */
  int chain_changed;
  do
  {
    chain_changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      int32_t svr = irop_get_vreg(src1);

      if (q->op == TCCIR_OP_STORE && dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR &&
          svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP && !src1.is_lval)
      {
        int vap = TCCIR_DECODE_VREG_POSITION(dvr);
        int tp = TCCIR_DECODE_VREG_POSITION(svr);
        if (vap <= max_var && tp <= max_tmp && var_def_count[vap] == 1 && tmp_lea_var[tp] >= 0 &&
            var_lea_var[vap] == -1 && vap != tmp_lea_var[tp])
        {
          var_lea_var[vap] = tmp_lea_var[tp];
          chain_instr[i / 8] |= (1 << (i % 8));
          chain_changed = 1;
        }
      }
      else if (q->op == TCCIR_OP_ASSIGN && !dest.is_lval && dvr >= 0 &&
               TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && svr >= 0 &&
               TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR)
      {
        int tp = TCCIR_DECODE_VREG_POSITION(dvr);
        int vap = TCCIR_DECODE_VREG_POSITION(svr);
        if (tp <= max_tmp && vap <= max_var && var_lea_var[vap] >= 0 && tmp_lea_var[tp] == -1)
        {
          tmp_lea_var[tp] = var_lea_var[vap];
          chain_instr[i / 8] |= (1 << (i % 8));
          chain_changed = 1;
        }
      }
    }
  } while (chain_changed);

  /* Phase 2: locate per-V init STORE (pre-LEA, const) and modify STORE
   * (post-LEA, through T or chained T'), with out-of-pattern use detection. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_LEA)
      continue;
    if (chain_instr[i / 8] & (1 << (i % 8)))
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int store_v = -1;

    /* STORE through T (or chained T') with constant value. */
    if (q->op == TCCIR_OP_STORE && dest.is_lval)
    {
      int32_t dvr = irop_get_vreg(dest);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
      {
        int t = TCCIR_DECODE_VREG_POSITION(dvr);
        if (t <= max_tmp && tmp_lea_var[t] >= 0)
        {
          int v = tmp_lea_var[t];
          if (!vi[v].disqualified)
          {
            IROperand effective_val = src1;
            int sv_tag = irop_get_tag(src1);

            /* Same TEMP→VAR→IMM look-through as the PARAM pass. */
            if (sv_tag == IROP_TAG_VREG)
            {
              int32_t s1vr = irop_get_vreg(src1);
              if (s1vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1vr) == TCCIR_VREG_TYPE_TEMP)
              {
                int t1 = TCCIR_DECODE_VREG_POSITION(s1vr);
                int def_idx = -1;
                int def_count = 0;
                for (int j = 0; j < i && def_count <= 1; j++)
                {
                  IRQuadCompact *r = &ir->compact_instructions[j];
                  if (r->op == TCCIR_OP_NOP || !irop_config[r->op].has_dest)
                    continue;
                  IROperand rdest = tcc_ir_op_get_dest(ir, r);
                  int32_t rdvr = irop_get_vreg(rdest);
                  if (rdvr >= 0 && TCCIR_DECODE_VREG_TYPE(rdvr) == TCCIR_VREG_TYPE_TEMP &&
                      TCCIR_DECODE_VREG_POSITION(rdvr) == t1)
                  {
                    def_idx = j;
                    def_count++;
                  }
                }
                if (def_count == 1)
                {
                  IRQuadCompact *def_q = &ir->compact_instructions[def_idx];
                  if (def_q->op == TCCIR_OP_ASSIGN || def_q->op == TCCIR_OP_LOAD)
                  {
                    IROperand def_src = tcc_ir_op_get_src1(ir, def_q);
                    int def_tag = irop_get_tag(def_src);
                    if ((def_tag == IROP_TAG_IMM32 || def_tag == IROP_TAG_SYMREF) &&
                        !irop_is_64bit(def_src))
                    {
                      effective_val = def_src;
                      effective_val.btype = src1.btype;
                      sv_tag = def_tag;
                    }
                    else
                    {
                      int32_t def_svr = irop_get_vreg(def_src);
                      if (def_svr >= 0 && TCCIR_DECODE_VREG_TYPE(def_svr) == TCCIR_VREG_TYPE_VAR)
                      {
                        int va = TCCIR_DECODE_VREG_POSITION(def_svr);
                        if (va <= max_var && var_def_count[va] == 1)
                        {
                          for (int j = 0; j < def_idx; j++)
                          {
                            IRQuadCompact *r = &ir->compact_instructions[j];
                            if (r->op != TCCIR_OP_STORE && r->op != TCCIR_OP_ASSIGN)
                              continue;
                            IROperand rdest = tcc_ir_op_get_dest(ir, r);
                            int32_t rdvr = irop_get_vreg(rdest);
                            if (rdvr < 0 || TCCIR_DECODE_VREG_TYPE(rdvr) != TCCIR_VREG_TYPE_VAR ||
                                TCCIR_DECODE_VREG_POSITION(rdvr) != va)
                              continue;
                            IROperand rsrc = tcc_ir_op_get_src1(ir, r);
                            int rtag = irop_get_tag(rsrc);
                            if ((rtag == IROP_TAG_IMM32 || rtag == IROP_TAG_SYMREF) &&
                                !irop_is_64bit(rsrc))
                            {
                              effective_val = rsrc;
                              effective_val.btype = src1.btype;
                              sv_tag = rtag;
                            }
                            break;
                          }
                        }
                      }
                    }
                  }
                }
              }
            }

            int sv_ok = (sv_tag == IROP_TAG_IMM32 || sv_tag == IROP_TAG_SYMREF) && !irop_is_64bit(effective_val);
            if (vi[v].store_idx == -1 && sv_ok)
            {
              vi[v].store_idx = i;
              vi[v].store_val = effective_val;
              store_v = v;
            }
            else
            {
              vi[v].disqualified = 1;
            }
          }
        }
      }
    }

    /* Detect the init STORE V <-- C0 (must be pre-LEA, single, constant). */
    if (q->op == TCCIR_OP_STORE && !dest.is_lval)
    {
      /* Plain VAR write (dest is VAR with !is_lval). */
    }
    if (q->op == TCCIR_OP_STORE)
    {
      int32_t dvr = irop_get_vreg(dest);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
      {
        int v = TCCIR_DECODE_VREG_POSITION(dvr);
        if (v <= max_var && vi[v].lea_idx >= 0)
        {
          /* Only pre-LEA, constant, exactly-one init is allowed. */
          if (i < vi[v].lea_idx)
          {
            int it_tag = irop_get_tag(src1);
            int it_ok = (it_tag == IROP_TAG_IMM32 || it_tag == IROP_TAG_SYMREF) && !irop_is_64bit(src1);
            if (vi[v].init_idx == -1 && it_ok)
            {
              vi[v].init_idx = i;
              vi[v].init_val = src1;
            }
            else
            {
              vi[v].disqualified = 1;
            }
          }
          else
          {
            /* Post-LEA direct write of V (not via the tracked T) — disqualify. */
            vi[v].disqualified = 1;
          }
        }
      }
    }

    /* Out-of-pattern use detection on all three operands. */
    IROperand ops_arr[3];
    ops_arr[0] = dest;
    ops_arr[1] = src1;
    ops_arr[2] = src2;
    for (int oi = 0; oi < 3; oi++)
    {
      int32_t vr = irop_get_vreg(ops_arr[oi]);
      if (vr < 0)
        continue;
      int vt = TCCIR_DECODE_VREG_TYPE(vr);
      int vp = TCCIR_DECODE_VREG_POSITION(vr);

      /* TEMP holding &V (or chained alias of it): allowed only as STORE-through
       * dest (oi==0 with is_lval).  Any other use disqualifies V. */
      if (vt == TCCIR_VREG_TYPE_TEMP && vp <= max_tmp && tmp_lea_var[vp] >= 0)
      {
        int v = tmp_lea_var[vp];
        if (v == store_v && oi == 0)
          continue;
        vi[v].disqualified = 1;
      }
      /* VAR chain alias V_a (var_lea_var[vp] set): any use outside the chain
       * disqualifies the target.  Chain instructions are skipped earlier. */
      if (vt == TCCIR_VREG_TYPE_VAR && vp <= max_var && var_lea_var[vp] >= 0)
      {
        vi[var_lea_var[vp]].disqualified = 1;
      }
      /* V itself: allowed as init STORE dest (already handled above) and as
       * post-STORE src.  Pre-modify reads disqualify. */
      if (vt == TCCIR_VREG_TYPE_VAR && vp <= max_var && vi[vp].lea_idx >= 0)
      {
        int v = vp;
        /* Skip the init STORE dest position. */
        if (q->op == TCCIR_OP_STORE && oi == 0 && i == vi[v].init_idx)
          continue;
        /* Reads (src1/src2) of V before the modify STORE disqualify. */
        if (oi >= 1)
        {
          if (vi[v].store_idx == -1 || i <= vi[v].store_idx)
            vi[v].disqualified = 1;
        }
      }
    }
  }

  /* Phase 3: rewrite reads of V past the STORE, then NOP init + LEA + STORE + chain. */
  for (int v = 0; v <= max_var; v++)
  {
    if (vi[v].disqualified)
      continue;
    if (vi[v].lea_idx < 0 || vi[v].store_idx < 0 || vi[v].init_idx < 0)
      continue;

    int rewrote = 0;
    int store_idx = vi[v].store_idx;
    IROperand store_val = vi[v].store_val;

    for (int i = store_idx + 1; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);

      int32_t s1vr = irop_get_vreg(src1);
      if (s1vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1vr) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(s1vr) == v && !(src1.is_lval && !src1.is_local))
      {
        IROperand newop = store_val;
        newop.btype = src1.btype;
        newop.is_lval = 0;
        tcc_ir_op_set_src1(ir, q, newop);
        if (q->op == TCCIR_OP_LOAD)
          q->op = TCCIR_OP_ASSIGN;
        rewrote++;
      }

      int32_t s2vr = irop_get_vreg(src2);
      if (s2vr >= 0 && TCCIR_DECODE_VREG_TYPE(s2vr) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(s2vr) == v && !(src2.is_lval && !src2.is_local))
      {
        IROperand newop = store_val;
        newop.btype = src2.btype;
        newop.is_lval = 0;
        tcc_ir_op_set_src2(ir, q, newop);
        rewrote++;
      }
    }

    if (rewrote > 0)
    {
      ir->compact_instructions[vi[v].init_idx].op = TCCIR_OP_NOP;
      ir->compact_instructions[vi[v].lea_idx].op = TCCIR_OP_NOP;
      ir->compact_instructions[store_idx].op = TCCIR_OP_NOP;
      changes += rewrote + 3;

      /* NOP chain instructions belonging to this V. */
      for (int i = 0; i < n; i++)
      {
        if (!(chain_instr[i / 8] & (1 << (i % 8))))
          continue;
        IRQuadCompact *q = &ir->compact_instructions[i];
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t dvr = irop_get_vreg(dest);
        int32_t svr = irop_get_vreg(src1);
        int belongs = 0;
        if (q->op == TCCIR_OP_STORE && dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
        {
          int vap = TCCIR_DECODE_VREG_POSITION(dvr);
          if (vap <= max_var && var_lea_var[vap] == v)
            belongs = 1;
        }
        else if (q->op == TCCIR_OP_ASSIGN && svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR)
        {
          int vap = TCCIR_DECODE_VREG_POSITION(svr);
          if (vap <= max_var && var_lea_var[vap] == v)
            belongs = 1;
        }
        if (belongs)
        {
          q->op = TCCIR_OP_NOP;
          changes++;
        }
      }

      int32_t vvr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, v);
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vvr);
      if (interval)
        interval->addrtaken = 0;
    }
  }

  tcc_free(vi);
  tcc_free(tmp_lea_var);
  tcc_free(var_lea_var);
  tcc_free(var_def_count);
  tcc_free(chain_instr);
  return changes;
}

int tcc_ir_opt_float_narrowing_ex(IROptCtx *ctx) { return tcc_ir_opt_float_narrowing(ctx->ir); }

/* tcc_ir_opt_pack64: peephole that detects the C-level
 *   `((uint64_t)hi << 32) | (uint64_t)lo`
 * pattern in the IR and collapses it to a single PACK64 op.
 *
 * IR shape before:
 *   i_zh: T_zh   = ZEXT src_hi             (u32 -> u64)
 *   i_sh: T_sh   = T_zh SHL #32            (u64)
 *   i_zl: T_zl   = ZEXT src_lo             (u32 -> u64)
 *   i_or: dest   = T_sh OR T_zl            (u64; operand order may be swapped)
 *
 * IR shape after:
 *   i_zh, i_sh, i_zl: NOP
 *   i_or: dest = PACK64(src_lo, src_hi)
 *
 * Requires each intermediate (T_zh, T_sh, T_zl) to be a TEMP with exactly
 * one use — otherwise NOP'ing the producer would lose data. */

/* tcc_ir_opt_assign_fuse: fuse a producer with its immediately-consuming
 * ASSIGN into a single op that writes directly to the ASSIGN's dest.
 *
 *   i_def:   T_new   = X OP Y                ; single-use TEMP
 *   i_asn:   T_final = T_new [ASSIGN]
 *
 *   →
 *
 *   i_def:   T_final = X OP Y                ; rewritten dest
 *   i_asn:   NOP
 *
 * This is a register-coalescing hint at the IR level: regalloc would
 * otherwise allocate T_new and T_final to different physical registers
 * and emit a MOV between them.  By rewriting the producer's dest to
 * T_final, we tell regalloc to place the result directly in T_final's
 * register, eliminating the MOV.
 *
 * Constraints:
 *   - T_new is a TEMP with exactly one use (the ASSIGN) and one def
 *     (the producer immediately preceding the ASSIGN, modulo NOPs).
 *   - The producer's op has a single dest (not STORE/STORE_INDEXED).
 *   - The ASSIGN and its producer share a basic block (no jump targets
 *     between them).
 *
 * Note: T_final may have multiple defs (e.g. one in each arm of an
 * if/else diamond).  That's fine — we're only changing where one of
 * those defs lives, not the value chain. */

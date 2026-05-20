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

static int ir_opt_eval_const_string_operand(TCCIRState *ir, IROperand op, int use_idx, IROperand *out, int depth)
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

static int ir_opt_fold_strcmp_result(const char *s1, const char *s2)
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

static int ir_opt_fold_strncmp_result(const char *s1, const char *s2, uint64_t n)
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

static int ir_opt_fold_memcmp_result(const char *s1, const char *s2, uint64_t n)
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
      if (q->op == TCCIR_OP_FUNCCALLVAL && ir_opt_get_call_param_operand(ir, i, 0, &arg0) &&
          ir_opt_eval_const_string(ir, arg0, i, &s1, 0))
      {
        ir_opt_nop_call_params(ir, i);
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int)strlen(s1), VT_INT));
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

    /* --- Simple redirects to __tcc_* helpers --- */
    {
      const char *helper = NULL;
      switch (id)
      {
      case STRBI_MEMMOVE:
        helper = "__tcc_memmove";
        break;
      case STRBI_BCOPY:
        helper = "__tcc_bcopy";
        break;
      case STRBI_MEMPCPY:
        helper = "__tcc_mempcpy";
        break;
      case STRBI_STRCAT:
        helper = "__tcc_strcat";
        break;
      case STRBI_STRCHR:
      case STRBI_INDEX:
        helper = "__tcc_strchr";
        break;
      case STRBI_STRCPY:
        helper = "__tcc_strcpy";
        break;
      case STRBI_STPCPY:
        helper = "__tcc_stpcpy";
        break;
      case STRBI_STPNCPY:
        helper = "__tcc_stpncpy";
        break;
      case STRBI_STRNLEN:
        helper = "__tcc_strnlen";
        break;
      case STRBI_STRPBRK:
        helper = "__tcc_strpbrk";
        break;
      case STRBI_STRRCHR:
      case STRBI_RINDEX:
        helper = "__tcc_strrchr";
        break;
      case STRBI_STRSTR:
        helper = "__tcc_strstr";
        break;
      case STRBI_STRCSPN:
        helper = "__tcc_strcspn";
        break;
      case STRBI_STRNCPY:
        helper = "__tcc_strncpy";
        break;
      case STRBI_STRNCAT:
        helper = "__tcc_strncat";
        break;
      default:
        break;
      }
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

    if (!ir_opt_pure_expr_equal(ir, p0, i, p1, i, 0))
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

    LOG_IR_GEN("OPTIMIZE: IPC replace call to %s with #%lld at i=%d", get_tok_str(callee->v, NULL), (long long)val, i);

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

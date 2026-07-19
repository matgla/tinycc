/*
 *  TCC IR - String builtin lowering (flat pass)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Relocated flat pass, Branch A [A*]; see docs/plan_legacy_flat_ir_ssa_retire.md */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

/* Lowering only: redirect string builtins to their __tcc_* helpers and
 * specialize memcmp(a,b,1).  Constant folding is ssa:const_string_fold's job —
 * it runs at the next pipeline slot and again in the SSA phase, and
 * resolve_str_builtin_id maps the __tcc_* names back, so a redirected call is
 * still foldable.  See docs/plan_ssa_const_string_fold.md. */
int tcc_ir_opt_const_string_calls(TCCIRState *ir)
{
  int changes = 0;

  if (!ir)
    return 0;

  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    Sym *callee;

    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      continue;

    callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;

    const char *name = get_tok_str(callee->v, NULL);
    const int id = resolve_str_builtin_id(callee->v, name);
    if (id == STRBI_UNKNOWN)
      continue;

    if (id == STRBI_STRLEN)
    {
      if (change_callee_sym_keep_type(ir, i, "__tcc_strlen"))
        changes++;
      continue;
    }

    /* Simple redirects to __tcc_* helpers via a static id->name table */
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

    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    /* memcmp(a,b,1): the one-byte helper takes two arguments. */
    if (id == STRBI_MEMCMP)
    {
      IROperand arg2;
      uint64_t n;

      if (!ir_opt_get_call_param_operand(ir, i, 2, &arg2) || !ir_opt_eval_const_u64(ir, arg2, i, &n, 0))
        continue;
      if (n != 1)
        continue;

      ir_opt_nop_call_param(ir, i, 2);
      if (!change_callee_sym(ir, i, "__tcc_memcmp1", VT_INT))
        continue;
      ir_opt_change_call_argc(ir, i, 2);
      changes++;
      continue;
    }

    if (id == STRBI_STRNCMP)
    {
      if (change_callee_sym(ir, i, "__tcc_strncmp", VT_INT))
        changes++;
      continue;
    }

    if (id == STRBI_STRCMP)
    {
      if (change_callee_sym_keep_type(ir, i, "__tcc_strcmp"))
        changes++;
      continue;
    }
  }

  return changes;
}

/*
 *  TCC IR - Function purity table, cache and inference (for LICM call hoisting)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "licm.h"
#include "opt.h"
#include "opt_utils.h"
#include "cfg.h"
#include "core.h"
#include "pool.h"
#include "vreg.h"
#include <string.h>

/* purity: 2 = PURE (reads memory), 3 = CONST (depends only on arguments) */
static struct
{
  const char *name;
  int purity;
} pure_func_table[] = {
    /* String functions - PURE (read memory) */
    {"strlen", 2},
    {"strcmp", 2},
    {"strncmp", 2},
    {"strchr", 2},
    {"strrchr", 2},
    {"strstr", 2},
    {"strpbrk", 2},
    {"strcspn", 2},
    {"strspn", 2},
    /* TCC-internal renamed variants (read-only string functions) */
    {"__tcc_strlen", 2},
    {"__tcc_strcmp", 2},
    {"__tcc_strnlen", 2},
    {"__tcc_strpbrk", 2},
    {"__tcc_strrchr", 2},
    {"__tcc_strstr", 2},
    {"__tcc_strcspn", 2},
    {"__tcc_memcmp1", 2},

    /* Memory functions - PURE */
    {"memcmp", 2},
    {"memchr", 2},

    /* Math functions - CONST (no memory reads, pure computation) */
    {"abs", 3},
    {"labs", 3},
    {"llabs", 3},
    {"fabs", 3},
    {"fabsf", 3},
    {"sqrt", 3},
    {"sqrtf", 3},
    {"sin", 3},
    {"sinf", 3},
    {"cos", 3},
    {"cosf", 3},
    {"tan", 3},
    {"tanf", 3},
    {"atan", 3},
    {"atanf", 3},
    {"atan2", 3},
    {"atan2f", 3},
    {"exp", 3},
    {"expf", 3},
    {"log", 3},
    {"logf", 3},
    {"log10", 3},
    {"log10f", 3},
    {"pow", 3},
    {"powf", 3},
    {"ceil", 3},
    {"ceilf", 3},
    {"floor", 3},
    {"floorf", 3},
    {"round", 3},
    {"roundf", 3},
    {"fmod", 3},
    {"fmodf", 3},
    {"modf", 3},
    {"modff", 3},

    /* Character classification - CONST */
    {"isalpha", 3},
    {"isdigit", 3},
    {"isalnum", 3},
    {"isspace", 3},
    {"isupper", 3},
    {"islower", 3},
    {"isprint", 3},
    {"isgraph", 3},
    {"ispunct", 3},
    {"iscntrl", 3},
    {"isxdigit", 3},
    {"tolower", 3},
    {"toupper", 3},
};

#define NUM_PURE_FUNCS (sizeof(pure_func_table) / sizeof(pure_func_table[0]))

void tcc_ir_cache_func_purity(TCCState *s, int func_token, TCCFuncPurity purity)
{
  if (!s || func_token < TOK_IDENT)
    return;

  for (int i = 0; i < s->func_purity_cache_count; i++)
  {
    if (s->func_purity_cache[i].token == func_token)
      return;
  }

  if (s->func_purity_cache_count >= FUNC_PURITY_CACHE_SIZE)
    return;

  s->func_purity_cache[s->func_purity_cache_count].token = func_token;
  s->func_purity_cache[s->func_purity_cache_count].purity = purity;
  s->func_purity_cache_count++;

  LOG_LICM("PURITY: Cached '%s' as %s", get_tok_str(func_token, NULL),
         purity == TCC_FUNC_PURITY_CONST  ? "CONST"
         : purity == TCC_FUNC_PURITY_PURE ? "PURE"
                                          : "IMPURE");
}

int tcc_ir_lookup_func_purity(TCCState *s, int func_token)
{
  if (!s || func_token < TOK_IDENT)
    return -1;

  for (int i = 0; i < s->func_purity_cache_count; i++)
  {
    if (s->func_purity_cache[i].token == func_token)
      return s->func_purity_cache[i].purity;
  }
  return -1;
}

/* Conservative: 0 for any address we cannot prove is stack/param */
static int is_stack_or_param_addr(TCCIRState *ir, IROperand op)
{
  int tag = irop_get_tag(op);

  if (tag == IROP_TAG_STACKOFF)
    return 1;

  if (tag == IROP_TAG_IMM32 || tag == IROP_TAG_I64)
    return 0;

  if (tag == IROP_TAG_VREG)
    return 0;

  if (tag == IROP_TAG_SYMREF)
    return 0;

  return 0;
}

TCCFuncPurity tcc_ir_infer_func_purity(TCCIRState *ir, Sym *func_sym)
{
  if (!ir || !func_sym)
    return TCC_FUNC_PURITY_IMPURE;

  const char *func_name = get_tok_str(func_sym->v, NULL);

  int is_const = 1;

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    switch (q->op)
    {
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (!is_stack_or_param_addr(ir, dest))
        {
          LOG_LICM("PURITY: Function '%s' is IMPURE: stores to non-stack memory", func_name);
          return TCC_FUNC_PURITY_IMPURE;
        }
      }
      break;

    case TCCIR_OP_LOAD:
    case TCCIR_OP_LOAD_INDEXED:
    case TCCIR_OP_LOAD_POSTINC:
      /* load from non-stack/param is still PURE, just not CONST */
      {
        IROperand src = tcc_ir_op_get_src1(ir, q);
        if (!is_stack_or_param_addr(ir, src))
        {
          is_const = 0;
        }
      }
      break;

    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        Sym *callee = irop_get_sym_ex(ir, src1);
        if (callee)
        {
          /* table/attributes only, never the cache: avoids infinite recursion */
          int callee_purity = TCC_FUNC_PURITY_UNKNOWN;

          const char *callee_name = get_tok_str(callee->v, NULL);
          for (size_t j = 0; j < sizeof(pure_func_table) / sizeof(pure_func_table[0]); j++)
          {
            if (strcmp(callee_name, pure_func_table[j].name) == 0)
            {
              callee_purity = pure_func_table[j].purity;
              break;
            }
          }

          /* soft-float / 64-bit AEABI helpers compute from their arguments
           * alone, so a function is not made impure by calling one */
          if (callee_purity == TCC_FUNC_PURITY_UNKNOWN && tcc_ir_is_pure_aeabi(callee_name))
            callee_purity = TCC_FUNC_PURITY_CONST;

          if (callee_purity == TCC_FUNC_PURITY_UNKNOWN)
          {
            int func_pure = callee->f.func_pure;
            int func_const = callee->f.func_const;
            if (callee->type.ref)
            {
              func_pure |= callee->type.ref->f.func_pure;
              func_const |= callee->type.ref->f.func_const;
            }
            if (func_const)
              callee_purity = TCC_FUNC_PURITY_CONST;
            else if (func_pure)
              callee_purity = TCC_FUNC_PURITY_PURE;
          }

          if (callee_purity == TCC_FUNC_PURITY_IMPURE || callee_purity == TCC_FUNC_PURITY_UNKNOWN)
          {
            LOG_LICM("PURITY: Function '%s' is IMPURE: calls impure function '%s'", func_name, callee_name);
            return TCC_FUNC_PURITY_IMPURE;
          }
          if (callee_purity == TCC_FUNC_PURITY_PURE)
            is_const = 0;
        }
        else
        {
          LOG_LICM("PURITY: Function '%s' is IMPURE: indirect call", func_name);
          return TCC_FUNC_PURITY_IMPURE;
        }
      }
      break;

    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_INIT_CHAIN_SLOT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BLOCK_COPY:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_IJUMP:
      LOG_LICM("PURITY: Function '%s' is IMPURE: opaque side-effecting op %d", func_name, q->op);
      return TCC_FUNC_PURITY_IMPURE;

    case TCCIR_OP_VLA_ALLOC:
      LOG_LICM("PURITY: Function '%s' is IMPURE: VLA allocation", func_name);
      return TCC_FUNC_PURITY_IMPURE;

    default:
      break;
    }
  }

  TCCFuncPurity result = is_const ? TCC_FUNC_PURITY_CONST : TCC_FUNC_PURITY_PURE;
  LOG_LICM("PURITY: Function '%s' inferred as %s", func_name, result == TCC_FUNC_PURITY_CONST ? "CONST" : "PURE");
  return result;
}

int tcc_ir_get_func_purity(TCCIRState *ir, Sym *sym)
{
  if (!sym)
    return TCC_FUNC_PURITY_UNKNOWN;

  /* must mask VT_BTYPE: a bare `t & VT_FUNC` passes non-function types (VT_INT & VT_FUNC != 0) */
  if ((sym->type.t & VT_BTYPE) != VT_FUNC)
    return TCC_FUNC_PURITY_IMPURE;

  const char *func_name = get_tok_str(sym->v, NULL);
  if (!func_name)
    return TCC_FUNC_PURITY_UNKNOWN;

  /* declarations carry pure/const on sym->type.ref->f, not sym->f: check both */
  int func_pure = sym->f.func_pure;
  int func_const = sym->f.func_const;
  int func_noreturn = sym->f.func_noreturn;

  if (sym->type.ref)
  {
    func_pure |= sym->type.ref->f.func_pure;
    func_const |= sym->type.ref->f.func_const;
    func_noreturn |= sym->type.ref->f.func_noreturn;
  }

  LOG_LICM("Checking purity for function '%s': func_pure=%d, func_const=%d", func_name, func_pure, func_const);

  for (size_t i = 0; i < NUM_PURE_FUNCS; i++)
  {
    if (strcmp(func_name, pure_func_table[i].name) == 0)
    {
      LOG_LICM("Found '%s' in pure function table with purity=%d", func_name, pure_func_table[i].purity);
      return pure_func_table[i].purity;
    }
  }

  /* Soft-float / 64-bit AEABI helpers compute from their arguments alone: they
   * touch no memory, so they are CONST.  Without this the LICM/DCE purity query
   * classifies every `__aeabi_dmul` as IMPURE and a loop-invariant soft-float
   * expression is recomputed on every iteration. */
  if (tcc_ir_is_pure_aeabi(func_name))
  {
    LOG_LICM("Function '%s' is a pure AEABI helper (CONST)", func_name);
    return TCC_FUNC_PURITY_CONST;
  }

  /* noreturn functions exit or loop forever, never pure */
  if (func_noreturn)
  {
    return TCC_FUNC_PURITY_IMPURE;
  }

  if (func_const)
  {
    LOG_LICM("Function '%s' has func_const attribute", func_name);
    return TCC_FUNC_PURITY_CONST;
  }

  if (func_pure)
  {
    LOG_LICM("Function '%s' has func_pure attribute", func_name);
    return TCC_FUNC_PURITY_PURE;
  }

  /* inferred purity of same-TU functions */
  extern TCCState *tcc_state;
  if (tcc_state)
  {
    int cached = tcc_ir_lookup_func_purity(tcc_state, sym->v);
    if (cached >= 0)
    {
      LOG_LICM("Found cached purity for '%s': %d", func_name, cached);
      return cached;
    }
  }

  LOG_LICM("Function '%s' is unknown, marking as IMPURE", func_name);
  return TCC_FUNC_PURITY_IMPURE;
}

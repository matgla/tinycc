/*
 *  TCC IR - Helper-call purity tables
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

int ir_opt_is_memcpy_or_memmove_name(const char *name)
{
  return name &&
         (strcmp(name, "memcpy") == 0 || strcmp(name, "memmove") == 0 ||
          strcmp(name, "__aeabi_memcpy") == 0 || strcmp(name, "__aeabi_memcpy4") == 0 ||
          strcmp(name, "__aeabi_memcpy8") == 0 || strcmp(name, "__aeabi_memmove") == 0 ||
          strcmp(name, "__aeabi_memmove4") == 0 || strcmp(name, "__aeabi_memmove8") == 0);
}

int tcc_ir_is_pure_aeabi(const char *name)
{
  if (!name || name[0] != '_' || name[1] != '_')
    return 0;
  if (strcmp(name, "__aeabi_lcmp") == 0 || strcmp(name, "__aeabi_ulcmp") == 0)
    return 1;
  /* div/mod trap only on a zero divisor, which is UB */
  if (strcmp(name, "__aeabi_lmul") == 0 || strcmp(name, "__aeabi_ldivmod") == 0 ||
      strcmp(name, "__aeabi_uldivmod") == 0 || strcmp(name, "__aeabi_lmod") == 0 ||
      strcmp(name, "__aeabi_ulmod") == 0)
    return 1;
  if (strcmp(name, "__aeabi_llsl") == 0 || strcmp(name, "__aeabi_llsr") == 0 || strcmp(name, "__aeabi_lasr") == 0)
    return 1;
  if (strcmp(name, "__aeabi_dadd") == 0 || strcmp(name, "__aeabi_dsub") == 0 || strcmp(name, "__aeabi_dmul") == 0 ||
      strcmp(name, "__aeabi_ddiv") == 0 || strcmp(name, "__aeabi_fadd") == 0 || strcmp(name, "__aeabi_fsub") == 0 ||
      strcmp(name, "__aeabi_fmul") == 0 || strcmp(name, "__aeabi_fdiv") == 0)
    return 1;
  if (strcmp(name, "__aeabi_dcmpeq") == 0 || strcmp(name, "__aeabi_dcmplt") == 0 ||
      strcmp(name, "__aeabi_dcmple") == 0 || strcmp(name, "__aeabi_dcmpge") == 0 ||
      strcmp(name, "__aeabi_dcmpgt") == 0 || strcmp(name, "__aeabi_dcmpun") == 0 ||
      strcmp(name, "__aeabi_fcmpeq") == 0 || strcmp(name, "__aeabi_fcmplt") == 0 ||
      strcmp(name, "__aeabi_fcmple") == 0 || strcmp(name, "__aeabi_fcmpge") == 0 ||
      strcmp(name, "__aeabi_fcmpgt") == 0 || strcmp(name, "__aeabi_fcmpun") == 0)
    return 1;
  if (strcmp(name, "__aeabi_f2d") == 0 || strcmp(name, "__aeabi_d2f") == 0 || strcmp(name, "__aeabi_i2d") == 0 ||
      strcmp(name, "__aeabi_i2f") == 0 || strcmp(name, "__aeabi_ui2d") == 0 || strcmp(name, "__aeabi_ui2f") == 0 ||
      strcmp(name, "__aeabi_d2iz") == 0 || strcmp(name, "__aeabi_d2uiz") == 0 || strcmp(name, "__aeabi_f2iz") == 0 ||
      strcmp(name, "__aeabi_f2uiz") == 0 || strcmp(name, "__aeabi_l2d") == 0 || strcmp(name, "__aeabi_l2f") == 0 ||
      strcmp(name, "__aeabi_ul2d") == 0 || strcmp(name, "__aeabi_ul2f") == 0 || strcmp(name, "__aeabi_d2lz") == 0 ||
      strcmp(name, "__aeabi_d2ulz") == 0 || strcmp(name, "__aeabi_f2lz") == 0 || strcmp(name, "__aeabi_f2ulz") == 0)
    return 1;
  if (strcmp(name, "__bswapsi2") == 0 || strcmp(name, "__bswapdi3") == 0)
    return 1;
  return 0;
}

/* "const" helpers: touch no memory, so two calls with equal arguments are interchangeable. */
int ir_opt_is_pure_helper_name(const char *name)
{
  if (!name)
    return 0;

  return strcmp(name, "isnan") == 0 || strcmp(name, "__isnan") == 0 || strcmp(name, "__isnanf") == 0 ||
         strcmp(name, "__aeabi_f2d") == 0 || strcmp(name, "__aeabi_d2f") == 0;
}

/* "pure" helpers: read memory through their pointer args, so only an unused result is dead. */
/* Never CSE two of these calls -- memory may have changed in between. */
int ir_opt_is_readonly_str_helper_name(const char *name)
{
  if (!name)
    return 0;

  return strcmp(name, "__tcc_strcmp") == 0 || strcmp(name, "__tcc_strncmp") == 0 ||
         strcmp(name, "__tcc_strlen") == 0 || strcmp(name, "__tcc_strnlen") == 0 ||
         strcmp(name, "__tcc_strchr") == 0 || strcmp(name, "__tcc_strrchr") == 0 ||
         strcmp(name, "__tcc_strpbrk") == 0 || strcmp(name, "__tcc_strstr") == 0 ||
         strcmp(name, "__tcc_strcspn") == 0;
}

int ir_opt_is_flag_cmp_helper_name(const char *name)
{
  if (!name)
    return 0;

  return strcmp(name, "__aeabi_cfcmple") == 0 || strcmp(name, "__aeabi_cdcmple") == 0;
}

int ir_opt_is_pure_fallthrough_instruction(TCCIRState *ir, int idx)
{
  IRQuadCompact *q;
  Sym *callee;
  const char *name;

  if (!ir || idx < 0 || idx >= ir->next_instruction_index)
    return 0;

  q = &ir->compact_instructions[idx];
  switch (q->op)
  {
  case TCCIR_OP_NOP:
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_OR:
  case TCCIR_OP_AND:
  case TCCIR_OP_XOR:
  case TCCIR_OP_BOOL_OR:
  case TCCIR_OP_BOOL_AND:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
    return 1;
  case TCCIR_OP_FUNCCALLVAL:
    callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      return 0;
    name = get_tok_str(callee->v, NULL);
    return ir_opt_is_pure_helper_name(name);
  default:
    return 0;
  }
}

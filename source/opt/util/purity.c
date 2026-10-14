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

int ir_opt_name_in(const char *name, const char *list)
{
  /* First characters first: these lists are asked about every call
   * instruction in several passes, and most names match no entry. */
  for (const char *k = list; *k; k += strlen(k) + 1)
    if (k[0] == name[0] && strcmp(name, k) == 0)
      return 1;
  return 0;
}

int ir_opt_is_memcpy_or_memmove_name(const char *name)
{
  return name && ir_opt_name_in(name, "memcpy\0memmove\0__aeabi_memcpy\0__aeabi_memcpy4\0__aeabi_memcpy8\0"
                                      "__aeabi_memmove\0__aeabi_memmove4\0__aeabi_memmove8\0");
}

int ir_opt_memset_params(const char *name, int *size_idx, int *fill_idx)
{
  if (!name)
    return 0;
  if (strcmp(name, "memset") == 0)
  {
    *size_idx = 2;
    *fill_idx = 1;
    return 1;
  }
  if (strcmp(name, "__aeabi_memset") == 0)
  {
    *size_idx = 1;
    *fill_idx = 2;
    return 1;
  }
  return 0;
}

int tcc_ir_is_pure_aeabi(const char *name)
{
  if (!name || name[0] != '_' || name[1] != '_')
    return 0;
  if (strncmp(name, "__aeabi_", 8) == 0)
    return ir_opt_name_in(name + 8,
                          /* compares; div/mod trap only on a zero divisor, which is UB */
                          "lcmp\0ulcmp\0lmul\0ldivmod\0uldivmod\0lmod\0ulmod\0llsl\0llsr\0lasr\0"
                          "dadd\0dsub\0dmul\0ddiv\0fadd\0fsub\0fmul\0fdiv\0"
                          "dcmpeq\0dcmplt\0dcmple\0dcmpge\0dcmpgt\0dcmpun\0"
                          "fcmpeq\0fcmplt\0fcmple\0fcmpge\0fcmpgt\0fcmpun\0"
                          "f2d\0d2f\0i2d\0i2f\0ui2d\0ui2f\0d2iz\0d2uiz\0f2iz\0f2uiz\0l2d\0l2f\0ul2d\0ul2f\0"
                          "d2lz\0d2ulz\0f2lz\0f2ulz\0");
  return ir_opt_name_in(name + 2, "bswapsi2\0bswapdi3\0");
}

/* "const" helpers: touch no memory, so two calls with equal arguments are interchangeable. */
int ir_opt_is_pure_helper_name(const char *name)
{
  if (!name)
    return 0;

  return ir_opt_name_in(name, "isnan\0__isnan\0__isnanf\0__aeabi_f2d\0__aeabi_d2f\0");
}

/* "pure" helpers: read memory through their pointer args, so only an unused result is dead. */
/* Never CSE two of these calls -- memory may have changed in between. */
int ir_opt_is_readonly_str_helper_name(const char *name)
{
  if (!name)
    return 0;

  return ir_opt_name_in(name, "__tcc_strcmp\0__tcc_strncmp\0__tcc_strlen\0__tcc_strnlen\0__tcc_strchr\0"
                              "__tcc_strrchr\0__tcc_strpbrk\0__tcc_strstr\0__tcc_strcspn\0");
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
    callee = tcc_ir_op_src1_sym(ir, q);
    if (!callee)
      return 0;
    name = get_tok_str(callee->v, NULL);
    return ir_opt_is_pure_helper_name(name);
  default:
    return 0;
  }
}

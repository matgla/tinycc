/*
 *  TCC IR - Type Helpers Implementation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

/* ============================================================================
 * Type Classification
 * ============================================================================ */

/* Returns true if type is float */
int tcc_ir_type_is_float(int t)
{
  int bt = t & VT_BTYPE;
  return bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE || bt == VT_QFLOAT;
}

/* Returns true if type is double */
int tcc_ir_type_is_double(int t)
{
  int bt = t & VT_BTYPE;
  return bt == VT_DOUBLE || bt == VT_LDOUBLE;
}

/* Returns true if type is 64-bit (double, ldouble, or long long) */
int tcc_ir_type_is_64bit(int t)
{
  int bt = t & VT_BTYPE;
  return bt == VT_DOUBLE || bt == VT_LDOUBLE || bt == VT_LLONG;
}

/* Returns true if type is floating point */
int tcc_ir_type_is_fp(int t)
{
  int bt = t & VT_BTYPE;
  return bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE || bt == VT_QFLOAT;
}

/* Returns true if type is integer */
int tcc_ir_type_is_int(int t)
{
  return !tcc_ir_type_is_fp(t);
}

/* Returns true if type is pointer */
int tcc_ir_type_is_ptr(int t)
{
  return (t & VT_BTYPE) == VT_PTR;
}

/* Returns true if type is struct */
int tcc_ir_type_is_struct(int t)
{
  return (t & VT_BTYPE) == VT_STRUCT;
}

/* Returns true if type is void */
int tcc_ir_type_is_void(int t)
{
  return (t & VT_BTYPE) == VT_VOID;
}

/* Returns true if type is unsigned */
int tcc_ir_type_is_unsigned(int t)
{
  return (t & VT_UNSIGNED) != 0;
}

/* Returns true if type is signed */
int tcc_ir_type_is_signed(int t)
{
  return !tcc_ir_type_is_unsigned(t) && !tcc_ir_type_is_fp(t);
}

/* Returns true if type is boolean (from comparison) */
int tcc_ir_type_is_bool(int t)
{
  return (t & VT_CMP) != 0;
}

/* ============================================================================
 * SValue Type Helpers
 * ============================================================================ */

/* Check if an SValue operand is spilled (in memory) */
int tcc_ir_type_spilled(SValue *sv)
{
  return (sv->pr0_reg == PREG_REG_NONE) || sv->pr0_spilled;
}

/* Returns true if type is 64-bit */
int tcc_ir_type_64bit(int t)
{
  return tcc_ir_type_is_64bit(t);
}

/* ============================================================================
 * Legacy API (for compatibility during migration)
 * ============================================================================ */

/* Check if an SValue operand is spilled (in memory) - legacy name */
int tcc_ir_is_spilled(SValue *sv)
{
  return tcc_ir_type_spilled(sv);
}

/* Returns true if type is 64-bit (double, ldouble, or long long) - legacy name */
int tcc_ir_is_64bit(int t)
{
  return tcc_ir_type_is_64bit(t);
}

/* ============================================================================
 * FPU Operation Detection
 * ============================================================================ */

/* Returns true if operation requires FPU */
int tcc_ir_type_op_needs_fpu(TccIrOp op)
{
  switch (op)
  {
    case TCCIR_OP_FADD:
    case TCCIR_OP_FSUB:
    case TCCIR_OP_FMUL:
    case TCCIR_OP_FDIV:
    case TCCIR_OP_FNEG:
    case TCCIR_OP_FCMP:
    case TCCIR_OP_CVT_FTOF:
    case TCCIR_OP_CVT_ITOF:
    case TCCIR_OP_CVT_FTOI:
      return 1;
    default:
      return 0;
  }
}

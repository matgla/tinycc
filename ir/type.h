/*
 *  TCC IR - Type Helpers
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_TYPE_H
#define TCC_IR_TYPE_H

#include "../tcc.h"

/* ============================================================================
 * Type Classification
 * ============================================================================ */

/* Returns true if type is float */
static inline int tcc_ir_type_is_float(int t)
{
  return (t & VT_BTYPE) == VT_FLOAT;
}

/* Returns true if type is double */
static inline int tcc_ir_type_is_double(int t)
{
  int btype = t & VT_BTYPE;
  return btype == VT_DOUBLE || btype == VT_LDOUBLE;
}

/* Returns true if type is 64-bit (double, ldouble, or long long) */
static inline int tcc_ir_type_is_64bit(int t)
{
  int btype = t & VT_BTYPE;
  return btype == VT_DOUBLE || btype == VT_LDOUBLE || btype == VT_LLONG;
}

/* Returns true if type is floating point (float or double) */
static inline int tcc_ir_type_is_fp(int t)
{
  int btype = t & VT_BTYPE;
  return btype == VT_FLOAT || btype == VT_DOUBLE || btype == VT_LDOUBLE;
}

/* Returns true if type is integer (not floating point) */
static inline int tcc_ir_type_is_int(int t)
{
  return !tcc_ir_type_is_fp(t);
}

/* Returns true if type is pointer */
static inline int tcc_ir_type_is_ptr(int t)
{
  return (t & VT_BTYPE) == VT_PTR;
}

/* Returns true if type is struct */
static inline int tcc_ir_type_is_struct(int t)
{
  return (t & VT_BTYPE) == VT_STRUCT;
}

/* Returns true if type is void */
static inline int tcc_ir_type_is_void(int t)
{
  return (t & VT_BTYPE) == VT_VOID;
}

/* Returns true if type is unsigned */
static inline int tcc_ir_type_is_unsigned(int t)
{
  return (t & VT_UNSIGNED) != 0;
}

/* Returns true if type is signed */
static inline int tcc_ir_type_is_signed(int t)
{
  return !tcc_ir_type_is_unsigned(t) && !tcc_ir_type_is_fp(t);
}

/* Returns true if type is boolean (from comparison) */
static inline int tcc_ir_type_is_bool(int t)
{
  return (t & VT_CMP) != 0;
}

/* ============================================================================
 * SValue Type Helpers
 * ============================================================================ */

/* Check if an SValue operand is spilled (in memory) */
int tcc_ir_type_spilled(SValue *sv);

/* Returns true if SValue type is 64-bit */
int tcc_ir_type_64bit(int t);

/* ============================================================================
 * FPU Operation Detection
 * ============================================================================ */

/* Returns true if operation requires FPU */
int tcc_ir_type_op_needs_fpu(TccIrOp op);

#endif /* TCC_IR_TYPE_H */

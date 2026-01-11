/*
 * TinyCC Type Traits and Utilities
 *
 * This file provides common type checking and manipulation utilities
 * used across the compiler.
 */

#ifndef TCCTYPE_H
#define TCCTYPE_H

#include <stdint.h>

/* Forward declarations to avoid circular dependencies */
#ifndef VT_BTYPE
/* If VT_BTYPE is not defined, this header is included too early.
 * These definitions should come from tcc.h */
#endif

/**
 * Check if a type is 64-bit (long long, double, or long double)
 *
 * @param t Type value (typically from CType.t or SValue.type.t)
 * @return Non-zero if type is 64-bit, zero otherwise
 */
static inline int tcc_is_64bit_type(int t)
{
  int bt = t & VT_BTYPE;
  return (bt == VT_DOUBLE || bt == VT_LDOUBLE || bt == VT_LLONG);
}

/**
 * Check if a type is a floating point type
 *
 * @param t Type value
 * @return Non-zero if type is float/double/ldouble, zero otherwise
 */
static inline int tcc_is_float_type(int t)
{
  int bt = t & VT_BTYPE;
  return (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE);
}

/**
 * Check if a type is an integer type
 *
 * @param t Type value
 * @return Non-zero if type is an integer, zero otherwise
 */
static inline int tcc_is_integer_type(int t)
{
  int bt = t & VT_BTYPE;
  return (bt == VT_INT || bt == VT_BYTE || bt == VT_SHORT ||
          bt == VT_LLONG || bt == VT_BOOL || bt == VT_LONG);
}

/**
 * Check if a type is a pointer type
 *
 * @param t Type value
 * @return Non-zero if type is a pointer, zero otherwise
 */
static inline int tcc_is_pointer_type(int t)
{
  return (t & VT_BTYPE) == VT_PTR;
}

/**
 * Check if a type is a struct or union
 *
 * @param t Type value
 * @return Non-zero if type is struct/union, zero otherwise
 */
static inline int tcc_is_struct_type(int t)
{
  int bt = t & VT_BTYPE;
  return (bt == VT_STRUCT);
}

/**
 * Get the size of a basic type in bytes
 *
 * @param t Type value
 * @return Size in bytes, or -1 for unknown/complex types
 */
static inline int tcc_get_basic_type_size(int t)
{
  int bt = t & VT_BTYPE;
  switch (bt)
  {
  case VT_BYTE:
  case VT_BOOL:
    return 1;
  case VT_SHORT:
    return 2;
  case VT_INT:
  case VT_FLOAT:
  case VT_PTR:
  case VT_FUNC:
    return 4;
  case VT_LLONG:
  case VT_DOUBLE:
  case VT_LDOUBLE:
    return 8;
  case VT_STRUCT:
    return -1; /* Size must be computed from Sym */
  default:
    return -1;
  }
}

/**
 * Check if a type requires 8-byte alignment
 *
 * @param t Type value
 * @return Non-zero if 8-byte alignment required, zero otherwise
 */
static inline int tcc_requires_8byte_align(int t)
{
  return tcc_is_64bit_type(t);
}

#endif /* TCCTYPE_H */

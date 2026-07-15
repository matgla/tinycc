/*
 *  TCC IR - Type Helpers
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

/* ============================================================================
 * Type Classification - Implemented in type.c
 * ============================================================================ */

/* Returns true if type is float */
int tcc_ir_type_is_float(int t);

/* Returns true if type is double */
int tcc_ir_type_is_double(int t);

/* Returns true if type is 64-bit (double, ldouble, or long long) */
int tcc_ir_type_is_64bit(int t);

/* Returns true if type is floating point (float or double) */
int tcc_ir_type_is_fp(int t);

/* Returns true if type is integer (not floating point) */
int tcc_ir_type_is_int(int t);

/* Returns true if type is pointer */
int tcc_ir_type_is_ptr(int t);

/* Returns true if type is struct */
int tcc_ir_type_is_struct(int t);

/* Returns true if type is void */
int tcc_ir_type_is_void(int t);

/* Returns true if type is unsigned */
int tcc_ir_type_is_unsigned(int t);

/* Returns true if type is signed */
int tcc_ir_type_is_signed(int t);

/* Returns true if type is boolean (from comparison) */
int tcc_ir_type_is_bool(int t);

/* ============================================================================
 * SValue Type Helpers
 * ============================================================================ */

/* Check if an SValue operand is spilled (in memory) */
int tcc_ir_type_spilled(struct SValue *sv);

/* Returns true if SValue type is 64-bit */
int tcc_ir_type_64bit(int t);

/* Check if an SValue operand is spilled (legacy name) */
int tcc_ir_is_spilled(struct SValue *sv);

/* Returns true if type is 64-bit (legacy name) */
int tcc_ir_is_64bit(int t);

/* ============================================================================
 * FPU Operation Detection
 * ============================================================================ */

/* Returns true if operation requires FPU */
int tcc_ir_type_op_needs_fpu(TccIrOp op);

/* Check if an SValue operand needs dereferencing to get the actual value.
 * Returns true when the operand holds an address that must be loaded through. */
bool tcc_ir_operand_needs_dereference(struct SValue *sv);


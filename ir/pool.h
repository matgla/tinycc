/*
 *  TCC IR - Operand Pool Management
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_POOL_H
#define TCC_IR_POOL_H

#include "operand.h"

struct TCCIRState;

/* ============================================================================
 * Pool Initialization
 * ============================================================================ */

/* Initialize all operand pools */
void tcc_ir_pool_init(struct TCCIRState *ir);

/* Free all operand pools */
void tcc_ir_pool_free(struct TCCIRState *ir);

/* ============================================================================
 * IROperand Pool Operations
 * ============================================================================ */

/* Add IROperand to pool, return index */
int tcc_ir_pool_add(struct TCCIRState *ir, IROperand irop);

/* Get IROperand from pool by index */
IROperand tcc_ir_pool_get(struct TCCIRState *ir, int index);

/* Set IROperand in pool by index */
void tcc_ir_pool_set(struct TCCIRState *ir, int index, IROperand irop);

/* Ensure pool has capacity for n more elements */
void tcc_ir_pool_ensure(struct TCCIRState *ir, int n);

/* ============================================================================
 * Specialized Pool Operations
 * ============================================================================ */

/* Add int64 constant to pool, return index */
int tcc_ir_pool_i64_add(struct TCCIRState *ir, int64_t val);

/* Get int64 constant from pool */
int64_t tcc_ir_pool_i64_get(struct TCCIRState *ir, int index);

/* Add float64 (bits) to pool, return index */
int tcc_ir_pool_f64_add(struct TCCIRState *ir, uint64_t bits);

/* Get float64 bits from pool */
uint64_t tcc_ir_pool_f64_get(struct TCCIRState *ir, int index);

/* Add symbol reference to pool, return index */
int tcc_ir_pool_sym_add(struct TCCIRState *ir, struct Sym *sym, int32_t addend);

/* Get symbol reference from pool */
struct IRPoolSymref *tcc_ir_pool_sym_get(struct TCCIRState *ir, int index);

/* Add CType to pool, return index */
int tcc_ir_pool_ctype_add(struct TCCIRState *ir, struct CType *type);

/* Get CType from pool */
struct CType *tcc_ir_pool_ctype_get(struct TCCIRState *ir, int index);

/* ============================================================================
 * Jump Target Management
 * ============================================================================ */

/* Set jump target address in dest operand */
void tcc_ir_pool_jump_target_set(struct TCCIRState *ir, int instr_idx, int target_address);

#endif /* TCC_IR_POOL_H */

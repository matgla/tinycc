/*
 *  TCC IR - Value Materialization
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_MAT_H
#define TCC_IR_MAT_H

#include "operand.h"

struct TCCIRState;
struct SValue;
struct IROperand;

/* ============================================================================
 * Materialization Result Structures
 * ============================================================================ */

/* Result of materializing a value */
typedef struct TCCMatValue {
  int used_scratch;
  struct TCCMachineScratchRegs scratch;
  int original_pr0;
  int original_pr1;
} TCCMatValue;

/* Result of materializing an address */
typedef struct TCCMatAddr {
  int used_scratch;
  struct TCCMachineScratchRegs scratch;
  int base_reg;
  int needs_deref;
} TCCMatAddr;

/* Result of materializing a destination */
typedef struct TCCMatDest {
  int used_scratch;
  struct TCCMachineScratchRegs scratch;
  int frame_offset;
  int is_64bit;
} TCCMatDest;

/* ============================================================================
 * SValue Materialization
 * ============================================================================ */

/* Materialize SValue to register */
void tcc_ir_mat_value(struct TCCIRState *ir, struct SValue *sv, TCCMatValue *result);

/* Materialize constant/comparison/jump to register */
void tcc_ir_mat_const(struct TCCIRState *ir, struct SValue *sv, TCCMatValue *result);

/* Materialize address of stack slot */
void tcc_ir_mat_addr(struct TCCIRState *ir, struct SValue *sv, TCCMatAddr *result, int dest_reg);

/* Materialize destination for store */
void tcc_ir_mat_dest(struct TCCIRState *ir, struct SValue *dest, TCCMatDest *result);

/* ============================================================================
 * IROperand Materialization
 * ============================================================================ */

/* Materialize IROperand to register */
void tcc_ir_mat_value_op(struct TCCIRState *ir, struct IROperand *op, TCCMatValue *result);

/* Materialize constant/comparison/jump to register */
void tcc_ir_mat_const_op(struct TCCIRState *ir, struct IROperand *op, TCCMatValue *result);

/* Materialize address of stack slot */
void tcc_ir_mat_addr_op(struct TCCIRState *ir, struct IROperand *op, TCCMatAddr *result, int dest_reg);

/* Materialize destination for store */
void tcc_ir_mat_dest_op(struct TCCIRState *ir, struct IROperand *op, TCCMatDest *result);

/* ============================================================================
 * Materialization Cleanup
 * ============================================================================ */

/* Store back materialized destination if needed */
void tcc_ir_mat_dest_storeback(struct TCCIRState *ir, struct IROperand *op, TCCMatDest *mat);

/* Release scratch registers from materialized value */
void tcc_ir_mat_value_release(struct TCCIRState *ir, TCCMatValue *mat);

/* Release scratch registers from materialized address */
void tcc_ir_mat_addr_release(struct TCCIRState *ir, TCCMatAddr *mat);

/* Release scratch registers from materialized destination */
void tcc_ir_mat_dest_release(struct TCCIRState *ir, TCCMatDest *mat);

/* ============================================================================
 * Spill Detection
 * ============================================================================ */

/* Check if SValue is spilled */
int tcc_ir_mat_spilled(struct SValue *sv);

/* Check if IROperand is spilled */
int tcc_ir_mat_spilled_op(const struct IROperand *op);

#endif /* TCC_IR_MAT_H */

/*
 *  TCC IR - Stack Layout Management
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_STACK_H
#define TCC_IR_STACK_H

#include "../tcctype.h"

struct TCCIRState;
struct SValue;
struct IROperand;
struct SpillCache;

/* ============================================================================
 * Stack Layout Building
 * ============================================================================ */

/* Build complete stack layout for function */
void tcc_ir_stack_build(struct TCCIRState *ir);

/* Reset stack layout to empty */
void tcc_ir_stack_reset(struct TCCIRState *ir);

/* ============================================================================
 * Stack Slot Queries
 * ============================================================================ */

/* Get stack slot by vreg (or NULL if not found) */
const struct TCCStackSlot *tcc_ir_stack_slot_by_vreg(const struct TCCIRState *ir, int vreg);

/* Get stack slot by frame offset (or NULL if not found) */
const struct TCCStackSlot *tcc_ir_stack_slot_by_offset(const struct TCCIRState *ir, int frame_offset);

/* Get stack slot by index */
const struct TCCStackSlot *tcc_ir_stack_slot_by_index(struct TCCIRState *ir, int idx);

/* Get number of stack slots */
int tcc_ir_stack_slot_count(struct TCCIRState *ir);

/* ============================================================================
 * Physical Register Assignment
 * ============================================================================ */

/* Assign physical registers to vreg */
void tcc_ir_stack_reg_assign(struct TCCIRState *ir, int vreg, int offset, int r0, int r1);

/* Get physical registers assigned to vreg */
void tcc_ir_stack_reg_get(struct TCCIRState *ir, int vreg, int *r0, int *r1);

/* ============================================================================
 * Spill Cache (IR State Wrappers)
 * ============================================================================ */

/* Clear spill cache */
void tcc_ir_stack_spill_cache_clear(struct TCCIRState *ir);

/* Record register -> offset mapping in spill cache */
void tcc_ir_stack_spill_cache_record(struct TCCIRState *ir, int reg, int offset);

/* Lookup offset in spill cache, return register or -1 */
int tcc_ir_stack_spill_cache_lookup(struct TCCIRState *ir, int offset);

/* Invalidate register entry in spill cache */
void tcc_ir_stack_spill_cache_invalidate_reg(struct TCCIRState *ir, int reg);

/* Invalidate offset entry in spill cache */
void tcc_ir_stack_spill_cache_invalidate_offset(struct TCCIRState *ir, int offset);

/* ============================================================================
 * Stack Layout Properties
 * ============================================================================ */

/* Get total frame size */
int tcc_ir_stack_frame_size(struct TCCIRState *ir);

/* Get frame alignment requirement */
int tcc_ir_stack_alignment(struct TCCIRState *ir);

/* Get offset to arguments area */
int tcc_ir_stack_args_offset(struct TCCIRState *ir);

/* Get size of arguments area */
int tcc_ir_stack_args_size(struct TCCIRState *ir);

/* ============================================================================
 * Legacy API Wrappers (to be deprecated)
 * ============================================================================ */

/* Build stack layout - legacy name (calls tcc_ir_stack_build) */
void tcc_ir_build_stack_layout(struct TCCIRState *ir);

/* Assign physical registers to vreg - legacy name (calls tcc_ir_stack_reg_assign) */
void tcc_ir_assign_physical_register(struct TCCIRState *ir, int vreg, int offset, int r0, int r1);

#endif /* TCC_IR_STACK_H */

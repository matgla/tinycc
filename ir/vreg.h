/*
 *  TCC IR - Virtual Register Management
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_VREG_H
#define TCC_IR_VREG_H

/* operand.h is included via tcc.h as tccir_operand.h */

struct TCCIRState;

/* ============================================================================
 * Virtual Register Allocation
 * ============================================================================ */

/* Allocate a temporary virtual register */
int tcc_ir_vreg_alloc_temp(struct TCCIRState *ir);

/* Ensure temp live interval array can hold at least `count` entries */
void tcc_ir_vreg_ensure_temp_capacity(struct TCCIRState *ir, int count);

/* Allocate a variable virtual register */
int tcc_ir_vreg_alloc_var(struct TCCIRState *ir);

/* Allocate a parameter virtual register */
int tcc_ir_vreg_alloc_param(struct TCCIRState *ir);

/* Allocate a static chain virtual register for nested functions.
 * This is a special vreg that models the static chain register (R10 on ARM)
 * as a parameter-like entity. It is live-in at function entry with
 * incoming_reg0 set to the static chain register. */
int tcc_ir_vreg_alloc_static_chain(struct TCCIRState *ir);

/* ============================================================================
 * Virtual Register Queries
 * ============================================================================ */

/* Check if vreg is valid */
int tcc_ir_vreg_is_valid(struct TCCIRState *ir, int vr);

/* Check if vreg is ignored (should not be spilled) */
int tcc_ir_vreg_is_ignored(struct TCCIRState *ir, int vr);

/* Get type information for vreg */
int tcc_ir_vreg_type_get(struct TCCIRState *ir, int vreg);

/* Get string representation of vreg type */
const char *tcc_ir_vreg_type_string(int vreg_type);

/* ============================================================================
 * Virtual Register Type Setting
 * ============================================================================ */

/* Mark vreg as float/double type */
void tcc_ir_vreg_type_set_fp(struct TCCIRState *ir, int vreg, int is_float, int is_double);

/* Mark vreg as 64-bit (long long or double) */
void tcc_ir_vreg_type_set_64bit(struct TCCIRState *ir, int vreg);

/* Phase 3: Mark vreg as complex type */
void tcc_ir_vreg_type_set_complex(struct TCCIRState *ir, int vreg);

/* Set original stack offset for vreg */
void tcc_ir_vreg_offset_set(struct TCCIRState *ir, int vreg, int offset);

/* ============================================================================
 * Virtual Register Flags
 * ============================================================================ */

/* Mark vreg as address-taken */
void tcc_ir_vreg_flag_addrtaken_set(struct TCCIRState *ir, int vreg);

/* Check if vreg is address-taken */
int tcc_ir_vreg_flag_addrtaken_get(struct TCCIRState *ir, int vreg);

/* Mark vreg as spilled */
void tcc_ir_vreg_flag_spilled_set(struct TCCIRState *ir, int vreg);

/* Check if vreg is spilled */
int tcc_ir_vreg_flag_spilled_get(struct TCCIRState *ir, int vreg);

/* ============================================================================
 * Virtual Register Physical Assignment
 * ============================================================================ */

/* Get physical register assigned to vreg (or PREG_REG_NONE) */
int tcc_ir_vreg_preg_get(struct TCCIRState *ir, int vreg);

/* Set physical register for vreg */
void tcc_ir_vreg_preg_set(struct TCCIRState *ir, int vreg, int preg);

/* Get high physical register for 64-bit vreg */
int tcc_ir_vreg_preg_hi_get(struct TCCIRState *ir, int vreg);

/* Set high physical register for 64-bit vreg */
void tcc_ir_vreg_preg_hi_set(struct TCCIRState *ir, int vreg, int preg);

/* ============================================================================
 * Live Interval Access
 * ============================================================================ */

/* Get live interval for vreg */
struct IRLiveInterval *tcc_ir_vreg_live_interval(struct TCCIRState *ir, int vreg);

/* ============================================================================
 * Stack Slot Access
 * ============================================================================ */

/* Get stack slot index for vreg (or -1 if not assigned) */
int tcc_ir_vreg_stack_slot_get(struct TCCIRState *ir, int vreg);

/* Set stack slot index for vreg */
void tcc_ir_vreg_stack_slot_set(struct TCCIRState *ir, int vreg, int slot_idx);

/* Get frame offset for vreg */
int tcc_ir_vreg_frame_offset_get(struct TCCIRState *ir, int vreg);

#endif /* TCC_IR_VREG_H */

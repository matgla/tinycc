/*
 *  TCC IR - Liveness Analysis
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_LIVE_H
#define TCC_IR_LIVE_H

struct TCCIRState;
struct IRLiveInterval;

/* ============================================================================
 * Liveness Analysis
 * ============================================================================ */

/* Perform full liveness analysis on IR */
void tcc_ir_live_analysis(struct TCCIRState *ir);

/* Compute live intervals by scanning IR */
void tcc_ir_live_intervals_compute(struct TCCIRState *ir);

/* Patch live intervals with assigned physical registers */
void tcc_ir_live_intervals_patch(struct TCCIRState *ir);

/* Clear all live intervals */
void tcc_ir_live_intervals_clear(struct TCCIRState *ir);

/* Initialize interval start fields */
void tcc_ir_live_intervals_init(struct TCCIRState *ir);

/* ============================================================================
 * Live Interval Extension
 * ============================================================================ */

/* Extend live intervals for vregs used as function parameters */
void tcc_ir_live_params_extend(struct TCCIRState *ir);

/* Extend intervals for vregs used across backward jumps */
void tcc_ir_live_jumps_extend(struct TCCIRState *ir);

/* Extend a specific interval to cover instruction range */
void tcc_ir_live_interval_extend(struct IRLiveInterval *interval, int start, int end);

/* ============================================================================
 * Call Site Analysis
 * ============================================================================ */

/* Check if there's a function call in instruction range */
int tcc_ir_live_has_call_in_range(struct TCCIRState *ir, int start, int end);

/* Record call site for liveness analysis */
void tcc_ir_live_call_record(struct TCCIRState *ir, int instr_idx);

/* ============================================================================
 * Special Cases
 * ============================================================================ */

/* Avoid spilling stack-passed parameters */
void tcc_ir_live_params_avoid_spill(struct TCCIRState *ir);

/* Mark return value vregs with incoming register */
void tcc_ir_live_return_mark(struct TCCIRState *ir);

#endif /* TCC_IR_LIVE_H */

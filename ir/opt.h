/*
 *  TCC IR - Optimization Passes
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_OPT_H
#define TCC_IR_OPT_H

struct TCCIRState;

/* ============================================================================
 * Optimization Pass Functions
 * ============================================================================ */

/* Dead Code Elimination - remove unreachable instructions */
int tcc_ir_opt_dce(struct TCCIRState *ir);

/* Dead Store Elimination - remove stores to dead variables */
int tcc_ir_opt_dse(struct TCCIRState *ir);

/* Constant Propagation - fold constant expressions */
int tcc_ir_opt_const_prop(struct TCCIRState *ir);

/* Constant Propagation (temporary variables only) */
int tcc_ir_opt_const_prop_tmp(struct TCCIRState *ir);

/* Copy Propagation - replace copies with originals */
int tcc_ir_opt_copy_prop(struct TCCIRState *ir);

/* Arithmetic CSE - eliminate redundant arithmetic */
int tcc_ir_opt_cse_arith(struct TCCIRState *ir);

/* Boolean CSE - eliminate redundant boolean operations */
int tcc_ir_opt_cse_bool(struct TCCIRState *ir);

/* Boolean Idempotent Simplification */
int tcc_ir_opt_bool_idempotent(struct TCCIRState *ir);

/* Boolean Expression Simplification */
int tcc_ir_opt_bool_simplify(struct TCCIRState *ir);

/* Return Value Optimization */
int tcc_ir_opt_return(struct TCCIRState *ir);

/* Store-Load Forwarding */
int tcc_ir_opt_sl_forward(struct TCCIRState *ir);

/* Redundant Store Elimination */
int tcc_ir_opt_store_redundant(struct TCCIRState *ir);

/* ============================================================================
 * Optimization Driver
 * ============================================================================ */

/* Run all enabled optimizations */
void tcc_ir_opt_run_all(struct TCCIRState *ir, int level);

/* Run specific optimization by name */
int tcc_ir_opt_run_by_name(struct TCCIRState *ir, const char *name);

/* ============================================================================
 * Optimization Statistics
 * ============================================================================ */

typedef struct TCCOptStats {
  int dce_removed;
  int dse_removed;
  int const_folded;
  int copies_propagated;
  int cse_eliminated;
  int stores_forwarded;
} TCCOptStats;

/* Get optimization statistics */
void tcc_ir_opt_stats_get(TCCOptStats *stats);

/* Reset optimization statistics */
void tcc_ir_opt_stats_reset(void);

/* ============================================================================
 * FP Offset Cache Optimization
 * ============================================================================ */

/* Initialize FP offset cache */
void tcc_ir_opt_fp_cache_init(struct TCCIRState *ir);

/* Clear FP offset cache */
void tcc_ir_opt_fp_cache_clear(struct TCCIRState *ir);

/* Free FP offset cache */
void tcc_ir_opt_fp_cache_free(struct TCCIRState *ir);

/* Lookup offset in FP cache, return register or -1 */
int tcc_ir_opt_fp_cache_lookup(struct TCCIRState *ir, int offset, int *phys_reg);

/* Record offset -> register mapping in FP cache */
void tcc_ir_opt_fp_cache_record(struct TCCIRState *ir, int offset, int phys_reg);

/* Invalidate register entry in FP cache */
void tcc_ir_opt_fp_cache_invalidate_reg(struct TCCIRState *ir, int phys_reg);

#endif /* TCC_IR_OPT_H */

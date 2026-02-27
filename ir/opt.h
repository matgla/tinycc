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
struct IRLoops;

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

/* Value Tracking through Arithmetic - track constants through ADD/SUB */
int tcc_ir_opt_value_tracking(struct TCCIRState *ir);

/* Constant Branch Folding - fold branches with constant conditions */
int tcc_ir_opt_branch_folding(struct TCCIRState *ir);

/* Copy Propagation - replace copies with originals */
int tcc_ir_opt_copy_prop(struct TCCIRState *ir);

/* Legacy copy propagation function - wrapper for tcc_ir_opt_copy_prop */
int tcc_ir_copy_propagation(struct TCCIRState *ir);

/* Arithmetic CSE - eliminate redundant arithmetic */
int tcc_ir_opt_cse_arith(struct TCCIRState *ir);

/* Boolean CSE - eliminate redundant boolean operations */
int tcc_ir_opt_cse_bool(struct TCCIRState *ir);

/* Global CSE - eliminate redundant computations across basic blocks
 * Phase 2 of BUBBLE_SORT_COMPARISON_PLAN
 * Uses dominator-based analysis to find redundant computations
 * in different basic blocks and replace them with ASSIGN */
int tcc_ir_opt_cse_global(struct TCCIRState *ir);

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

/* MLA (Multiply-Accumulate) Fusion - fuse MUL + ADD into MLA */
int tcc_ir_opt_mla_fusion(struct TCCIRState *ir);

/* Indexed Load/Store Fusion - fuse SHL + ADD + LOAD/STORE into indexed memory op */
int tcc_ir_opt_indexed_memory_fusion(struct TCCIRState *ir);

/* Post-Increment Load/Store Fusion - fuse LOAD/STORE + ADD into post-increment op */
int tcc_ir_opt_postinc_fusion(struct TCCIRState *ir);

/* Stack Address CSE - hoist repeated stack address computations */
int tcc_ir_opt_stack_addr_cse(struct TCCIRState *ir);

/* Non-negative value tracking & branch folding */
int tcc_ir_opt_nonneg_branch_fold(struct TCCIRState *ir);

/* Float narrowing - replace double-precision math with float when safe */
int tcc_ir_opt_float_narrowing(struct TCCIRState *ir);

/* Jump Threading - forward jump targets through NOPs and jump chains */
int tcc_ir_opt_jump_threading(struct TCCIRState *ir);

/* Eliminate Fall-Through Jumps - remove redundant unconditional jumps */
int tcc_ir_opt_eliminate_fallthrough(struct TCCIRState *ir);

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

typedef struct TCCOptStats
{
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

/* ============================================================================
 * Helper Functions (defined in tccir.c, used by optimization passes)
 * ============================================================================ */

/* Find the defining instruction for a vreg before a given index */
int tcc_ir_find_defining_instruction(struct TCCIRState *ir, int32_t vreg, int before_idx);

/* Check if a vreg has exactly one use (excluding a specific index) */
int tcc_ir_vreg_has_single_use(struct TCCIRState *ir, int32_t vreg, int exclude_idx);

/* ============================================================================
 * Strength Reduction for Multiply (Phase 3 of FUNCTION_CALLS_OPTIMIZATION_PLAN)
 * ============================================================================ */

/* Transform MUL by constant into shift/add/sub sequence
 * Returns number of instructions generated (0 if not transformable) */
int tcc_ir_strength_reduce_mul(struct TCCIRState *ir, int instr_idx);

/* Run strength reduction on all MUL instructions in function */
int tcc_ir_opt_strength_reduction(struct TCCIRState *ir);

/* ============================================================================
 * Induction Variable Strength Reduction (ARRAY_SUM_OPTIMIZATION_PLAN Phase 1)
 * ============================================================================ */

/* Transform array access via index into pointer increment:
 *   ptr = base + iv*stride  ->  ptr = base (in preheader); ptr += stride (in body)
 * This is the key optimization for array sum loops.
 * Returns number of transformations applied. */
int tcc_ir_opt_iv_strength_reduction(struct TCCIRState *ir);

/* IV strength reduction with pre-detected loops from LICM.
 * This avoids re-detecting loops and ensures correct indices after LICM hoisting. */
int tcc_ir_opt_iv_strength_reduction_with_loops(struct TCCIRState *ir, struct IRLoops *loops);

#endif /* TCC_IR_OPT_H */

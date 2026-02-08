/*
 *  TCC IR - Loop-Invariant Code Motion (LICM) Optimization
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_LICM_H
#define TCC_IR_LICM_H

#include "ir.h"
#include "opt.h"

/* ============================================================================
 * Loop Detection
 * ============================================================================ */

/* Maximum number of loops per function */
#define LICM_MAX_LOOPS 128

/* Maximum number of blocks per loop */
#define LICM_MAX_LOOP_BLOCKS 64

/* Loop structure - simplified for natural loops */
typedef struct IRLoop
{
  int header_idx;           /* Header instruction index */
  int start_idx;            /* First instruction in loop */
  int end_idx;              /* Last instruction in loop */
  int preheader_idx;        /* Where to insert hoisted code (-1 if none) */
  int *body_instrs;         /* Array of instruction indices in loop body */
  int num_body_instrs;      /* Number of instructions in loop body */
  int body_instrs_capacity; /* Capacity of body_instrs array */
  int depth;                /* Nesting depth */
} IRLoop;

/* Loop analysis result */
typedef struct IRLoops
{
  IRLoop *loops; /* Array of loops */
  int num_loops; /* Number of loops found */
  int capacity;  /* Capacity of loops array */
} IRLoops;

/* ============================================================================
 * Function Purity Detection (for Automatic LICM Optimization)
 * ============================================================================ */

/* Function purity levels for LICM */
typedef enum TCCFuncPurity
{
  TCC_FUNC_PURITY_UNKNOWN = 0,
  TCC_FUNC_PURITY_IMPURE = 1, /* Has side effects or depends on global state */
  TCC_FUNC_PURITY_PURE = 2,   /* No side effects, result depends only on args */
  TCC_FUNC_PURITY_CONST = 3,  /* PURE + doesn't read memory (only args) */
} TCCFuncPurity;

/* Infer function purity by analyzing its IR
 * Called after IR generation for each function
 * Returns: TCC_FUNC_PURITY_CONST, TCC_FUNC_PURITY_PURE, or TCC_FUNC_PURITY_IMPURE */
TCCFuncPurity tcc_ir_infer_func_purity(TCCIRState *ir, Sym *func_sym);

/* Add function purity to cache */
void tcc_ir_cache_func_purity(TCCState *s, int func_token, TCCFuncPurity purity);

/* Lookup function purity from cache */
int tcc_ir_lookup_func_purity(TCCState *s, int func_token);

/* Get function purity for a symbol (checks cache, attributes, and well-known table)
 * Returns TCC_FUNC_PURITY_UNKNOWN, TCC_FUNC_PURITY_IMPURE, TCC_FUNC_PURITY_PURE, or TCC_FUNC_PURITY_CONST */
int tcc_ir_get_func_purity(TCCIRState *ir, Sym *sym);

/* ============================================================================
 * Main API
 * ============================================================================ */

/* Main entry point: perform LICM optimization on IR
 * Returns number of instructions hoisted, or 0 if none */
int tcc_ir_opt_licm(TCCIRState *ir);

/* Extended LICM: performs LICM and returns the loop detection result.
 * Caller owns the returned IRLoops* and must free it with tcc_ir_free_loops().
 * Returns NULL if no loops were found. */
IRLoops *tcc_ir_opt_licm_ex(TCCIRState *ir);

/* Detect loops in the IR - simplified version for natural loops */
IRLoops *tcc_ir_detect_loops(TCCIRState *ir);

/* Free loop analysis data */
void tcc_ir_free_loops(IRLoops *loops);

/* Check if an instruction index is inside a loop */
int tcc_ir_is_in_loop(IRLoop *loop, int instr_idx);

/* Identify and hoist loop-invariant stack address computations
 * Returns number of instructions hoisted */
int tcc_ir_hoist_loop_invariants(TCCIRState *ir, IRLoops *loops);

#endif /* TCC_IR_LICM_H */

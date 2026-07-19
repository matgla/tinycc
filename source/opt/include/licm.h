/*
 *  TCC IR - Loop-Invariant Code Motion (LICM) Optimization
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "ir.h"
#include "opt.h"

#define LICM_MAX_LOOPS 128

#define LICM_MAX_LOOP_BLOCKS 64

/* Natural loops only. */
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

typedef struct IRLoops
{
  IRLoop *loops; /* Array of loops */
  int num_loops; /* Number of loops found */
  int capacity;  /* Capacity of loops array */
} IRLoops;

typedef enum TCCFuncPurity
{
  TCC_FUNC_PURITY_UNKNOWN = 0,
  TCC_FUNC_PURITY_IMPURE = 1, /* Has side effects or depends on global state */
  TCC_FUNC_PURITY_PURE = 2,   /* No side effects, result depends only on args */
  TCC_FUNC_PURITY_CONST = 3,  /* PURE + doesn't read memory (only args) */
} TCCFuncPurity;

/* Call after IR generation for the function; never returns UNKNOWN. */
TCCFuncPurity tcc_ir_infer_func_purity(TCCIRState *ir, Sym *func_sym);

void tcc_ir_cache_func_purity(TCCState *s, int func_token, TCCFuncPurity purity);

int tcc_ir_lookup_func_purity(TCCState *s, int func_token);

/* Checks cache, then attributes, then the well-known-function table. */
int tcc_ir_get_func_purity(TCCIRState *ir, Sym *sym);

/* Returns number of instructions hoisted, or 0 if none. */
int tcc_ir_opt_licm(TCCIRState *ir);

/* Caller owns the result and must free it with tcc_ir_free_loops(); NULL if no loops. */
IRLoops *tcc_ir_opt_licm_ex(TCCIRState *ir);

/* ssa:licm — runs on flat IR before ssa:iv_strength_reduction. */
int ssa_opt_licm(TCCIRState *ir);

/* Must run immediately before register allocation; returns params promoted. */
int tcc_ir_promote_loop_stack_params(TCCIRState *ir);

/* Natural loops only. */
IRLoops *tcc_ir_detect_loops(TCCIRState *ir);

void tcc_ir_free_loops(IRLoops *loops);

int tcc_ir_is_in_loop(IRLoop *loop, int instr_idx);

/* Returns max values hoistable (>= 1) from register pressure over the loop body. */
int tcc_ir_estimate_hoist_budget(TCCIRState *ir, int loop_start, int loop_end, int num_params);

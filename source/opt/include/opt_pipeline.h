/*
 *  TCC IR - Optimization Pass Pipeline
 *
 *  Declarative pass registration, grouping, and execution.
 *  Replaces procedural orchestration with configurable pass tables.
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include <stdint.h>
#include "opt_engine.h"

struct TCCIRState;

/* REQUIRES_*: analysis a pass needs valid on entry. */
#define IR_PASS_REQUIRES_DU          (1u << 0)
#define IR_PASS_REQUIRES_DU_TMP_ONLY (1u << 1)
#define IR_PASS_REQUIRES_MERGE       (1u << 2)
#define IR_PASS_REQUIRES_BLOCKS      (1u << 3)
#define IR_PASS_REQUIRES_LOOPS       (1u << 4)

typedef int (*ir_opt_pass_fn)(IROptCtx *ctx);
typedef int (*ir_opt_pass_ir_fn)(struct TCCIRState *ir);

/* takes_ir selects fn.run_ir, so plain TCCIRState* passes need no IROptCtx* adapter. */
typedef struct IROptPass
{
  const char *name;
  union {
    ir_opt_pass_fn run;
    ir_opt_pass_ir_fn run_ir;
  } fn;
  uint32_t requires;
  uint16_t flag_offset;
  uint8_t takes_ir;
} IROptPass;

/* Sequence of passes with optional fixed-point iteration. */
typedef struct IRPassGroup
{
  const char *name;
  const IROptPass *passes;
  int count;
  int max_iterations;
  uint8_t compact_after;
  int8_t trigger_idx;
} IRPassGroup;

/* Returns total number of changes across all passes. */
int tcc_ir_opt_run_pipeline(struct TCCIRState *ir, const IRPassGroup *groups,
                            int group_count);

int tcc_ir_opt_run_group(IROptCtx *ctx, const IRPassGroup *group);

extern const IRPassGroup propagation_group;
extern const IRPassGroup memory_group;
extern const IRPassGroup late_cleanup_group;
extern const IRPassGroup entry_store_group;

/* Concrete gen-pass adapters (pipeline-callable). */
int tcc_ir_opt_gens_fusion_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_deref_indexed_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_bool_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_call_result_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_branch_ex(IROptCtx *ctx);

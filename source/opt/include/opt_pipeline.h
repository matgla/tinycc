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

/* REQUIRES_*: analysis a pass needs valid on entry; INVALIDATES_*: what it leaves stale. */
#define IR_PASS_REQUIRES_DU          (1u << 0)
#define IR_PASS_REQUIRES_DU_TMP_ONLY (1u << 1)
#define IR_PASS_REQUIRES_MERGE       (1u << 2)
#define IR_PASS_REQUIRES_BLOCKS      (1u << 3)
#define IR_PASS_REQUIRES_LOOPS       (1u << 4)

#define IR_PASS_INVALIDATES_DU       (1u << 0)
#define IR_PASS_INVALIDATES_CFG      (1u << 1)
#define IR_PASS_INVALIDATES_LOOPS    (1u << 2)
#define IR_PASS_INVALIDATES_ALL      (0xFFu)

typedef int (*ir_opt_pass_fn)(IROptCtx *ctx);

typedef struct IROptPass
{
  const char *name;
  ir_opt_pass_fn run;
  uint32_t requires;
  uint32_t invalidates;
  uint16_t flag_offset;
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

/* Runs an IROptGen table as a pipeline pass. */
typedef struct IROptGenPassData
{
  const IROptGen *gens;
  int count;
} IROptGenPassData;

int tcc_ir_opt_gen_pass_adapter(IROptCtx *ctx, const IROptGenPassData *data);

typedef enum IROptLevel
{
  IR_OPT_LEVEL_0 = 0,
  IR_OPT_LEVEL_1 = 1,
  IR_OPT_LEVEL_2 = 2,
  IR_OPT_LEVEL_S = 3,
} IROptLevel;

/* Sets the out-params to a static table — do not free. */
void tcc_ir_opt_get_pipeline(IROptLevel level, const IRPassGroup **out_groups,
                             int *out_count);

/* get_pipeline + run_pipeline; returns total changes. */
int tcc_ir_opt_run_default(struct TCCIRState *ir, IROptLevel level);

/* Entry-store-prop group (trigger-based, 3 iterations, compact_after). */
extern const IRPassGroup entry_store_group;

/* Concrete gen-pass adapters (pipeline-callable). */
int tcc_ir_opt_gens_fusion_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_deref_indexed_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_bool_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_call_result_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_branch_ex(IROptCtx *ctx);

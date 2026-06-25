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

#ifndef TCC_IR_OPT_PIPELINE_H
#define TCC_IR_OPT_PIPELINE_H

#include <stdint.h>
#include "opt_engine.h"

struct TCCIRState;

/* ============================================================================
 * Pass requirement / invalidation flags
 * ============================================================================ */
#define IR_PASS_REQUIRES_DU          (1u << 0)
#define IR_PASS_REQUIRES_DU_TMP_ONLY (1u << 1)
#define IR_PASS_REQUIRES_MERGE       (1u << 2)
#define IR_PASS_REQUIRES_BLOCKS      (1u << 3)
#define IR_PASS_REQUIRES_LOOPS       (1u << 4)

#define IR_PASS_INVALIDATES_DU       (1u << 0)
#define IR_PASS_INVALIDATES_CFG      (1u << 1)
#define IR_PASS_INVALIDATES_LOOPS    (1u << 2)
#define IR_PASS_INVALIDATES_ALL      (0xFFu)

/* ============================================================================
 * Pass descriptor
 * ============================================================================ */
typedef int (*ir_opt_pass_fn)(IROptCtx *ctx);

typedef struct IROptPass
{
  const char *name;
  ir_opt_pass_fn run;
  uint32_t requires;
  uint32_t invalidates;
  uint16_t flag_offset;
} IROptPass;

/* ============================================================================
 * Pass group: a sequence of passes with optional fixed-point iteration
 * ============================================================================ */
typedef struct IRPassGroup
{
  const char *name;
  const IROptPass *passes;
  int count;
  int max_iterations;
  uint8_t compact_after;
  int8_t trigger_idx;
} IRPassGroup;

/* ============================================================================
 * Pipeline execution
 * ============================================================================ */

/* Run a pipeline of pass groups on the given IR.
 * Returns total number of changes across all passes. */
int tcc_ir_opt_run_pipeline(struct TCCIRState *ir, const IRPassGroup *groups,
                            int group_count);

/* Run a single pass group (used internally and for testing). */
int tcc_ir_opt_run_group(IROptCtx *ctx, const IRPassGroup *group);

/* ============================================================================
 * Generator-to-pass adapter
 * ============================================================================ */

/* Create a pass function from an IROptGen table (for gens_fusion, gens_bool, etc.) */
typedef struct IROptGenPassData
{
  const IROptGen *gens;
  int count;
} IROptGenPassData;

int tcc_ir_opt_gen_pass_adapter(IROptCtx *ctx, const IROptGenPassData *data);

/* ============================================================================
 * Optimization level presets
 * ============================================================================ */
typedef enum IROptLevel
{
  IR_OPT_LEVEL_0 = 0,
  IR_OPT_LEVEL_1 = 1,
  IR_OPT_LEVEL_2 = 2,
  IR_OPT_LEVEL_S = 3,
} IROptLevel;

/* Get the default pipeline for a given optimization level.
 * Sets *out_groups and *out_count. Returned pointer is static — do not free. */
void tcc_ir_opt_get_pipeline(IROptLevel level, const IRPassGroup **out_groups,
                             int *out_count);

/* Convenience: run the full pipeline for a given optimization level.
 * Equivalent to get_pipeline + run_pipeline. Returns total changes. */
int tcc_ir_opt_run_default(struct TCCIRState *ir, IROptLevel level);

/* Entry-store-prop group (trigger-based, 3 iterations, compact_after) */
extern const IRPassGroup entry_store_group;

/* Concrete gen-pass adapters (pipeline-callable) */
int tcc_ir_opt_gens_fusion_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_deref_indexed_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_disp_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_chain_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_pair_reorder_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_bool_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_call_result_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_call_result_post_ex(IROptCtx *ctx);
int tcc_ir_opt_gens_branch_ex(IROptCtx *ctx);

#endif /* TCC_IR_OPT_PIPELINE_H */

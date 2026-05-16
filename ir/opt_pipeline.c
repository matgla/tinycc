/*
 *  TCC IR - Optimization Pass Pipeline
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_pipeline.h"
#include "opt.h"
#include "opt_gens_fusion.h"
#include "opt_gens_bool.h"
#include "opt_gens_call_result.h"
#include "opt_gens_branch.h"
#include "opt_utils.h"

#define FLAG(f) (uint16_t)offsetof(TCCState, f)

static void pipeline_ensure_requirements(IROptCtx *ctx, uint32_t requires)
{
  if (requires & IR_PASS_REQUIRES_DU)
    tcc_ir_opt_ctx_require_du(ctx);
  else if (requires & IR_PASS_REQUIRES_DU_TMP_ONLY)
    tcc_ir_opt_ctx_require_du_mode(ctx, IR_DU_MODE_TMP_ONLY);
  if (requires & IR_PASS_REQUIRES_MERGE)
    tcc_ir_opt_ctx_require_merge(ctx);
  if (requires & IR_PASS_REQUIRES_BLOCKS)
    tcc_ir_opt_ctx_require_block_starts(ctx);
  if (requires & IR_PASS_REQUIRES_LOOPS)
    tcc_ir_opt_ctx_require_loops(ctx);
}

static void pipeline_apply_invalidations(IROptCtx *ctx, uint32_t invalidates)
{
  if (invalidates)
    tcc_ir_opt_ctx_invalidate(ctx);
}

int tcc_ir_opt_run_group(IROptCtx *ctx, const IRPassGroup *group)
{
  int total_changes = 0;
  int iterations = group->max_iterations > 0 ? group->max_iterations : 1;

  for (int iter = 0; iter < iterations; iter++) {
    int round_changes = 0;

    for (int p = 0; p < group->count; p++) {
      const IROptPass *pass = &group->passes[p];
      if (!pass->run)
        continue;
      if (pass->flag_offset && !*((unsigned char *)tcc_state + pass->flag_offset))
        continue;

      pipeline_ensure_requirements(ctx, pass->requires);

      int changes = pass->run(ctx);
      if (changes > 0) {
        round_changes += changes;
        pipeline_apply_invalidations(ctx, pass->invalidates);
      }
    }

    total_changes += round_changes;
    if (round_changes == 0)
      break;
  }

  return total_changes;
}

int tcc_ir_opt_run_pipeline(TCCIRState *ir, const IRPassGroup *groups,
                            int group_count)
{
  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);

  int total_changes = 0;

  for (int g = 0; g < group_count; g++) {
    total_changes += tcc_ir_opt_run_group(&ctx, &groups[g]);

    if (groups[g].compact_after)
      tcc_ir_opt_ctx_invalidate(&ctx);
  }

  tcc_ir_opt_ctx_free(&ctx);
  return total_changes;
}

int tcc_ir_opt_gen_pass_adapter(IROptCtx *ctx, const IROptGenPassData *data)
{
  return tcc_ir_opt_run_gens(ctx, data->gens, data->count);
}

/* ============================================================================
 * Optimization Level Presets
 * ============================================================================ */

#define PASS(nm, fn, req, inv) { nm, fn, req, inv, 0 }
#define PASS_GATED(nm, fn, req, inv, flag) { nm, fn, req, inv, flag }

static const IROptPass propagation_passes[] = {
  PASS_GATED("dce",              tcc_ir_opt_dce_ex,              0, IR_PASS_INVALIDATES_DU, FLAG(opt_dce)),
  PASS_GATED("const_prop",      tcc_ir_opt_const_prop_ex,       0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("global_init",     tcc_ir_opt_global_init_prop_ex, 0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("const_prop_tmp",  tcc_ir_opt_const_prop_tmp_ex,   0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("add_reassoc",     tcc_ir_opt_add_reassoc_ex,      0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("string_calls",    tcc_ir_opt_const_string_calls_ex, 0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("value_tracking",  tcc_ir_opt_value_tracking_ex,   0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("cmp_expr_fold",   tcc_ir_opt_cmp_expr_fold_ex,    0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("branch_fold",     tcc_ir_opt_branch_folding_ex,   0, IR_PASS_INVALIDATES_ALL, FLAG(opt_const_prop)),
  PASS_GATED("stack_nonnull",   tcc_ir_opt_stack_addr_nonnull_fold_ex, 0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("setif_fuse",      tcc_ir_opt_setif_branch_fuse_ex, 0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("stack_bool",      tcc_ir_opt_stack_bool_diamond_ex, 0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("or_bool",         tcc_ir_opt_or_bool_diamond_ex,  0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("var_tmp_fwd",     tcc_ir_opt_var_tmp_fwd_ex,      0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("var_to_tmp",      tcc_ir_opt_var_to_tmp_ex,       0, IR_PASS_INVALIDATES_DU, FLAG(opt_copy_prop)),
  PASS_GATED("nonneg_fold",     tcc_ir_opt_nonneg_branch_fold_ex, 0, IR_PASS_INVALIDATES_ALL, FLAG(opt_nonneg_fold)),
  PASS_GATED("float_branch",    tcc_ir_opt_float_branch_fold_ex, 0, IR_PASS_INVALIDATES_ALL, FLAG(opt_vrp)),
  PASS_GATED("vrp",             tcc_ir_opt_vrp_ex,              0, IR_PASS_INVALIDATES_ALL, FLAG(opt_vrp)),
  PASS_GATED("float_narrow",    tcc_ir_opt_float_narrowing_ex,  0, IR_PASS_INVALIDATES_DU, FLAG(opt_float_narrow)),
  PASS_GATED("deref_fwd",       tcc_ir_opt_deref_fwd_ex,        0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
};

static const IROptPass fusion_passes[] = {
  PASS("fusion_mla",    tcc_ir_opt_gens_fusion_ex,     IR_PASS_REQUIRES_DU, IR_PASS_INVALIDATES_DU),
  PASS_GATED("deref_indexed", tcc_ir_opt_gens_deref_indexed_ex, IR_PASS_REQUIRES_DU, IR_PASS_INVALIDATES_DU, FLAG(opt_indexed_memory)),
  PASS_GATED("disp_fusion",   tcc_ir_opt_gens_disp_ex,       IR_PASS_REQUIRES_DU, IR_PASS_INVALIDATES_DU, FLAG(opt_disp_fusion)),
  PASS_GATED("copy_prop",     tcc_ir_opt_copy_prop_ex,       0, IR_PASS_INVALIDATES_DU, FLAG(opt_copy_prop)),
  PASS_GATED("dce",           tcc_ir_opt_dce_ex,             0, IR_PASS_INVALIDATES_DU, FLAG(opt_dce)),
  PASS_GATED("chain_fold",    tcc_ir_opt_gens_chain_ex,      IR_PASS_REQUIRES_DU, IR_PASS_INVALIDATES_DU, FLAG(opt_disp_fusion)),
  PASS_GATED("pair_reorder",  tcc_ir_opt_gens_pair_reorder_ex, IR_PASS_REQUIRES_DU, IR_PASS_INVALIDATES_DU, FLAG(opt_disp_fusion)),
  PASS_GATED("postinc",       tcc_ir_opt_postinc_fusion_ex,  0, IR_PASS_INVALIDATES_DU, FLAG(opt_postinc_fusion)),
  PASS_GATED("bool_simplify", tcc_ir_opt_gens_bool_ex,       0, IR_PASS_INVALIDATES_DU, FLAG(opt_bool_idempotent)),
};

static const IROptPass memory_passes[] = {
  PASS_GATED("sl_forward",  tcc_ir_opt_sl_forward_ex,      0, IR_PASS_INVALIDATES_ALL, FLAG(opt_store_load_fwd)),
  PASS_GATED("const_prop",  tcc_ir_opt_const_prop_ex,      0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("const_tmp",   tcc_ir_opt_const_prop_tmp_ex,  0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("const_var",   tcc_ir_opt_const_var_prop_ex,  0, IR_PASS_INVALIDATES_DU, FLAG(opt_const_prop)),
  PASS_GATED("branch_fold", tcc_ir_opt_branch_folding_ex,  0, IR_PASS_INVALIDATES_ALL, FLAG(opt_const_prop)),
  PASS_GATED("dce",         tcc_ir_opt_dce_ex,             0, IR_PASS_INVALIDATES_DU, FLAG(opt_dce)),
  PASS_GATED("jump_thread", tcc_ir_opt_jump_threading_ex,  0, IR_PASS_INVALIDATES_ALL, FLAG(opt_jump_threading)),
};

static const IROptPass late_cleanup_passes[] = {
  PASS_GATED("dce",              tcc_ir_opt_dce_ex,              0, IR_PASS_INVALIDATES_DU, FLAG(opt_dce)),
  PASS_GATED("store_redundant",  tcc_ir_opt_store_redundant_ex,  0, IR_PASS_INVALIDATES_DU, FLAG(opt_redundant_store)),
  PASS_GATED("dse",              tcc_ir_opt_dse_ex,              0, IR_PASS_INVALIDATES_DU, FLAG(opt_dead_store)),
  PASS_GATED("dead_var_store",   tcc_ir_opt_dead_var_store_elim_ex, 0, IR_PASS_INVALIDATES_DU, FLAG(opt_dead_store)),
  PASS_GATED("redundant_assign", tcc_ir_opt_redundant_var_assign_ex, 0, IR_PASS_INVALIDATES_DU, FLAG(opt_dead_store)),
  PASS_GATED("call_result",      tcc_ir_opt_gens_call_result_ex, IR_PASS_REQUIRES_DU, IR_PASS_INVALIDATES_DU, FLAG(opt_dead_store)),
};

#undef PASS
#undef PASS_GATED
#undef FLAG

#define COUNTOF(arr) (int)(sizeof(arr) / sizeof((arr)[0]))

/* O0: minimal — only DCE for correctness */
static const IROptPass o0_passes[] = {
  { "dce", tcc_ir_opt_dce_ex, 0, IR_PASS_INVALIDATES_DU, 0 },
};
static const IRPassGroup pipeline_o0[] = {
  { "cleanup", o0_passes, COUNTOF(o0_passes), 1, 0 },
};

/* O1: propagation + simplification + late cleanup */
static const IRPassGroup pipeline_o1[] = {
  { "propagation",  propagation_passes,  COUNTOF(propagation_passes),  10, 0 },
  { "late_cleanup", late_cleanup_passes, COUNTOF(late_cleanup_passes), 1, 1 },
};

/* O2: full pipeline including memory + fusion */
static const IRPassGroup pipeline_o2[] = {
  { "propagation",  propagation_passes,  COUNTOF(propagation_passes),  10, 0 },
  { "memory",       memory_passes,       COUNTOF(memory_passes),       12, 1 },
  { "fusion",       fusion_passes,       COUNTOF(fusion_passes),       1, 0 },
  { "late_cleanup", late_cleanup_passes, COUNTOF(late_cleanup_passes), 1, 1 },
};

/* Os: like O2 but skip fusion (keeps code size smaller) */
static const IRPassGroup pipeline_os[] = {
  { "propagation",  propagation_passes,  COUNTOF(propagation_passes),  10, 0 },
  { "memory",       memory_passes,       COUNTOF(memory_passes),       12, 1 },
  { "late_cleanup", late_cleanup_passes, COUNTOF(late_cleanup_passes), 1, 1 },
};

void tcc_ir_opt_get_pipeline(IROptLevel level, const IRPassGroup **out_groups,
                             int *out_count)
{
  switch (level) {
  case IR_OPT_LEVEL_0:
    *out_groups = pipeline_o0;
    *out_count = 1;
    break;
  case IR_OPT_LEVEL_1:
    *out_groups = pipeline_o1;
    *out_count = 2;
    break;
  case IR_OPT_LEVEL_S:
    *out_groups = pipeline_os;
    *out_count = 3;
    break;
  case IR_OPT_LEVEL_2:
  default:
    *out_groups = pipeline_o2;
    *out_count = 4;
    break;
  }
}

int tcc_ir_opt_run_default(TCCIRState *ir, IROptLevel level)
{
  const IRPassGroup *groups;
  int count;
  tcc_ir_opt_get_pipeline(level, &groups, &count);
  return tcc_ir_opt_run_pipeline(ir, groups, count);
}

/* ============================================================================
 * Concrete gen-pass adapters for pipeline integration
 * ============================================================================ */
int tcc_ir_opt_gens_fusion_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, fusion_gens, fusion_gens_count);
}

int tcc_ir_opt_gens_deref_indexed_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, fusion_deref_indexed_gens, fusion_deref_indexed_gens_count);
}

int tcc_ir_opt_gens_disp_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, fusion_disp_gens, fusion_disp_gens_count);
}

int tcc_ir_opt_gens_chain_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, fusion_chain_gens, fusion_chain_gens_count);
}

int tcc_ir_opt_gens_pair_reorder_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, fusion_pair_reorder_gens, fusion_pair_reorder_gens_count);
}

int tcc_ir_opt_gens_bool_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, bool_gens, bool_gens_count);
}

int tcc_ir_opt_gens_call_result_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, call_result_gens, call_result_gens_count);
}

int tcc_ir_opt_gens_call_result_post_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, call_result_post_gens, call_result_post_gens_count);
}

int tcc_ir_opt_gens_branch_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, branch_gens, branch_gens_count);
}

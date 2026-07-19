/*
 *  TCC IR - Gen-pass adapters for the pipeline
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
#include "opt_gens_fusion.h"
#include "opt/flat/bool.h"
#include "opt/flat/call_result.h"
#include "opt/flat/branch.h"
#include "const_string_fold.h"

int tcc_ir_opt_gens_fusion_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, fusion_gens, fusion_gens_count);
}

int tcc_ir_opt_gens_deref_indexed_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, fusion_deref_indexed_gens, fusion_deref_indexed_gens_count);
}

int tcc_ir_opt_gens_bool_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, bool_gens, bool_gens_count);
}

int tcc_ir_opt_gens_call_result_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, call_result_gens, call_result_gens_count);
}

int tcc_ir_opt_gens_branch_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_gens(ctx, branch_gens, branch_gens_count);
}

int tcc_ir_opt_cmp_stack_addr_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_cmp_stack_addr_fold(ctx->ir); }

int tcc_ir_opt_const_prop_tmp_ex(IROptCtx *ctx) { return tcc_ir_opt_const_prop_tmp(ctx->ir); }
int tcc_ir_opt_const_var_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_const_var_prop(ctx->ir); }
int tcc_ir_opt_global_init_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_global_init_prop(ctx->ir); }
int tcc_ir_opt_symref_const_prop_ex(IROptCtx *ctx) { return tcc_ir_opt_symref_const_prop(ctx->ir); }
int tcc_ir_opt_value_tracking_ex(IROptCtx *ctx) { return tcc_ir_opt_value_tracking(ctx->ir); }
int tcc_ir_opt_const_string_calls_ex(IROptCtx *ctx) { return tcc_ir_opt_const_string_calls(ctx->ir); }
int ssa_const_string_fold_flat_ex(IROptCtx *ctx) { return tcc_ir_ssa_opt_const_string_fold_flat(ctx->ir); }

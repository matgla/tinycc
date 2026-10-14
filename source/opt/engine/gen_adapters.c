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

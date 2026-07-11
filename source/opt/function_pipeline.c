/*
 *  TCC Optimizer - Per-Function Optimization Pipeline
 *
 *  Target-independent optimization passes for a single function's IR, from
 *  propagation through the post-pipeline cleanups.  Register allocation and
 *  codegen preparation are the backend's job, driven by gen_function
 *  (source/backend/generators/); see function_pipeline.h.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "tcc.h"
#include "ir/core.h"
#include "ir/opt.h"
#include "ir/opt_utils.h"
#include "ir/opt_engine.h"
#include "ir/opt_pipeline.h"
#include "ir/opt_gens_fusion.h"
#include "ir/opt_gens_bool.h"
#include "ir/opt_gens_call_result.h"
#include "tccir.h"

#include "function_pipeline.h"

#include <string.h>

extern void dbg_scan_overlap(TCCIRState *ir, const char *pass);

#ifdef CONFIG_TCC_DEBUG
#define DUMP_IR_AFTER_PASS(ir, name) dump_ir_after_pass(ir, name)
#else
#define DUMP_IR_AFTER_PASS(ir, name) ((void)0)
#endif

static void dump_ir_after_pass(TCCIRState *ir, const char *pass_name)
{
  tcc_ir_dump_after_pass(ir, pass_name);
}

/* ================================================================== */
/*  Phase 1: propagation + early CSE                                  */
/* ================================================================== */
static void run_propagation_passes(TCCIRState *ir)
{
  if (tcc_state->opt_ipc)
  {
    tcc_ir_opt_const_call_replace(ir);
    DUMP_IR_AFTER_PASS(ir, "const_call_replace");
  }

  {
    const IRPassGroup *groups;
    int group_count;
    tcc_ir_opt_get_pipeline(IR_OPT_LEVEL_2, &groups, &group_count);
    IROptCtx prop_ctx;
    tcc_ir_opt_ctx_init(&prop_ctx, ir);
    tcc_ir_opt_run_group(&prop_ctx, &groups[0]);
    tcc_ir_opt_ctx_free(&prop_ctx);
  }
  dbg_scan_overlap(ir, "P1-after-prop-group");

  tcc_state->ir_post_float_narrow = 1;

  DUMP_IR_AFTER_PASS(ir, "propagation_group");

  if (tcc_state->optimize >= 1)
    tcc_ir_opt_globalsym_cse(ir);

  tcc_ir_opt_compact_nops(ir);
  DUMP_IR_AFTER_PASS(ir, "compact_nops_pre_jthread");
}

/* ================================================================== */
/*  Phase 2: gens fusion, disp, chain, bool, lea, stackoff CSE        */
/* ================================================================== */
static void run_opt_pipeline(TCCIRState *ir)
{
  IROptCtx pipeline_ctx;
  tcc_ir_opt_ctx_init(&pipeline_ctx, ir);

  if (tcc_state->optimize > 0)
    tcc_ir_opt_gens_fusion_ex(&pipeline_ctx);

  if (tcc_state->opt_indexed_memory)
    tcc_ir_opt_gens_deref_indexed_ex(&pipeline_ctx);

  if (tcc_state->opt_disp_fusion)
    tcc_ir_opt_gens_disp_ex(&pipeline_ctx);

  if (tcc_state->opt_disp_fusion)
    tcc_ir_opt_add_deref_fold(ir);

  if (tcc_state->opt_disp_fusion) {
    tcc_ir_opt_ctx_invalidate(&pipeline_ctx);
    tcc_ir_opt_gens_chain_ex(&pipeline_ctx);
    tcc_ir_opt_gens_pair_reorder_ex(&pipeline_ctx);
  }

  if (tcc_state->optimize >= 1)
    tcc_ir_opt_call_chain_rename(ir);

  if (tcc_state->optimize >= 1)
    tcc_ir_opt_stackoff_addr_cse(ir);

  if (tcc_state->opt_lea_fold)
    tcc_ir_opt_lea_fold(ir);

  if (tcc_state->opt_lea_fold)
    tcc_ir_opt_lea_rmw_fold(ir);

  if (tcc_state->opt_bool_idempotent) {
    tcc_ir_opt_ctx_invalidate(&pipeline_ctx);
    tcc_ir_opt_gens_bool_ex(&pipeline_ctx);
  }

  tcc_ir_opt_compact_nops(ir);
  DUMP_IR_AFTER_PASS(ir, "compact_nops_pre_slfwd");

  tcc_ir_opt_ctx_free(&pipeline_ctx);
  dbg_scan_overlap(ir, "P2-after-pipeline_ctx");
}

/* ================================================================== */
/*  Phase 3: store/load forwarding + memory-group passes              */
/* ================================================================== */
static void run_store_load_fwd_passes(TCCIRState *ir)
{
  if (tcc_state->opt_store_load_fwd && !ir->has_static_chain)
  {
    IROptCtx esp_ctx;
    tcc_ir_opt_ctx_init(&esp_ctx, ir);
    tcc_ir_opt_run_group(&esp_ctx, &entry_store_group);
    tcc_ir_opt_ctx_free(&esp_ctx);
  }
  DUMP_IR_AFTER_PASS(ir, "entry_store_group");

  if (tcc_state->opt_redundant_store)
  {
    tcc_ir_opt_struct_copy_roundtrip_elim(ir);
  }

  if (tcc_state->opt_store_load_fwd && !ir->has_static_chain)
  {
    const IRPassGroup *groups;
    int group_count;
    tcc_ir_opt_get_pipeline(IR_OPT_LEVEL_2, &groups, &group_count);
    IROptCtx sl_ctx;
    tcc_ir_opt_ctx_init(&sl_ctx, ir);
    tcc_ir_opt_run_group(&sl_ctx, &groups[1]);
    tcc_ir_opt_ctx_free(&sl_ctx);
  }
  DUMP_IR_AFTER_PASS(ir, "memory_group");

  if (tcc_state->opt_store_load_fwd && !ir->has_static_chain)
  {
    tcc_ir_opt_param_addrof_const_fold(ir);
    DUMP_IR_AFTER_PASS(ir, "ZZ_padrof");
    tcc_ir_opt_local_addrof_const_fold(ir);
    DUMP_IR_AFTER_PASS(ir, "ZZ_ladrof");
    if (tcc_state->opt_const_prop)
      tcc_ir_opt_addrof_var_fwd(ir);
    DUMP_IR_AFTER_PASS(ir, "ZZ_aofvar");
    if (tcc_state->opt_store_load_fwd)
      tcc_ir_opt_global_sl_fwd(ir);
    DUMP_IR_AFTER_PASS(ir, "ZZ_gslfwd");
    if (tcc_state->opt_store_load_fwd)
      tcc_ir_opt_invariant_global_load_hoist(ir);
    DUMP_IR_AFTER_PASS(ir, "ZZ_iglh");
  }
}

/* ================================================================== */
/*  Phase 4: dead-store, call-result, late cleanup                    */
/* ================================================================== */
static void run_dead_store_and_cleanup(TCCIRState *ir)
{
  {
    IROptCtx cr_ctx;
    tcc_ir_opt_ctx_init(&cr_ctx, ir);
    if (tcc_state->opt_dead_store)
      tcc_ir_opt_gens_call_result_ex(&cr_ctx);
    tcc_ir_opt_ctx_free(&cr_ctx);
  }

  if (tcc_state->opt_dead_store)
    tcc_ir_opt_dead_init_via_call(ir);
  DUMP_IR_AFTER_PASS(ir, "ZZ_dead_init_via_call");

  {
    const IRPassGroup *groups;
    int group_count;
    tcc_ir_opt_get_pipeline(IR_OPT_LEVEL_2, &groups, &group_count);
    const IRPassGroup *cleanup_group = &groups[group_count - 1];
    IROptCtx cleanup_ctx;
    tcc_ir_opt_ctx_init(&cleanup_ctx, ir);
    tcc_ir_opt_run_group(&cleanup_ctx, cleanup_group);
    tcc_ir_opt_ctx_free(&cleanup_ctx);
    DUMP_IR_AFTER_PASS(ir, "ZZ_late_cleanup_1");

    if (tcc_state->opt_dead_store) {
      for (int iter = 0; iter < 4; iter++) {
        IROptCtx ctx_cr;
        tcc_ir_opt_ctx_init(&ctx_cr, ir);
        int ch = tcc_ir_opt_gens_call_result_ex(&ctx_cr);
        tcc_ir_opt_ctx_free(&ctx_cr);
        if (ch == 0)
          break;
        IROptCtx ctx_lc;
        tcc_ir_opt_ctx_init(&ctx_lc, ir);
        tcc_ir_opt_run_group(&ctx_lc, cleanup_group);
        tcc_ir_opt_ctx_free(&ctx_lc);
      }
    }
  }

  tcc_ir_opt_memmove_to_indexed_stores(ir);
  tcc_ir_opt_compact_nops(ir);

#ifdef CONFIG_TCC_DEBUG
  if (tcc_state->dump_ir) {
    printf("=== IR AFTER LOOP ROTATION ===\n");
    tcc_ir_show(ir);
    printf("=== END IR AFTER LOOP ROTATION ===\n");
  }
#endif
}

/* ================================================================== */
/*  Phase 5: post-pipeline (pack64, shl32, deref, stack_addr, select) */
/* ================================================================== */
static void run_post_pipeline_passes(TCCIRState *ir)
{
  if (tcc_state->opt_store_load_fwd)
  {
    if (tcc_ir_opt_diamond_store_fwd(ir) > 0)
    {
      DUMP_IR_AFTER_PASS(ir, "ZZ_diamond_store_fwd");
    }
  }

  if (tcc_ir_opt_memmove_to_indexed_stores(ir) > 0)
  {
    tcc_ir_opt_compact_nops(ir);
  }

  tcc_ir_opt_pack64(ir);
  tcc_ir_opt_pack64_implicit(ir);
  tcc_ir_opt_pack64_from_stack_stores(ir);
  tcc_ir_opt_shl32_or_chain(ir);

  DUMP_IR_AFTER_PASS(ir, "ZZ2_shl32");

  if (tcc_state->opt_const_prop)
    tcc_ir_opt_deref_fwd(ir);
  DUMP_IR_AFTER_PASS(ir, "ZZ2_deref_fwd");

  if (tcc_state->opt_stack_addr_cse)
    tcc_ir_opt_stack_addr_cse(ir);

  DUMP_IR_AFTER_PASS(ir, "ZZ2_paf");

  if (tcc_state->opt_const_prop)
  {
    tcc_ir_opt_cmp_stack_addr_fold(ir);
  }

  DUMP_IR_AFTER_PASS(ir, "ZZ2_dle1");

  if (tcc_state->opt_jump_threading)
    tcc_ir_opt_eliminate_fallthrough(ir);

  if (tcc_state->opt_copy_prop)
  {
    tcc_ir_opt_var_tmp_fwd(ir);
  }
  DUMP_IR_AFTER_PASS(ir, "ZZ2_vtf");

  dbg_scan_overlap(ir, "R1-before-pack64_taut");
  if (tcc_ir_opt_pack64_tautology(ir) > 0)
  {
    if (tcc_state->opt_copy_prop)
    {
      tcc_ir_opt_var_tmp_fwd(ir);
    }
  }
  DUMP_IR_AFTER_PASS(ir, "ZZ2_p64t");

  dbg_scan_overlap(ir, "P3-before-cmp_narrow_64");
  dbg_scan_overlap(ir, "R4-just-before-cmp_narrow");
  DUMP_IR_AFTER_PASS(ir, "ZZ2_lge");
  tcc_ir_opt_cmp_narrow_64(ir);

  dbg_scan_overlap(ir, "P4-before-assign_fuse");
  tcc_ir_opt_assign_fuse(ir);
  dbg_scan_overlap(ir, "P4b-after-assign_fuse");
  DUMP_IR_AFTER_PASS(ir, "ZZ2_af");

  tcc_ir_opt_select(ir);
  dbg_scan_overlap(ir, "P5-after-select");

  if (tcc_state->optimize > 0)
    tcc_ir_opt_setif_neg_to_select(ir);
  DUMP_IR_AFTER_PASS(ir, "ZZ2_sel");
}

/* ================================================================== */
/*  Compute function properties: global reads, memcpy/memmove usage   */
/* ================================================================== */
static void analyze_function_properties(TCCIRState *ir, Sym *sym, int func_var,
                                         int *nonstatic_global_copier_out)
{
  (void)func_var;
  nocode_wanted = 0;

  int had_aggr_copy = 0;
  int reads_global = 0;
  int has_params = ir && ir->parameters_count > 0;
  if (ir)
  {
    for (int ii = 0; ii < ir->next_instruction_index; ii++)
    {
      IRQuadCompact *q = &ir->compact_instructions[ii];
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      {
        Sym *cs = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
        const char *cn = cs ? get_tok_str(cs->v, NULL) : NULL;
        if (cn && (strstr(cn, "memmove") || strstr(cn, "memcpy")))
          had_aggr_copy = 1;
      }
      if (!reads_global &&
          (irop_config[q->op].has_dest || irop_config[q->op].has_src1 || irop_config[q->op].has_src2))
      {
        int call_callee = (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID);
        IROperand gops[3];
        gops[0] = tcc_ir_op_get_dest(ir, q);
        gops[1] = call_callee ? IROP_NONE : tcc_ir_get_src1(ir, ii);
        gops[2] = tcc_ir_get_src2(ir, ii);
        for (int j = 0; j < 3; j++)
          if (gops[j].is_sym && !gops[j].is_local)
          {
            reads_global = 1;
            break;
          }
      }
    }
  }
  int nonstatic_global_copier =
      had_aggr_copy && reads_global && has_params && sym && !(sym->type.t & VT_STATIC);
  if (nonstatic_global_copier_out)
    *nonstatic_global_copier_out = nonstatic_global_copier;
}

/* ================================================================== */
/*  Main entry point — target-independent optimization only.          */
/*  Register allocation, stack layout and codegen prep are driven by   */
/*  gen_function (source/backend/generators/), which calls the backend */
/*  RA pipeline after this returns.                                    */
/* ================================================================== */
void tcc_ir_opt_run_function_pipeline(TCCIRState *ir, Sym *sym, int func_var,
                                      int *nonstatic_global_copier_out)
{
#ifdef CONFIG_TCC_DEBUG
  if (tcc_state->dump_ir)
  {
    tcc_ir_dump_set_show_physical_regs(0); /* Show only virtual registers */
    printf("=== IR BEFORE OPTIMIZATIONS ===\n");
    tcc_ir_show(ir);
    printf("=== END IR BEFORE OPTIMIZATIONS ===\n");
  }
#endif

  if (tcc_state->opt_dead_store && !tcc_state->ir_late_reopt_phase)
    tcc_ir_collect_tu_static_reads_preopt(ir);

  run_propagation_passes(ir);
  run_opt_pipeline(ir);
  run_store_load_fwd_passes(ir);
  run_dead_store_and_cleanup(ir);
  run_post_pipeline_passes(ir);

  analyze_function_properties(ir, sym, func_var, nonstatic_global_copier_out);

  if (tcc_state->optimize > 0 && tcc_state->opt_redundant_store)
  {
    tcc_ir_opt_memmove_global_load_fwd(ir);
  }
}

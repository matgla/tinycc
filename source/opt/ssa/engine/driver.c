/*
 *  TCC IR - SSA optimization pass driver
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "opt.h"
#include "ssa_opt.h"
#include "opt/ssa/branch.h"
#include "const_string_fold.h"
#include "bitop_const_fold.h"
#include "load_cse.h"
#include "diamond_store_fwd.h"
#include "opt/ssa/strength.h"
#include "opt/ssa/reassoc.h"
#include "opt/ssa/fold.h"
#include "opt/ssa/cmp_eq.h"
#include "opt/ssa/vrp.h"
#include "opt/ssa/setif_or_taut.h"
#include "opt/ssa/bool_norm.h"
#include "opt/ssa/cmp_offset_fold.h"
#include "opt/ssa/tmp_block_const.h"
#include "opt/ssa/gvn.h"
#include "opt/ssa/var_imm_prop.h"
#include "opt/ssa/cprop.h"


static const IRSSAOptGen *target_gens;
static int target_gen_count;

void tcc_ir_ssa_opt_register_target(const IRSSAOptGen *gens, int count)
{
  target_gens = gens;
  target_gen_count = count;
}

int ssa_opt_run_gens(IRSSAOptCtx *ctx, const IRSSAOptGen *gens, int count)
{
  TCCIRState *ir = ctx->ir;
  int changes = 0;

  for (int i = 0; i < ir->next_instruction_index; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_NOP)
      continue;
    for (int g = 0; g < count; g++) {
      if (gens[g].op == op) {
        changes += gens[g].fn(ctx, i);
        break;
      }
    }
  }

  return changes;
}

int tcc_ir_ssa_opt_run(IRSSAOptCtx *ctx)
{
  int total = 0;
  int iteration = 0;
  const int max_iterations = 5;
  int changes;

  /* Every pass must stay observable via -dump-ir-passes=<name> and, under
   * TCC_PASS_TIMING=1, in the per-pass timing dump. */
#define SSA_RUN(name, call)                                                                                            \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!tcc_ir_opt_pass_disabled(name))                                                                               \
    {                                                                                                                  \
      int _c;                                                                                                          \
      TCC_PASS_TIMED(_c, name, (call));                                                                                \
      changes += _c;                                                                                                   \
    }                                                                                                                  \
    dbg_scan_imm_dest(ctx->ir, name);                                                                                  \
    tcc_ir_dump_after_pass(ctx->ir, name);                                                                             \
  } while (0)

  do {
    changes = 0;
    iteration++;

    SSA_RUN("ssa:var_const_fold", ssa_opt_var_const_fold(ctx));
    SSA_RUN("ssa:sccp", ssa_opt_sccp(ctx));
    SSA_RUN("ssa:cprop", ssa_opt_cprop(ctx));
    SSA_RUN("ssa:var_to_param_forward", ssa_opt_var_to_param_forward(ctx));
    SSA_RUN("ssa:tmp_block_const",
            (tcc_state && tcc_state->opt_const_prop) ? ssa_opt_tmp_block_const(ctx) : 0);
    SSA_RUN("ssa:fold", ssa_opt_fold(ctx));
    SSA_RUN("ssa:cprop", ssa_opt_cprop(ctx));
    SSA_RUN("ssa:var_imm_prop", ssa_opt_var_imm_prop(ctx));
    SSA_RUN("ssa:const_prop_tmp", ssa_opt_const_prop_tmp(ctx));
    SSA_RUN("ssa:load_cse", ssa_opt_load_cse(ctx));
    SSA_RUN("ssa:diamond_store_fwd", ssa_opt_diamond_store_fwd(ctx));
    SSA_RUN("ssa:const_string_fold", tcc_ir_ssa_opt_const_string_fold(ctx));
    SSA_RUN("ssa:bitop_const_fold", tcc_ir_ssa_opt_bitop_const_fold(ctx));
    SSA_RUN("ssa:ptr_store_dse", tcc_ir_ssa_opt_ptr_store_dse(ctx));
    SSA_RUN("ssa:branch", ssa_opt_branch(ctx));
    SSA_RUN("ssa:cmp_eq_prop", ssa_opt_cmp_eq_prop(ctx));
    SSA_RUN("ssa:vrp", (tcc_state && tcc_state->opt_vrp) ? ssa_opt_vrp(ctx) : 0);
    SSA_RUN("ssa:setif_or_taut",
            (tcc_state && tcc_state->opt_const_prop) ? ssa_opt_setif_or_taut(ctx) : 0);
    SSA_RUN("ssa:setif_mask_fold",
            (tcc_state && tcc_state->opt_const_prop) ? ssa_opt_setif_mask_fold(ctx) : 0);
    SSA_RUN("ssa:cmp_offset_fold",
            (tcc_state && tcc_state->opt_const_prop) ? ssa_opt_cmp_offset_fold(ctx) : 0);
    SSA_RUN("ssa:reassoc", ssa_opt_reassoc(ctx));
    SSA_RUN("ssa:strength", ssa_opt_strength(ctx));
    SSA_RUN("ssa:narrow", ssa_opt_narrow(ctx));
    SSA_RUN("ssa:gvn", ssa_opt_gvn(ctx));
    SSA_RUN("ssa:phi_simplify", ssa_opt_phi_simplify(ctx));
    /* -O2 only: -O1 keeps empty counting loops intact. */
    SSA_RUN("ssa:dead_loop",
            (tcc_state && tcc_state->optimize >= 2) ? ssa_opt_dead_loop(ctx) : 0);
    SSA_RUN("ssa:dce", ssa_opt_dce(ctx));

    if (target_gens && target_gen_count > 0) {
      int _c;
      TCC_PASS_TIMED(_c, "ssa:target_gens",
                     ssa_opt_run_gens(ctx, target_gens, target_gen_count));
      changes += _c;
    }

    total += changes;
  } while (changes > 0 && iteration < max_iterations);
#undef SSA_RUN

  /* AFTER the main loop, never inside it: bool_norm deletes the `CMP b,#0`
   * that setif_mask_fold matches on, so running the two together lets whichever
   * fires first in an iteration rob the other of its shape. */
  if (tcc_state && tcc_state->opt_const_prop &&
      !tcc_ir_opt_pass_disabled("ssa:bool_norm")) {
    int _c;
    TCC_PASS_TIMED(_c, "ssa:bool_norm", ssa_opt_bool_norm(ctx));
    tcc_ir_dump_after_pass(ctx->ir, "ssa:bool_norm");
    total += _c;
  }

  /* Convergence tail: branch folds late in the main loop expose
   * reaching-constant chains the capped loop never revisits; this cheap
   * quintet finishes them off (304_fuzz whole-program fold).  Must run BEFORE
   * guard_collapse, which is the pipeline terminator. */
  for (int tail = 0; tail < 8 && !tcc_ir_opt_pass_disabled("ssa:late_tail"); tail++) {
    int ch = 0;
    if (tcc_state && tcc_state->opt_const_prop &&
        !tcc_ir_opt_pass_disabled("ssa:tmp_block_const")) {
      int _c;
      TCC_PASS_TIMED(_c, "ssa:tmp_block_const", ssa_opt_tmp_block_const(ctx));
      tcc_ir_dump_after_pass(ctx->ir, "ssa:tmp_block_const");
      ch += _c;
    }
    if (!tcc_ir_opt_pass_disabled("ssa:fold")) {
      int _c;
      TCC_PASS_TIMED(_c, "ssa:fold", ssa_opt_fold(ctx));
      ch += _c;
    }
    if (!tcc_ir_opt_pass_disabled("ssa:cprop")) {
      int _c;
      TCC_PASS_TIMED(_c, "ssa:cprop", ssa_opt_cprop(ctx));
      ch += _c;
    }
    if (!tcc_ir_opt_pass_disabled("ssa:branch")) {
      int _c;
      TCC_PASS_TIMED(_c, "ssa:branch", ssa_opt_branch(ctx));
      ch += _c;
    }
    if (!tcc_ir_opt_pass_disabled("ssa:dce")) {
      int _c;
      TCC_PASS_TIMED(_c, "ssa:dce", ssa_opt_dce(ctx));
      ch += _c;
    }
    total += ch;
    if (!ch)
      break;
  }

  {
    int _c;
    TCC_PASS_TIMED(_c, "ssa:guard_collapse", tcc_ir_ssa_opt_guard_collapse_ex(ctx, 1));
    total += _c;
  }

  return total;
}

int tcc_ir_ssa_opt_run_target(IRSSAOptCtx *ctx)
{
  if (!target_gens || target_gen_count <= 0)
    return 0;
  int total = 0;
  for (int iter = 0; iter < 3; iter++) {
    int changes = ssa_opt_run_gens(ctx, target_gens, target_gen_count);
    if (changes == 0)
      break;
    total += changes;
    /* DCE removes instructions we NOP'd; rerun cprop to clean up new copies. */
    ssa_opt_cprop(ctx);
    ssa_opt_dce(ctx);
  }
  return total;
}

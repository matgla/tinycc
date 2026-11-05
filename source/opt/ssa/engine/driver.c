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
#include "opt/ssa/cprop.h"
#include "opt/ssa/dup_diamond.h"


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
  /* Every instruction asked every pattern for its opcode. */
  short first[TCCIR_OP_COUNT];
  memset(first, -1, sizeof first);
  for (int g = count - 1; g >= 0; g--)
    if (gens[g].op >= 0 && gens[g].op < TCCIR_OP_COUNT)
      first[gens[g].op] = (short)g;

  for (int i = 0; i < ir->next_instruction_index; i++) {
    int op = ir->compact_instructions[i].op;
    if (op != TCCIR_OP_NOP && first[op] >= 0)
      changes += gens[first[op]].fn(ctx, i);
  }

  return changes;
}

static void ssa_orphan_cmp_nop(void *opaque, int i)
{
  ssa_opt_nop_instr(opaque, i);
}

/* One slot per pass for the rerun tracker; passes listed twice share one. */
enum
{
  SSA_SLOT_var_forward,
  SSA_SLOT_sccp,
  SSA_SLOT_cprop,
  SSA_SLOT_var_to_param_forward,
  SSA_SLOT_tmp_block_const,
  SSA_SLOT_fold,
  SSA_SLOT_const_prop_tmp,
  SSA_SLOT_load_cse,
  SSA_SLOT_diamond_store_fwd,
  SSA_SLOT_const_string_fold,
  SSA_SLOT_bitop_const_fold,
  SSA_SLOT_ptr_store_dse,
  SSA_SLOT_ptr_silent_store,
  SSA_SLOT_branch,
  SSA_SLOT_switch_fold,
  SSA_SLOT_cmp_eq_prop,
  SSA_SLOT_vrp,
  SSA_SLOT_setif_mask_fold,
  SSA_SLOT_cmp_offset_fold,
  SSA_SLOT_reassoc,
  SSA_SLOT_strength,
  SSA_SLOT_narrow,
  SSA_SLOT_gvn,
  SSA_SLOT_dup_diamond,
  SSA_SLOT_pure_call_cse,
  SSA_SLOT_phi_simplify,
  SSA_SLOT_dead_loop,
  SSA_SLOT_stack_deref_fold,
  SSA_SLOT_copy_fwd,
  SSA_SLOT_load_combine,
  SSA_SLOT_orphan_cmp,
  SSA_SLOT_dce,
  SSA_SLOT_bool_norm,
  SSA_SLOT_target_gens,
  SSA_SLOT_COUNT
};

/* What a schedule entry is gated on, read when it is about to run. */
enum
{
  SG_ALWAYS,
  SG_CONST_PROP, /* -fopt-const-prop */
  SG_VRP,        /* -fopt-vrp */
  SG_O1,         /* optimize > 0 */
  SG_O2,         /* optimize >= 2 */
  SG_O1_SLFWD    /* optimize > 0 and -fopt-store-load-fwd */
};

static int ssa_gate_open(int gate)
{
  if (!tcc_state)
    return gate == SG_ALWAYS;
  switch (gate)
  {
  case SG_CONST_PROP: return TCC_OPT(tcc_state, opt_const_prop) != 0;
  case SG_VRP: return TCC_OPT(tcc_state, opt_vrp) != 0;
  case SG_O1: return TCC_OPT(tcc_state, optimize) > 0;
  case SG_O2: return TCC_OPT(tcc_state, optimize) >= 2;
  case SG_O1_SLFWD: return TCC_OPT(tcc_state, optimize) > 0 && TCC_OPT(tcc_state, opt_store_load_fwd);
  default: return 1;
  }
}

typedef struct SsaPass
{
  uint8_t slot, gate;
  const char *name;
  int (*fn)(IRSSAOptCtx *ctx);
} SsaPass;

/* The rerun tracker: a pass that changed nothing cannot change anything until
 * some other pass has touched the IR -- run clean at generation G, it skips
 * while the generation is G.  Generation 0 is "has not run". */
typedef struct SsaRun
{
  uint32_t generation;
  uint32_t clean_at[SSA_SLOT_COUNT];
} SsaRun;

/* Runs one pass under its gate, the timer and the -dump-ir-passes hook.  A
 * skipped or gated-off pass still dumps: every pass stays observable via
 * -dump-ir-passes=<name> and the golden-IR harness sees what running it would
 * have shown. */
static int ssa_run_one(IRSSAOptCtx *ctx, SsaRun *r, const SsaPass *p)
{
  int c = 0;
  if (r->clean_at[p->slot] != r->generation && !tcc_ir_opt_pass_disabled(p->name))
  {
    TCC_PASS_TIMED(c, p->name, ssa_gate_open(p->gate) ? p->fn(ctx) : 0);
    if (c > 0)
      r->generation++;
    else
      r->clean_at[p->slot] = r->generation;
  }
  dbg_scan_imm_dest(ctx->ir, p->name);
  tcc_ir_dump_after_pass(ctx->ir, p->name);
  return c;
}

static int ssa_run_list(IRSSAOptCtx *ctx, SsaRun *r, const SsaPass *list, int n)
{
  int changes = 0;
  for (int i = 0; i < n; i++)
    changes += ssa_run_one(ctx, r, &list[i]);
  return changes;
}

static int ssa_run_orphan_cmp(IRSSAOptCtx *ctx)
{
  return tcc_ir_opt_orphan_cmp_elim_ex(ctx->ir, ssa_orphan_cmp_nop, ctx);
}

static int ssa_run_target_gens(IRSSAOptCtx *ctx)
{
  return ssa_opt_run_gens(ctx, target_gens, target_gen_count);
}

#define P(slot, gate, nm, f) { SSA_SLOT_##slot, gate, nm, f }

/* The schedule for a body with promoted variables (phis present). */
static const SsaPass ssa_main_passes[] = {
  P(sccp, SG_ALWAYS, "ssa:sccp", ssa_opt_sccp),
  P(cprop, SG_ALWAYS, "ssa:cprop", ssa_opt_cprop),
  P(var_to_param_forward, SG_ALWAYS, "ssa:var_to_param_forward", ssa_opt_var_to_param_forward),
  P(tmp_block_const, SG_CONST_PROP, "ssa:tmp_block_const", ssa_opt_tmp_block_const),
  P(fold, SG_ALWAYS, "ssa:fold", ssa_opt_fold),
  P(cprop, SG_ALWAYS, "ssa:cprop", ssa_opt_cprop),
  P(const_prop_tmp, SG_ALWAYS, "ssa:const_prop_tmp", ssa_opt_const_prop_tmp),
  P(load_cse, SG_ALWAYS, "ssa:load_cse", ssa_opt_load_cse),
  P(diamond_store_fwd, SG_ALWAYS, "ssa:diamond_store_fwd", ssa_opt_diamond_store_fwd),
  P(const_string_fold, SG_ALWAYS, "ssa:const_string_fold", tcc_ir_ssa_opt_const_string_fold),
  P(bitop_const_fold, SG_ALWAYS, "ssa:bitop_const_fold", tcc_ir_ssa_opt_bitop_const_fold),
  P(ptr_store_dse, SG_ALWAYS, "ssa:ptr_store_dse", tcc_ir_ssa_opt_ptr_store_dse),
  P(ptr_silent_store, SG_ALWAYS, "ssa:ptr_silent_store", tcc_ir_ssa_opt_ptr_silent_store),
  P(branch, SG_ALWAYS, "ssa:branch", ssa_opt_branch),
  P(switch_fold, SG_ALWAYS, "ssa:switch_fold", ssa_opt_switch_fold),
  P(cmp_eq_prop, SG_ALWAYS, "ssa:cmp_eq_prop", ssa_opt_cmp_eq_prop),
  P(vrp, SG_VRP, "ssa:vrp", ssa_opt_vrp),
  P(setif_mask_fold, SG_CONST_PROP, "ssa:setif_mask_fold", ssa_opt_setif_mask_fold),
  P(cmp_offset_fold, SG_CONST_PROP, "ssa:cmp_offset_fold", ssa_opt_cmp_offset_fold),
  P(reassoc, SG_ALWAYS, "ssa:reassoc", ssa_opt_reassoc),
  P(strength, SG_ALWAYS, "ssa:strength", ssa_opt_strength),
  P(narrow, SG_ALWAYS, "ssa:narrow", ssa_opt_narrow),
  P(gvn, SG_ALWAYS, "ssa:gvn", ssa_opt_gvn),
  P(dup_diamond, SG_ALWAYS, "ssa:dup_diamond", ssa_opt_dup_diamond),
  P(pure_call_cse, SG_ALWAYS, "ssa:pure_call_cse", ssa_opt_pure_call_cse),
  P(phi_simplify, SG_ALWAYS, "ssa:phi_simplify", ssa_opt_phi_simplify),
  /* -O2, not -O1: gcc deletes a pure counting loop only at -O2 -- its -O1
   * folds the body to a constant but still runs the trip count.  Gating this
   * at -O1 made our -O1 strictly stronger than its gcc peer, to the point
   * that bench_conditionals compiled byte-identically at -O1 and -O2 here
   * while gcc -O1 spent 4034 cycles on an empty loop.  Deleting the loop
   * belongs to the heavy tier alongside unrolling/LICM/IV-strength-red. */
  P(dead_loop, SG_O2, "ssa:dead_loop", ssa_opt_dead_loop),
  P(stack_deref_fold, SG_O1, "ssa:stack_deref_fold", ssa_opt_stack_deref_fold),
  /* After stack_deref_fold: a field read through `t1 = &t0` is a StackLoc only now. */
  P(copy_fwd, SG_O1_SLFWD, "ssa:copy_fwd", ssa_opt_copy_fwd),
  P(load_combine, SG_O2, "ssa:load_combine", ssa_opt_load_combine),
  P(orphan_cmp, SG_ALWAYS, "ssa:orphan_cmp", ssa_run_orphan_cmp),
  P(dce, SG_ALWAYS, "ssa:dce", ssa_opt_dce),
};
static const SsaPass ssa_target_pass = P(target_gens, SG_ALWAYS, "ssa:target_gens", ssa_run_target_gens);

/* The convergence tail: branch folds late in the main loop expose
 * reaching-constant chains the capped loop never revisits; this cheap
 * quintet finishes them off (304_fuzz whole-program fold). */
static const SsaPass ssa_tail_passes[] = {
  P(tmp_block_const, SG_CONST_PROP, "ssa:tmp_block_const", ssa_opt_tmp_block_const),
  P(fold, SG_ALWAYS, "ssa:fold", ssa_opt_fold),
  P(cprop, SG_ALWAYS, "ssa:cprop", ssa_opt_cprop),
  P(branch, SG_ALWAYS, "ssa:branch", ssa_opt_branch),
  P(dce, SG_ALWAYS, "ssa:dce", ssa_opt_dce),
};

/* The schedule when nothing was promotable (every variable address-taken):
 * the passes that operate on TEMP vregs without phi nodes, with the narrow
 * ssa:var_forward in place of ssa:var_to_param_forward and no loop passes. */
static const SsaPass ssa_fallback_passes[] = {
  P(var_forward, SG_ALWAYS, "ssa:var_forward", ssa_opt_var_forward),
  P(sccp, SG_ALWAYS, "ssa:sccp", ssa_opt_sccp),
  P(load_cse, SG_ALWAYS, "ssa:load_cse", ssa_opt_load_cse),
  P(diamond_store_fwd, SG_ALWAYS, "ssa:diamond_store_fwd", ssa_opt_diamond_store_fwd),
  P(const_string_fold, SG_ALWAYS, "ssa:const_string_fold", tcc_ir_ssa_opt_const_string_fold),
  P(bitop_const_fold, SG_ALWAYS, "ssa:bitop_const_fold", tcc_ir_ssa_opt_bitop_const_fold),
  P(ptr_store_dse, SG_ALWAYS, "ssa:ptr_store_dse", tcc_ir_ssa_opt_ptr_store_dse),
  P(ptr_silent_store, SG_ALWAYS, "ssa:ptr_silent_store", tcc_ir_ssa_opt_ptr_silent_store),
  P(cprop, SG_ALWAYS, "ssa:cprop", ssa_opt_cprop),
  P(fold, SG_ALWAYS, "ssa:fold", ssa_opt_fold),
  P(const_prop_tmp, SG_ALWAYS, "ssa:const_prop_tmp", ssa_opt_const_prop_tmp),
  P(branch, SG_ALWAYS, "ssa:branch", ssa_opt_branch),
  P(vrp, SG_VRP, "ssa:vrp", ssa_opt_vrp),
  P(setif_mask_fold, SG_CONST_PROP, "ssa:setif_mask_fold", ssa_opt_setif_mask_fold),
  P(cmp_offset_fold, SG_CONST_PROP, "ssa:cmp_offset_fold", ssa_opt_cmp_offset_fold),
  P(reassoc, SG_ALWAYS, "ssa:reassoc", ssa_opt_reassoc),
  P(strength, SG_ALWAYS, "ssa:strength", ssa_opt_strength),
  P(narrow, SG_ALWAYS, "ssa:narrow", ssa_opt_narrow),
  P(gvn, SG_ALWAYS, "ssa:gvn", ssa_opt_gvn),
  P(dup_diamond, SG_ALWAYS, "ssa:dup_diamond", ssa_opt_dup_diamond),
  P(phi_simplify, SG_ALWAYS, "ssa:phi_simplify", ssa_opt_phi_simplify),
  P(copy_fwd, SG_O1_SLFWD, "ssa:copy_fwd", ssa_opt_copy_fwd),
  P(load_combine, SG_O2, "ssa:load_combine", ssa_opt_load_combine),
  P(dce, SG_ALWAYS, "ssa:dce", ssa_opt_dce),
};
static const SsaPass ssa_bool_norm_pass = P(bool_norm, SG_ALWAYS, "ssa:bool_norm", ssa_opt_bool_norm);
#undef P

#define SSA_COUNTOF(a) ((int)(sizeof(a) / sizeof((a)[0])))

int tcc_ir_ssa_opt_run(IRSSAOptCtx *ctx)
{
  int total = 0;
  int iteration = 0;
  const int max_iterations = 5;
  int changes;
  SsaRun r = {1, {0}};

  do {
    iteration++;
    changes = ssa_run_list(ctx, &r, ssa_main_passes, SSA_COUNTOF(ssa_main_passes));
    if (target_gens && target_gen_count > 0)
      changes += ssa_run_one(ctx, &r, &ssa_target_pass);
    total += changes;
  } while (changes > 0 && iteration < max_iterations);

  /* AFTER the main loop, never inside it: bool_norm deletes the `CMP b,#0`
   * that setif_mask_fold matches on, so running the two together lets whichever
   * fires first in an iteration rob the other of its shape. */
  if (tcc_state && TCC_OPT(tcc_state, opt_const_prop))
    total += ssa_run_one(ctx, &r, &ssa_bool_norm_pass);

  /* The convergence tail must run BEFORE guard_collapse, the pipeline terminator. */
  for (int tail = 0; tail < 8 && !tcc_ir_opt_pass_disabled("ssa:late_tail"); tail++) {
    int ch = 0;
    for (int i = 0; i < SSA_COUNTOF(ssa_tail_passes); i++)
      if (ssa_tail_passes[i].gate == SG_ALWAYS || ssa_gate_open(ssa_tail_passes[i].gate))
        ch += ssa_run_one(ctx, &r, &ssa_tail_passes[i]);
    total += ch;
    if (!ch)
      break;
  }

  {
    int _c;
    const int known_clean = r.clean_at[SSA_SLOT_load_cse] == r.generation &&
                            r.clean_at[SSA_SLOT_cprop] == r.generation &&
                            r.clean_at[SSA_SLOT_fold] == r.generation &&
                            r.clean_at[SSA_SLOT_branch] == r.generation;
    TCC_PASS_TIMED(_c, "ssa:guard_collapse", tcc_ir_ssa_opt_guard_collapse_ex(ctx, 1, known_clean));
    total += _c;
  }

  return total;
}

int tcc_ir_ssa_opt_run_fallback(IRSSAOptCtx *ctx)
{
  int total = 0;
  SsaRun r = {1, {0}};
  ctx->no_stack_fwd = 0;
  for (int iter = 0; iter < 5; iter++) {
    int changes = ssa_run_list(ctx, &r, ssa_fallback_passes, SSA_COUNTOF(ssa_fallback_passes));
    /* guard_collapse reports its folds but not every cleanup it may do, so
     * nothing is skipped across its rounds. */
    changes += tcc_ir_ssa_opt_guard_collapse(ctx);
    r.generation++;
    total += changes;
    if (!changes)
      break;
  }
  /* After the loop, never inside it: bool_norm deletes the `CMP b,#0` that
   * setif_mask_fold matches on. */
  if (tcc_state && TCC_OPT(tcc_state, opt_const_prop)) {
    int c = ssa_run_one(ctx, &r, &ssa_bool_norm_pass);
    total += c;
    if (c)
      total += ssa_opt_dce(ctx);
  }
  /* Target-specific fusions (MLA, LOAD/STORE_INDEXED on ARM): they
   * pattern-match on existing TEMP vregs, no phis needed. */
  total += tcc_ir_ssa_opt_run_target(ctx);
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

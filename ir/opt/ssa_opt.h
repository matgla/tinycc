/*
 *  TCC IR - SSA Optimization Engine
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_SSA_OPT_H
#define TCC_IR_SSA_OPT_H

#include "cfg.h"
#include "ssa.h"

struct TCCIRState;
struct IRQuadCompact;
typedef struct IRQuadCompact IRQuadCompact;

/* ============================================================================
 * SSA Use-Def Chains
 * ============================================================================ */

enum { SSA_USE_INSTR = 0, SSA_USE_PHI = 1 };

typedef struct IRSSAUse {
  int idx;       /* instruction index (INSTR) or block (PHI) */
  int slot;      /* phi operand slot (PHI only) */
  uint8_t kind;
} IRSSAUse;

typedef struct IRSSAVregInfo {
  int def_instr;      /* instruction index, -1 if phi/entry */
  int def_phi_block;  /* block ID if phi, -1 otherwise */
  int def_count;      /* number of definitions (>1 means non-SSA multi-def TEMP) */
  IRSSAUse *uses;
  int use_count;
  int use_cap;
} IRSSAVregInfo;

/* ============================================================================
 * Optimization Context
 * ============================================================================ */

typedef struct IRSSAOptCtx {
  struct TCCIRState *ir;
  IRSSAState *ssa;
  IRCFG *cfg;
  IRSSAVregInfo *vinfo;   /* indexed by TEMP vreg position */
  int vinfo_cap;
  int changes;
  int no_stack_fwd;       /* disable stack store-load forwarding in load_cse */
} IRSSAOptCtx;

/* ============================================================================
 * Generator: explicit per-opcode rewrite rule (like thop_* instruction builders)
 * ============================================================================ */

typedef int (*ssa_gen_fn)(IRSSAOptCtx *ctx, int instr_idx);

typedef struct IRSSAOptGen {
  int op;
  ssa_gen_fn fn;
  const char *name;
} IRSSAOptGen;

/* ============================================================================
 * Pass Descriptor
 * ============================================================================ */

typedef int (*ssa_pass_fn)(IRSSAOptCtx *ctx);

typedef struct IRSSAOptPass {
  const char *name;
  ssa_pass_fn run;              /* custom pass function, or NULL */
  const IRSSAOptGen *gens;      /* generator table, or NULL */
  int gen_count;
} IRSSAOptPass;

/* ============================================================================
 * Driver API
 * ============================================================================ */

void tcc_ir_ssa_opt_init(IRSSAOptCtx *ctx, struct TCCIRState *ir,
                         IRSSAState *ssa, IRCFG *cfg);
void tcc_ir_ssa_opt_rebuild(IRSSAOptCtx *ctx);
void tcc_ir_ssa_opt_free(IRSSAOptCtx *ctx);
int tcc_ir_ssa_opt_run(IRSSAOptCtx *ctx);
/* Fixpoint over {load_cse, cprop, fold, branch, light dce} peeling
 * sequential constant-guard chains one folded branch per round. */
int tcc_ir_ssa_opt_guard_collapse(IRSSAOptCtx *ctx);
/* Same-block store-store DSE for full-width stores through single-def
 * TEMP pointer derefs (bitfield RMW chains after store→load forwarding). */
int tcc_ir_ssa_opt_ptr_store_dse(IRSSAOptCtx *ctx);
/* Run only the target-specific generators registered via
 * tcc_ir_ssa_opt_register_target. Iterates a few times for convergence. */
int tcc_ir_ssa_opt_run_target(IRSSAOptCtx *ctx);

/* Run a generator table over all instructions */
int ssa_opt_run_gens(IRSSAOptCtx *ctx, const IRSSAOptGen *gens, int count);

/* ============================================================================
 * Use-Def Helpers
 * ============================================================================ */

IRSSAVregInfo *ssa_opt_vinfo(IRSSAOptCtx *ctx, int32_t vreg);
void ssa_opt_add_use_instr(IRSSAVregInfo *vi, int instr_idx);
void ssa_opt_add_use_phi(IRSSAVregInfo *vi, int block, int slot);
/* Append use-list entries for every vreg `q` (at index i) reads — same rules
 * as the init-time scan (src1/src2, MLA accum, memory-write STORE dest). */
void ssa_opt_scan_instr_uses(IRSSAOptCtx *ctx, int i, IRQuadCompact *q);
void ssa_opt_remove_use_instr(IRSSAVregInfo *vi, int instr_idx);
void ssa_opt_nop_instr(IRSSAOptCtx *ctx, int idx);
int ssa_opt_can_replace_all_uses(IRSSAOptCtx *ctx, int32_t old_vr);
int ssa_opt_replace_all_uses(IRSSAOptCtx *ctx, int32_t old_vr, int32_t new_vr);
int ssa_opt_has_side_effects(int op);

/* ============================================================================
 * Target-Independent Passes
 * ============================================================================ */

int ssa_opt_dce(IRSSAOptCtx *ctx);
/* Unreachable-code sweep + dead-TEMP worklist only — no CFG rebuild, safe
 * to call every guard_collapse round. */
int ssa_opt_dce_light(IRSSAOptCtx *ctx);
int ssa_opt_cprop(IRSSAOptCtx *ctx);
/* ssa_opt_fold() moved to source/opt/ssa/include/opt/ssa/fold.h */
int ssa_opt_phi_simplify(IRSSAOptCtx *ctx);
/* ssa_opt_strength() moved to source/opt/ssa/include/opt/ssa/strength.h */
int ssa_opt_gvn(IRSSAOptCtx *ctx);
int ssa_opt_reassoc(IRSSAOptCtx *ctx);
int ssa_opt_narrow(IRSSAOptCtx *ctx);
/* Block reachability from current IR terminators (the static CFG is stale
 * after branch folds).  malloc'd array [num_blocks], caller frees. */
uint8_t *ssa_opt_compute_reachable_blocks(IRSSAOptCtx *ctx);
int ssa_opt_sccp(IRSSAOptCtx *ctx);
int ssa_opt_load_cse(IRSSAOptCtx *ctx);
int ssa_opt_var_forward(IRSSAOptCtx *ctx);

/* Forward single-def, single-use, non-address-taken VARs into their lone
 * FUNCPARAMVAL use site, NOPing the original STORE.  Narrow companion to
 * ssa_opt_var_forward: only the PARAM-use case (collapses inlined-helper
 * printf-arg materialisation) and skips deref sources so as not to expose
 * SCCP stack-load alias issues. */
int ssa_opt_var_to_param_forward(IRSSAOptCtx *ctx);
int ssa_opt_var_const_fold(IRSSAOptCtx *ctx);
int ssa_opt_var_imm_prop(IRSSAOptCtx *ctx);
/* SSA-time analog of the flat block-local const_prop_tmp (TMP/VAR constant
 * tracking + propagation, two-operand fold, SWITCH_TABLE const-index → JUMP,
 * CMP+SETIF and soft-FP compare folds). */
int ssa_opt_const_prop_tmp(IRSSAOptCtx *ctx);
int ssa_opt_dead_loop(IRSSAOptCtx *ctx);

/* Loop rotation (ssa:loop_rotate).  CFG/dominator-based natural-loop detection
 * driving the flat-IR rewrite; runs on flat IR just before CFG/SSA
 * construction, so it takes the raw IR state rather than an IRSSAOptCtx.
 * Returns the number of loops rotated.  See ir/opt/ssa_opt_loop.c. */
int ssa_opt_loop_rotate(struct TCCIRState *ir);

/* First-iteration-exit peeling (ssa:first_iter_exit).  Eliminates top-tested
 * natural loops whose header exit test is provably TRUE on first entry
 * (straight-line entry walk over VAR/TEMP constants and &VAR pointers).
 * Runs on flat IR immediately after ssa_opt_loop_rotate, before CFG/SSA
 * construction.  Returns the number of loops eliminated.
 * See ir/opt/ssa_opt_loop.c and docs/plan_legacy_loop_dead_first_iter_ssa.md. */
int ssa_opt_first_iter_exit(struct TCCIRState *ir);

/* Pointer-IV exit-value substitution (ssa:ptr_iv_exit_subst).  For counted
 * loops with known trip count N > 0: rewrites post-loop pointer-IV reads to
 * `Addr[StackLoc[X + step*N]]`, NOPs zero-trip entry guards, folds the
 * substituted CMP+JUMPIF consumers (`p != &a[N]`).  Flat IR, right after
 * ssa_opt_first_iter_exit.  Returns substitutions + control-flow changes.
 * See docs/plan_legacy_loop_ptr_iv_exit_subst_ssa.md. */
int ssa_opt_ptr_iv_exit_subst(struct TCCIRState *ir);

/* Loop constant simulation (ssa:loop_const_sim).  Collapses register-only
 * natural loops with a bounded trip count by executing the body on the host
 * and emitting residual final-value ASSIGN/STOREs.  Dominance-verified
 * candidates + contiguity/single-entry facts; reuses the shared engine
 * (lcs_fold_region).  Flat IR, right after ssa_opt_ptr_iv_exit_subst.
 * Gated by opt_loop_unroll (-O2).  See docs/plan_legacy_loop_const_sim_ssa.md. */
int ssa_opt_loop_const_sim(struct TCCIRState *ir);

/* Loop unrolling / constant-trip elimination (ssa:loop_unroll).  Per natural
 * loop (innermost-first), runs the shared mutators try_eliminate_loop ->
 * try_eliminate_loop_symbolic -> try_unroll_loop_ex on a synthetic contiguous
 * single-entry IRLoop built from CFG facts.  Flat IR, right after
 * ssa_opt_loop_const_sim.  Gated by opt_loop_unroll (-O2).
 * See docs/plan_legacy_loop_unroll_ssa.md. */
int ssa_opt_loop_unroll(struct TCCIRState *ir);

/* Decrement-to-zero (ssa:decrement_to_zero).  Rewrites count-up pure-counter
 * loops with a separate pre-test guard (produced by ssa:loop_rotate, left by
 * const_sim/unroll) into count-down-to-zero so the backend fuses the latch
 * SUB+CMP#0 into a flag-setting SUBS.  Dominance-verified outermost natural
 * loops; reuses the engine dtz_try_region (ir/opt_loop_utils.c).  Flat IR,
 * right after ssa_opt_loop_unroll.  Gated -O1+ (matches ssa:loop_rotate, the
 * shape provider).  See docs/plan_legacy_loop_decrement_to_zero_ssa.md. */
int ssa_opt_decrement_to_zero(struct TCCIRState *ir);

/* Induction-variable strength reduction (ssa:iv_strength_reduction).  Transforms
 * array-indexing recurrences base + i*stride into a maintained stride pointer
 * and optionally eliminates the counter IV against a hoisted end pointer.  Per
 * outermost dominance-verified natural loop, builds a synthetic contiguous
 * single-entry IRLoop with a dense body_instrs range and runs the retained
 * transform engine iv_strength_reduction_core (ir/opt_loop_utils.c) on a
 * 1-element IRLoops wrapper.  Flat IR, after ssa_opt_loop_unroll and before
 * ssa_opt_decrement_to_zero (preserving the legacy "consumers after IV-SR"
 * order).  Gated -O1+ (matches the legacy -fiv-strength-red at -O1+).
 * See docs/plan_legacy_loop_iv_strength_reduction_ssa.md. */
int ssa_opt_iv_strength_reduction(struct TCCIRState *ir);

/* Identical-block loop re-rolling (ssa:reroll).  Scans the linear stream for
 * runs of N>=4 structurally-identical blocks (period 3..32, internal vregs
 * renamed consistently) and re-rolls them into a counted loop.  Not a natural-
 * loop transform: it creates a loop from straight-line code, so it has no CFG
 * detector — the thin driver just runs the retained engine tcc_ir_opt_reroll
 * (ir/opt_reroll.c) at the head of the flat region, post-propagation, where
 * foldable runs are already collapsed by downstream const-prop and only
 * non-foldable repetition survives to re-roll.  Gated opt_reroll (-O2).
 * See docs/plan_legacy_loop_reroll_ssa.md. */
int ssa_opt_reroll(struct TCCIRState *ir);

/* Symbol-address rematerialization CSE (ssa:symaddr_cse).  Hoists the address
 * of a global symbol referenced inline (as the SYMREF src1 of >=3 ADDs, or the
 * lval-SYMREF base of an entry-block STORE) into a single TEMP materialized at
 * function entry, replacing the repeated `ldr rN,[pc,#off]` literal-pool loads
 * with reads of that TEMP (and folding qualifying STOREs to STORE_INDEXED off
 * the hoisted base).  Runs on flat IR just before CFG/SSA construction — the
 * shared def it creates has no pre-existing instruction for gvn to number, so
 * it is a materialization rule rather than a value-numbering match; the SSA
 * block-local reuse passes (ssa_opt_symref_operand_cse, cprop_symref_cse) then
 * pick up any remaining same-block deref uses.  Gated -O1+ (matches the retired
 * flat globalsym_cse).  See docs/plan_legacy_flat_ir_ssa_retire.md. */
int ssa_opt_symaddr_cse(struct TCCIRState *ir);

/* OR-bool-diamond (ssa:or_bool_diamond).  Folds the `acc |= (cond ? 1 : 0)`
 * stack-slot materialization into per-arm ORs (engine in ir/opt_branch.c).
 * Flat IR, right after ssa:cfg_cleanup, whose eliminate_fallthrough creates
 * the required STORE-slot/OR adjacency.  Gated opt_const_prop (-O1+). */
int ssa_opt_or_bool_diamond(struct TCCIRState *ir);

/* Drop phi operands flowing from dead_pred_block into phis at target_block_idx.
 * Used after folding/eliminating an edge so that phi resolution does not emit
 * copies for the dead path. */
void ssa_drop_phi_edge(IRSSAOptCtx *ctx, int dead_pred_block, int target_block_idx);

/* Resolve a TEMP vreg backward to find if it's Addr[StackLoc[N]].  Chases
 * single-def LEA → ASSIGN copy chains.  Returns the stack offset, or INT_MIN
 * if the chain doesn't resolve to a stack address.  Multi-def TEMPs bail. */
int ssa_opt_resolve_lea_stackloc(IRSSAOptCtx *ctx, int32_t vr);

/* Like ssa_opt_resolve_lea_stackloc, but also reports the *identity* of the
 * resolved address through *out_base_var:
 *   -1  -> a real direct stack slot (vreg_type == 0); the returned offset is
 *          authoritative and uniquely names the slot.
 *   >=0 -> the address is `&VAR` (a scalar local addressed via its VAR/PARAM
 *          spill encoding).  At SSA time such locals have no assigned slot, so
 *          the returned offset is a placeholder (typically 0) SHARED by every
 *          distinct VAR — callers MUST disambiguate by *out_base_var, not by
 *          the offset alone, or they alias unrelated locals (ptr fuzz seed 67:
 *          &u2 and &u3 both -> offset 0).
 * out_base_var may be NULL.  It is set to -1 on an INT_MIN (unresolved) return. */
int ssa_opt_resolve_lea_stackloc_ex(IRSSAOptCtx *ctx, int32_t vr, int32_t *out_base_var);

/* Resolve a vreg backward to its canonical (base_vr, offset) form.  Chases
 * single-def ASSIGN copies and `T = base ADD #imm` chains until it lands
 * on a VAR/PARAM root (or a TEMP whose definition isn't a recognized copy
 * pattern).  Returns 1 with *out_base / *out_off populated on success; 0
 * otherwise.  Used by load-CSE to recognize that two TEMP pointers
 * (T9 = V1, T19 = V1) name the same memory, so reads through them can
 * share a result. */
int ssa_opt_resolve_temp_to_base_off(IRSSAOptCtx *ctx, int32_t vr,
                                      int32_t *out_base, int32_t *out_off);

/* Resolve the effective stack offset that a STORE / STORE_INDEXED / LOAD /
 * LOAD_INDEXED targets, when its base address is a TEMP that resolves to
 * Addr[StackLoc[N]].  For STORE_INDEXED and LOAD_INDEXED, the immediate index
 * (with scale=0) is added to the resolved base.  Returns INT_MIN when the
 * dest is not TEMP-DEREF or the LEA chain does not resolve, or the index
 * is not a constant with scale 0. */
int ssa_opt_indirect_stack_offset(IRSSAOptCtx *ctx, const IRQuadCompact *q, int side);
/* Variant that also reports the resolved address identity via *out_base_var
 * (see ssa_opt_resolve_lea_stackloc_ex for the -1 / >=0 contract). */
int ssa_opt_indirect_stack_offset_ex(IRSSAOptCtx *ctx, const IRQuadCompact *q, int side,
                                     int32_t *out_base_var);
#define SSA_OPT_INDIRECT_DEST 0  /* STORE / STORE_INDEXED dest base */
#define SSA_OPT_INDIRECT_SRC1 1  /* LOAD / LOAD_INDEXED source base */

/* ============================================================================
 * Target-Specific Generator Registration
 *
 * Backends call tcc_ir_ssa_opt_register_target() once at startup to provide
 * their generator table. The driver runs them as the last pass.
 * ============================================================================ */

void tcc_ir_ssa_opt_register_target(const IRSSAOptGen *gens, int count);

#endif /* TCC_IR_SSA_OPT_H */

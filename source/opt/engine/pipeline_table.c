/*
 *  TCC IR - Pass tables and the named pass groups
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
#include "opt/flat/self_arith.h"
#include "opt/flat/self_copy.h"
#include "opt/flat/var_tmp_fwd.h"
#include "opt_xform.h"
#include "const_string_fold.h"

#define FLAG(f) (uint16_t)offsetof(TCCState, f)

#ifdef CONFIG_TCC_O0_ONLY
/* Only gated table entries call these, and those carry no function in an
 * O0-only compiler (PASS_GATED below). */
static int tcc_ir_opt_known_bits_cascade_ex(IROptCtx *ctx) __attribute__((unused));
static int tcc_ir_opt_const_prop_cascade_ex(IROptCtx *ctx) __attribute__((unused));
static int tcc_ir_opt_branch_cleanup_cascade_ex(IROptCtx *ctx) __attribute__((unused));
static int tcc_ir_opt_memory_trigger_ex(IROptCtx *ctx) __attribute__((unused));
static int tcc_ir_opt_entry_store_cleanup_ex(IROptCtx *ctx) __attribute__((unused));
#endif

/* A cascade slot runs passes whose results deliberately do NOT feed the outer
 * fixpoint count -- dce and compact_nops clean up after the passes that do, and
 * counting them would keep the outer group spinning.  But they still mutate the
 * IR, and the group driver's per-pass dirty tracking keys off ctx->generation,
 * which only advances on a *reported* change.  So a cascade records those
 * silent mutations and bumps the generation itself; without this, a slot that
 * returns 0 after NOPing an instruction would let the driver skip passes that
 * had legitimately new work.  Keep `silent` out of the returned count: it is a
 * cache-invalidation signal, not a fixpoint signal. */
#define CASCADE_END(ctx, total, silent)                                        \
  do {                                                                         \
    if ((silent) && (total) == 0)                                              \
      tcc_ir_opt_ctx_invalidate(ctx);                                          \
    return (total);                                                            \
  } while (0)

/* Cascade must converge locally: the outer memory-group trigger may drive only one iteration. */
static int tcc_ir_opt_known_bits_cascade_ex(IROptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int total = 0, silent = 0;
  for (int i = 0; i < 8; i++) {
    int ch = 0;
    ch += tcc_ir_opt_known_bits(ir);
    ch += tcc_ir_opt_const_prop_tmp(ir);
    /* ssa:branch runs after this whole flat phase — too late to unblock the cascade's DCE/forwarding behind a proven-dead guard */
    {
      IROptCtx bctx;
      tcc_ir_opt_ctx_init(&bctx, ir);
      ch += tcc_ir_opt_gens_branch_ex(&bctx);
      tcc_ir_opt_ctx_free(&bctx);
    }
    silent += tcc_ir_opt_dce(ir);
    ch += tcc_ir_opt_eliminate_fallthrough(ir);
    silent += tcc_ir_opt_compact_nops(ir);
    ch += tcc_ir_opt_sl_forward(ir);
    /* Re-run here: collapsing a guard grows the straight-line region, else forwarding stalls at the first BB boundary. */
    ch += tcc_ir_opt_global_sl_fwd(ir);
    if (!ch)
      break;
    total += ch;
  }
  CASCADE_END(ctx, total, silent);
}

static int tcc_ir_opt_const_prop_cascade_ex(IROptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int total = 0;
  for (int i = 0; i < 4; i++) {
    int ch = 0;
    ch += tcc_ir_opt_const_prop_tmp(ir);
    ch += tcc_ir_opt_const_var_prop(ir);
    ch += tcc_ir_opt_value_tracking(ir);
    if (!ch)
      break;
    total += ch;
  }
  total += tcc_ir_opt_const_prop_tmp(ir);
  return total;
}

/* Needed because elim_fallthrough otherwise only runs in the memory group, skipped when sl_forward finds nothing. */
static int tcc_ir_opt_branch_cleanup_cascade_ex(IROptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int total = 0, silent = 0;
  for (int i = 0; i < 8; i++) {
    int ch = 0;
    ch += tcc_ir_opt_eliminate_fallthrough(ir);
    if (ch)
      silent += tcc_ir_opt_compact_nops(ir);
    ch += tcc_ir_opt_dce(ir);
    if (!ch)
      break;
    total += ch;
  }
  CASCADE_END(ctx, total, silent);
}

/* PASS/PASS_GATED take a TCCIRState* pass, the _CTX forms an IROptCtx* one. */
#define PASS(nm, f) { .name = nm, .fn.run_ir = f, .takes_ir = 1 }
/* An O0-only compiler never runs a gated pass (pipeline_run.c treats every
 * flag as off there), so it does not reference one: the entry keeps its slot
 * and its gate, and the pass folds out of the build. */
#ifdef CONFIG_TCC_O0_ONLY
#define PASS_GATED(nm, f, flag) { .name = nm, .flag_offset = flag, .takes_ir = 1 }
#define PASS_GATED_CTX(nm, f, flag) { .name = nm, .flag_offset = flag }
#else
#define PASS_GATED(nm, f, flag) { .name = nm, .fn.run_ir = f, .flag_offset = flag, .takes_ir = 1 }
#define PASS_GATED_CTX(nm, f, flag) { .name = nm, .fn.run = f, .flag_offset = flag }
#endif

static const IROptPass propagation_passes[] = {
  /* Runs first so later passes don't work on a body that is about to collapse. */
  PASS_GATED("uninit_ub", tcc_ir_opt_uninit_local_ub, FLAG(opt_dce)),
  PASS_GATED("uninit_dom_ret", tcc_ir_opt_uninit_dominates_return, FLAG(opt_dce)),
  /* Before dce, which then takes the compares and counters the folded branches read. */
  PASS_GATED("unreachable_fold", tcc_ir_opt_unreachable_fold, FLAG(opt_jump_threading)),
  PASS_GATED("dce", tcc_ir_opt_dce, FLAG(opt_dce)),
  /* A constant local table only read through its address is read from .rodata instead. */
  PASS_GATED_CTX("const_local_table", tcc_ir_opt_const_local_table_ex, FLAG(opt_const_prop)),
  /* A local filled by a whole copy of a by-value struct parameter becomes the parameter's own memory. */
  PASS_GATED("param_copy_alias", tcc_ir_opt_param_copy_alias, FLAG(opt_store_load_fwd)),
  /* Resolves derefs through single-def &local pointers before the const/forwarding cluster below sees them. */
  PASS_GATED("ptr_local_fwd", tcc_ir_opt_ptr_local_fwd, FLAG(opt_store_load_fwd)),
  /* Must follow dce: its prologue clears stale `addrtaken` flags on VARs whose LEA was just DCE-ed. */
  PASS_GATED("const_var_prop", tcc_ir_opt_const_var_prop, FLAG(opt_const_prop)),
  PASS_GATED("global_init", tcc_ir_opt_global_init_prop, FLAG(opt_const_prop)),
  PASS_GATED("symref_prop", tcc_ir_opt_symref_const_prop, FLAG(opt_const_prop)),
  PASS_GATED("global_sl_fwd", tcc_ir_opt_global_sl_fwd, FLAG(opt_store_load_fwd)),
  PASS_GATED("const_prop_tmp", tcc_ir_opt_const_prop_tmp, FLAG(opt_const_prop)),
  /* Runs once: converges arbitrary RMW chain depth in a single forward pass. */
  PASS_GATED("const_agg_fold", tcc_ir_opt_const_aggregate_fold, FLAG(opt_const_prop)),
  PASS_GATED_CTX("known_bits", tcc_ir_opt_known_bits_ex, FLAG(opt_const_prop)),
  PASS_GATED("add_reassoc", tcc_ir_opt_add_reassoc, FLAG(opt_const_prop)),
  PASS_GATED("redundant_assign", tcc_ir_opt_redundant_var_assign, FLAG(opt_const_prop)),
  PASS_GATED("string_calls", tcc_ir_opt_const_string_calls, FLAG(opt_const_prop)),
  PASS_GATED("ssa_string_fold", tcc_ir_ssa_opt_const_string_fold_flat, FLAG(opt_const_prop)),
  /* After the string folds (a folded call can shrink below the inline
   * threshold) and before sl_forward/fusion so they see the expanded
   * loads/stores.  Inserts instructions -> ALL. */
  PASS("mem_inline", tcc_ir_opt_mem_inline),
  PASS_GATED_CTX("self_copy_elim", tcc_ir_opt_self_copy_elim_ex, FLAG(opt_const_prop)),
  PASS_GATED("value_tracking", tcc_ir_opt_value_tracking, FLAG(opt_const_prop)),
  PASS_GATED("cmp_expr_fold", tcc_ir_opt_cmp_expr_fold, FLAG(opt_const_prop)),
  PASS_GATED_CTX("self_arith", tcc_ir_opt_self_arith_fold_ex, FLAG(opt_const_prop)),
  PASS_GATED("switch_collapse", tcc_ir_opt_switch_collapse, FLAG(opt_const_prop)),
  PASS_GATED("setif_fuse", tcc_ir_opt_setif_branch_fuse, FLAG(opt_const_prop)),
  PASS_GATED("stack_bool", tcc_ir_opt_stack_bool_diamond, FLAG(opt_const_prop)),
  PASS_GATED_CTX("var_tmp_fwd", tcc_ir_opt_var_tmp_fwd_ex, FLAG(opt_const_prop)),
  /* Restored after b9b1be1a dropped it; same gate as the pre-refactor table. */
  PASS_GATED("var_to_tmp", tcc_ir_opt_var_to_tmp, FLAG(opt_copy_prop)),
  PASS_GATED("inline_param_copy", tcc_ir_opt_inline_param_copy_elim, FLAG(opt_copy_prop)),
};

/* elim_fallthrough first: propagation leaves fall-through jumps whose stale block boundaries stall sl_forward. */
static int tcc_ir_opt_memory_trigger_ex(IROptCtx *ctx)
{
  int silent = 0;
  if (TCC_OPT(tcc_state, opt_jump_threading) &&
      tcc_ir_opt_eliminate_fallthrough(ctx->ir))
    silent += 1 + tcc_ir_opt_compact_nops(ctx->ir);
  int total = tcc_ir_opt_sl_forward(ctx->ir);
  CASCADE_END(ctx, total, silent);
}

static const IROptPass memory_passes[] = {
  PASS_GATED_CTX("sl_forward", tcc_ir_opt_memory_trigger_ex, FLAG(opt_store_load_fwd)),
  /* Pattern only exists post-forwarding, and must precede the fusion group merging the SHL into the OR operand. */
  PASS_GATED("bf_insert_extract", tcc_ir_opt_bitfield_insert_extract, FLAG(opt_const_prop)),
  /* Needs the post-forwarding symmetric register form, and must precede the fusion group folding a trailing shift into the CMP. */
  PASS_GATED("cmp_field_fuse", tcc_ir_opt_cmp_field_fuse, FLAG(opt_const_prop)),
  PASS_GATED_CTX("const_cascade", tcc_ir_opt_const_prop_cascade_ex, FLAG(opt_const_prop)),
  PASS_GATED("setif_fuse", tcc_ir_opt_setif_branch_fuse, FLAG(opt_const_prop)),
  PASS_GATED("stack_bool", tcc_ir_opt_stack_bool_diamond, FLAG(opt_const_prop)),
  PASS_GATED_CTX("var_tmp_fwd", tcc_ir_opt_var_tmp_fwd_ex, FLAG(opt_const_prop)),
  PASS_GATED("dce", tcc_ir_opt_dce, FLAG(opt_dce)),
  PASS_GATED("elim_fallthru", tcc_ir_opt_eliminate_fallthrough, FLAG(opt_jump_threading)),
  /* Internal fixpoint: the outer memory loop's sl_forward trigger can stall mid-cascade and skip later iterations. */
  PASS_GATED_CTX("kb_cascade", tcc_ir_opt_known_bits_cascade_ex, FLAG(opt_const_prop)),
};

static const IROptPass late_cleanup_passes[] = {
  /* Post-forwarding; this group has no trigger so it always runs (the memory
   * group's sl_forward trigger goes idle exactly when forwarding is done,
   * which is when this pass's shapes exist): a chunked by-value copy that
   * survived forwarding, into a slot whose address escapes, becomes one
   * helper call instead of 2W scalar quads. */
  PASS("escape_copy_call", tcc_ir_opt_escape_copy_call),
  /* `(x^y) cmp y -> x cmp 0`: the plain-register XOR/CMP shape it matches only
   * exists once the fusion group has pulled the derefs out into LOAD_INDEXED,
   * which happens after both the propagation and memory groups have finished. */
  PASS_GATED("cmp_xor_cancel", tcc_ir_opt_cmp_xor_cancel, FLAG(opt_const_prop)),
  /* Runs first so the dead-store passes below see the simplified CFG. */
  PASS_GATED_CTX("branch_cleanup", tcc_ir_opt_branch_cleanup_cascade_ex, FLAG(opt_jump_threading)),
  /* Must precede zero_vla so the orphaned outer SP_SAVE/RESTORE pair collapses in the same round. */
  PASS_GATED("dead_vla_struct", tcc_ir_opt_dead_vla_struct_elim, FLAG(opt_dead_store)),
  PASS_GATED("alloca_load_fwd", tcc_ir_opt_alloca_load_fwd, FLAG(opt_dead_store)),
  /* VLA_ALLOC(size=0) -> NOP, so dead_lea_store (which bails on any VLA_ALLOC) can clean the stack scaffolding. */
  PASS_GATED("zero_vla", tcc_ir_opt_zero_vla_elim, FLAG(opt_dead_store)),
  PASS_GATED("byte_store_merge", tcc_ir_opt_byte_store_merge, FLAG(opt_redundant_store)),
  /* Byte/halfword field stores over a zero-filled local word fold into its word store. */
  PASS_GATED("slot_const_store_fold", tcc_ir_opt_slot_const_store_fold, FLAG(opt_redundant_store)),
  PASS_GATED("dse", tcc_ir_opt_dse, FLAG(opt_dead_store)),
  /* Only fires with ir_late_reopt_phase + sym->a.tu_no_readers; must precede the DCE cascade that frees its RHS. */
  PASS_GATED("dead_static_store", tcc_ir_opt_dead_static_store_elim, FLAG(opt_dead_store)),
  PASS_GATED("dead_var_store", tcc_ir_opt_dead_var_store_elim, FLAG(opt_dead_store)),
  PASS_GATED("dead_addrvar", tcc_ir_opt_dead_addrvar_elim, FLAG(opt_dead_store)),
  /* Trailing writes to addr-taken VARs read earlier but never after the dead write; dead_addrvar misses those. */
  PASS_GATED("dead_trail_addrvar", tcc_ir_opt_dead_trailing_addrvar_store_elim, FLAG(opt_dead_store)),
  /* VREG-target VLA_SP_SAVE — the shape produced by alloca_load_fwd. */
  PASS_GATED("dead_alloca_vreg", tcc_ir_opt_dead_alloca_vreg_elim, FLAG(opt_dead_store)),
  PASS_GATED("dead_local_slot", tcc_ir_opt_dead_local_slot_elim, FLAG(opt_dead_store)),
  PASS_GATED("dead_lea_store", tcc_ir_opt_dead_lea_store_elim, FLAG(opt_dead_store)),
  PASS_GATED("dead_temp_local", tcc_ir_opt_dead_temp_local_elim, FLAG(opt_dead_store)),
  PASS_GATED("redundant_assign", tcc_ir_opt_redundant_var_assign, FLAG(opt_dead_store)),
  /* The fusable CMP;SETIF;TEST_ZERO;JUMPIF chain often only FORMS late:
   * the memory group's final const cascade folds the XOR mask of inlined
   * bool checks, and redundant_assign (just above) removes the XOR#0
   * residue's `T9 <- T7` alias.  One more fuse run catches both
   * (20141107-1 sites). */
  PASS_GATED("setif_fuse", tcc_ir_opt_setif_branch_fuse, FLAG(opt_const_prop)),
  PASS_GATED("inplace_arith", tcc_ir_opt_store_inplace_arith, FLAG(opt_redundant_store)),
  PASS_GATED("global_base_share", tcc_ir_opt_global_base_share, FLAG(opt_indexed_memory)),
  /* Second run: the dead-store passes above expose new dead CMP/jump diamonds. */
  PASS_GATED_CTX("branch_cleanup", tcc_ir_opt_branch_cleanup_cascade_ex, FLAG(opt_jump_threading)),
  PASS_GATED("inf_loop_simpl", tcc_ir_opt_infinite_loop_simplify, FLAG(opt_dce)),
  /* Must follow inf_loop_simpl: NOPs the unobservable work preceding a collapsed never-returning self-jump. */
  PASS_GATED("dead_pre_inf", tcc_ir_opt_dead_before_infinite_loop, FLAG(opt_dce)),
};

static int tcc_ir_opt_entry_store_cleanup_ex(IROptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int ch = 0, silent = 0;
  ch += tcc_ir_opt_const_prop_tmp(ir);
  ch += tcc_ir_opt_const_var_prop(ir);
  ch += tcc_ir_opt_redundant_loop_check(ir);
  silent += tcc_ir_opt_dce(ir);
  silent += tcc_ir_opt_compact_nops(ir);
  ch += tcc_ir_opt_sl_forward(ir);
  silent += tcc_ir_opt_dce(ir);
  ch += tcc_ir_opt_dead_var_store_elim(ir);
  ch += tcc_ir_opt_const_var_prop(ir);
  silent += tcc_ir_opt_dce(ir);
  silent += tcc_ir_opt_compact_nops(ir);
  CASCADE_END(ctx, ch, silent);
}

static const IROptPass entry_store_passes[] = {
  PASS_GATED("entry_store", tcc_ir_opt_entry_store_prop, FLAG(opt_store_load_fwd)),
  PASS_GATED_CTX("esp_cleanup", tcc_ir_opt_entry_store_cleanup_ex, FLAG(opt_const_prop)),
  /* entry_store turns slot reads into constants, and only value_tracking evaluates a
   * soft-float helper call whose arguments have just become constant.  In the
   * propagation group it runs before this forwarding, so it never sees them; the
   * memory group is no good either, since its sl_forward trigger goes idle exactly
   * when entry_store did the forwarding instead. */
  PASS_GATED("esp_value_tracking", tcc_ir_opt_value_tracking, FLAG(opt_const_prop)),
};

#undef PASS
#undef PASS_GATED
#undef PASS_GATED_CTX
#undef FLAG

#define COUNTOF(arr) (int)(sizeof(arr) / sizeof((arr)[0]))

const IRPassGroup entry_store_group = {
  "entry_store_prop", entry_store_passes, COUNTOF(entry_store_passes), 3, 1, 0
};

const IRPassGroup propagation_group = {
  "propagation", propagation_passes, COUNTOF(propagation_passes), 10, 0, -1
};

const IRPassGroup memory_group = {
  "memory", memory_passes, COUNTOF(memory_passes), 12, 1, 0
};

const IRPassGroup late_cleanup_group = {
  "late_cleanup", late_cleanup_passes, COUNTOF(late_cleanup_passes), 2, 1, -1
};

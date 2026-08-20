/*
 *  TCC IR - Optimization Passes
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct TCCIRState;
struct TCCState;
struct IRLoops;
struct IROptCtx;
struct Sym;

/* Each pass has a plain (TCCIRState *) form wrapping the pipeline _ex (IROptCtx *) form. */
int tcc_ir_opt_dce(struct TCCIRState *ir);
int tcc_ir_opt_dce_ex(struct IROptCtx *ctx);

/* 1 if the callee never returns (noreturn attr or abort/exit/_Exit/quick_exit). */
int tcc_ir_callee_is_noreturn(struct Sym *callee);

/* Removes NOPs, shrinks the array, fixes jump targets; returns NOPs removed. */
int tcc_ir_opt_compact_nops(struct TCCIRState *ir);
int tcc_ir_opt_compact_nops_ex(struct IROptCtx *ctx);

/* NOPs the whole body when nothing is observable (no STORE/CALL/RETURNVALUE/volatile read). */
int tcc_ir_opt_useless_function_body(struct TCCIRState *ir);
int tcc_ir_opt_useless_function_body_ex(struct IROptCtx *ctx);

/* No RETURN anywhere and no calls/asm/volatile/setjmp/trap -> collapse body to `b .`. */
int tcc_ir_opt_noreturn_collapse(struct TCCIRState *ir);
int tcc_ir_opt_noreturn_collapse_ex(struct IROptCtx *ctx);

/* Collapses infinite loops with no externally-observable side effects to self-jumps. */
int tcc_ir_opt_infinite_loop_simplify(struct TCCIRState *ir);
int tcc_ir_opt_infinite_loop_simplify_ex(struct IROptCtx *ctx);

/* Stores preceding a side-effect-free infinite loop are unobservable: reroute entry edges to the loop sink. */
int tcc_ir_opt_dead_before_infinite_loop(struct TCCIRState *ir);
int tcc_ir_opt_dead_before_infinite_loop_ex(struct IROptCtx *ctx);

/* Lone-TRAP body: resets dirty_registers/leaffunc/noreturn/need_frame_pointer; caller must reset `loc`. */
int tcc_ir_opt_trap_only_body_suppress(struct TCCIRState *ir);
int tcc_ir_opt_trap_only_body_suppress_ex(struct IROptCtx *ctx);

/* NOPs VLA_ALLOCs with compile-time 0 size plus their VLA_SP_SAVE/RESTORE pair when SP is untouched between. */
int tcc_ir_opt_zero_vla_elim(struct TCCIRState *ir);
int tcc_ir_opt_zero_vla_elim_ex(struct IROptCtx *ctx);

/* NOPs a VLA whose base pointer only feeds STORE destinations (never loaded, never escaping). */
int tcc_ir_opt_dead_vla_struct_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_vla_struct_elim_ex(struct IROptCtx *ctx);

/* Retargets VLA_SP_SAVE from a slot to the immediately reloading LOAD's vreg (one `mov vreg, sp`). */
int tcc_ir_opt_alloca_load_fwd(struct TCCIRState *ir);
int tcc_ir_opt_alloca_load_fwd_ex(struct IROptCtx *ctx);

/* Companion to dead_vla_struct_elim for the VREG-target VLA_SP_SAVE shape alloca_load_fwd produces. */
int tcc_ir_opt_dead_alloca_vreg_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_alloca_vreg_elim_ex(struct IROptCtx *ctx);

/* Unconditional self-call before any return path never returns: collapse body to `b .`. */
int tcc_ir_opt_infinite_self_recursion(struct TCCIRState *ir, struct Sym *func_sym);

/* Sets ir->noreturn = 1 when every RETURN is gone after DCE past a noreturn call, so codegen omits the epilogue. */
int tcc_ir_opt_noreturn_call_epilogue_suppress(struct TCCIRState *ir);

int tcc_ir_opt_dse(struct TCCIRState *ir);
int tcc_ir_opt_dse_ex(struct IROptCtx *ctx);

int tcc_ir_opt_dead_addrvar_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_var_store_elim(struct TCCIRState *ir);

/* Addr-taken VAR writes following the LAST read, which addrvar misses because the VAR looks live overall. */
int tcc_ir_opt_dead_trailing_addrvar_store_elim(struct TCCIRState *ir);

/* Kills VAR assigns overwritten before the next read. */
int tcc_ir_opt_redundant_var_assign(struct TCCIRState *ir);

int tcc_ir_opt_const_prop_tmp(struct TCCIRState *ir);
/* Ungated core of the above (no pass-disable check / timing); shared with ssa_opt_const_prop_tmp. */
int tcc_ir_opt_const_prop_tmp_core(struct TCCIRState *ir);

/* Per-TMP known_zero/known_one masks, folding bitfield insert/extract chains const_prop misses. */
int tcc_ir_opt_known_bits(struct TCCIRState *ir);

/* DSE for STOREs through a LEA-temp deref; complements dead_local_slot_elim (direct StackLoc dests only). */
int tcc_ir_opt_dead_lea_store_elim(struct TCCIRState *ir);

/* Folds read-modify-write chains on non-escaping local aggregates by propagating the slot constant across calls. */
int tcc_ir_opt_const_aggregate_fold(struct TCCIRState *ir);

/* Constant-folds string builtin calls such as `strcmp` and `strncmp`. */
int tcc_ir_opt_const_string_calls(struct TCCIRState *ir);

/* Drops memmove/memcpy(dst, &stack_tmp, N) whose source was only written by preceding local STOREs, rewriting each to STORE_INDEXED. */
int tcc_ir_opt_memmove_to_indexed_stores(struct TCCIRState *ir);

/* Tracks constants through ADD/SUB. */
int tcc_ir_opt_value_tracking(struct TCCIRState *ir);

/* Fuses CMP+SETIF+TEST_ZERO+JUMPIF into CMP+JUMPIF. */
int tcc_ir_opt_setif_branch_fuse(struct TCCIRState *ir);

/* Collapses STORE/JUMP/STORE/TEST_ZERO/JUMPIF through a single-use stack slot into two direct branches. */
int tcc_ir_opt_stack_bool_diamond(struct TCCIRState *ir);

/* Single-BB VAR -> TMP promotion for a non-address-taken VAR with one def and in-BB lval-ASSIGN reads only. */
int tcc_ir_opt_var_to_tmp(struct TCCIRState *ir);

/* Drops the redundant per-parameter local an inline expansion creates (Vpar = Vsrc copy). */
int tcc_ir_opt_inline_param_copy_elim(struct TCCIRState *ir);
int tcc_ir_opt_inline_param_copy_elim_ex(struct IROptCtx *ctx);

int tcc_ir_opt_add_reassoc(struct TCCIRState *ir);

/* Folds `CMP V, Addr[StackLoc[Y]]` (and its JUMPIF/SELECT) when V is provably Addr[StackLoc[X]] + N, X+N == Y. */
int tcc_ir_opt_cmp_stack_addr_fold(struct TCCIRState *ir);

/* Rewrites derefs through known stack-address temps to direct StackLoc accesses; folds stack-address differences. */
int tcc_ir_opt_stack_addr_simplify(struct TCCIRState *ir);

int tcc_ir_opt_cmp_expr_fold(struct TCCIRState *ir);
int tcc_ir_opt_cmp_xor_cancel(struct TCCIRState *ir);

/* ZEXT + SHL #32 + ZEXT + OR -> PACK64. */
int tcc_ir_opt_pack64(struct TCCIRState *ir);

/* `(X_hi SHL #32) OR X_lo` -> PACK64 when both halves rely on implicit zero-extension into the i64 OR. */
int tcc_ir_opt_pack64_implicit(struct TCCIRState *ir);

/* PACK64(low(X), X>>32) -> ASSIGN X. */
int tcc_ir_opt_pack64_tautology(struct TCCIRState *ir);

/* 64-bit LOAD from StackLoc[A] preceded by 32-bit STOREs to StackLoc[A] and StackLoc[A+4] -> PACK64(lo, hi). */
int tcc_ir_opt_pack64_from_stack_stores(struct TCCIRState *ir);

/* `((X SHL 32) OR Y) SHL 32` -> `Y SHL 32`, and the AND #0xFFFFFFFF analogue. */
int tcc_ir_opt_shl32_or_chain(struct TCCIRState *ir);
/* Signed `x / 2^n` -> bias + arithmetic shift, so no SDIV is emitted. */
int tcc_ir_opt_sdiv_pow2(struct TCCIRState *ir);

/* Folds `T_new = X OP Y; T_final = T_new` into one op. */
int tcc_ir_opt_assign_fuse(struct TCCIRState *ir);

/* `CMP T_u64, u64_imm_with_hi_0` -> 32-bit when T's hi half is provably zero (SHR>=32 or ZEXT). */
int tcc_ir_opt_cmp_narrow_64(struct TCCIRState *ir);

/* `CMP imm, reg` -> `CMP reg, imm` with the condition mirrored on every flag
 * reader, so the constant reaches `cmp`'s immediate slot instead of a `mov`. */
int tcc_ir_opt_cmp_imm_swap(struct TCCIRState *ir);
/* Delete a CMP whose flags a dominating identical CMP already left in place. */
int tcc_ir_opt_redundant_cmp(struct TCCIRState *ir);

/* `T_u64 AND imm_with_hi_0` -> 32-bit when the result is only ever read as 32 bits.
 * Also makes the AND a low-only consumer, which lets shift64_dead_half mark the
 * SHR feeding it (the `(bits >> c) & mask` extract idiom). */
int tcc_ir_opt_and64_narrow(struct TCCIRState *ir);

/* Flags SHL/SHR/SAR results whose low or high word is provably unread, so codegen skips the dead half-write. */
int tcc_ir_opt_shift64_dead_half(struct TCCIRState *ir);

/* Annotate the halves of 64-bit values that are provably zero, and the halves
 * of a result that no consumer reads as a consequence.  Runs last, after every
 * operand rewrite -- the dead-half rule is only valid against the final
 * instruction stream. */
int tcc_ir_opt_zero_half64(struct TCCIRState *ir);

/* `(v64 >> k) & mask` with k >= 32 is one UBFX on v64's high word.  Must run
 * after the mask narrowing passes (and64_narrow / narrow.c) have made the
 * consumer 32-bit, and before shift64_dead_half, whose producer disappears. */
int tcc_ir_opt_shift64_extract_ubfx(struct TCCIRState *ir);

/* Collapses identical macro-unrolled block runs back into a loop. */
int tcc_ir_opt_reroll(struct TCCIRState *ir);

/* Collapses repeated `T = -T` chains by tracking each TEMP's canonical (base, sign) pair. */
int tcc_ir_opt_neg_chain_cse(struct TCCIRState *ir);
int tcc_ir_opt_neg_chain_cse_ex(struct IROptCtx *ctx);

/* `((V<<n) | low) >> n` -> V when low < 2^n and V < 2^(32-n). */
int tcc_ir_opt_bitfield_insert_extract(struct TCCIRState *ir);
int tcc_ir_opt_bitfield_insert_extract_ex(struct IROptCtx *ctx);

/* (W & ~field) | (V << lsb) -> BFI; must run before barrel_shift_fusion; records lsb/width in ir->bfi_params[orig_index]. */
int tcc_ir_opt_bitfield_insert_to_bfi(struct TCCIRState *ir);

/* Run of `a.fi != b.fi` bitfield compares to one label -> one masked word XOR compare; must precede shift-into-CMP fusion. */
int tcc_ir_opt_cmp_field_fuse(struct TCCIRState *ir);
int tcc_ir_opt_cmp_field_fuse_ex(struct IROptCtx *ctx);

int tcc_ir_opt_sl_forward(struct TCCIRState *ir);

/* Forwards a const ASSIGN to a VAR through its &V LEA into deref uses (the addr-taken shape var_to_tmp skips). */
int tcc_ir_opt_addrof_var_fwd(struct TCCIRState *ir);

/* Rewrites derefs through single-def entry-block `P = &V` pointer VARs (and their TEMP copies) to direct V accesses. */
int tcc_ir_opt_ptr_local_fwd(struct TCCIRState *ir);
int tcc_ir_opt_ptr_local_fwd_ex(struct IROptCtx *ctx);

/* Forwards a load of a copied local field (`x=G; ... x.field`) to the source global `G.field`. */
int tcc_ir_opt_copy_source_load_fwd(struct TCCIRState *ir);

/* Forwards STORE GlobalSym(X) into later in-BB deref reads; invalidated by calls, aliasing stores, BB boundaries. */
int tcc_ir_opt_global_sl_fwd(struct TCCIRState *ir);

/* Cross-BB CSE of global ASSIGN/LOAD; requires forward-only control flow and no aliasing stores. */
int tcc_ir_opt_invariant_global_load_hoist(struct TCCIRState *ir);
int tcc_ir_opt_invariant_global_load_hoist_ex(struct IROptCtx *ctx);

/* CSEs a global lvalue operand read >1 time in one straight-line clobber-free region into a single ASSIGN. */
int tcc_ir_opt_global_deref_cse(struct TCCIRState *ir);
int tcc_ir_opt_deref_operand_cse(struct TCCIRState *ir);

/* Narrows a whole-word bitfield RMW to the byte/halfword the field exactly fills. */
int tcc_ir_opt_bitfield_unit_narrow(struct TCCIRState *ir);

/* Inserts one `T_v = T***DEREF***` after a singly-defined pointer TEMP and rewrites later derefs to T_v non-lval. */
int tcc_ir_opt_invariant_temp_deref_hoist(struct TCCIRState *ir);
int tcc_ir_opt_invariant_temp_deref_hoist_ex(struct IROptCtx *ctx);

/* Collapses the spill/addr/store/reload sequence of `f(int v){ helper(&v); return v; }` after inlining. */
int tcc_ir_opt_param_addrof_const_fold(struct TCCIRState *ir);

/* Local-variable analogue: `V = c0; helper(&V); use(V)` where helper inlines to a constant STORE through &V. */
int tcc_ir_opt_local_addrof_const_fold(struct TCCIRState *ir);

/* Propagates constant VARs exposed by store-load forwarding. */
int tcc_ir_opt_const_var_prop(struct TCCIRState *ir);

/* LOAD of a static global with a known, never-written initializer -> ASSIGN of the constant. */
int tcc_ir_opt_global_init_prop(struct TCCIRState *ir);

/* Propagates `ASSIGN T = symref(S, +A)` into later in-block uses so global_init_prop can fold against section data. */
int tcc_ir_opt_symref_const_prop(struct TCCIRState *ir);

/* Packs a constant-initialized _Complex float local used as a single FUNCPARAMVAL into a 64-bit immediate. */
int tcc_ir_opt_complex_const_param_fold(struct TCCIRState *ir);

/* Sets sym->f.func_pure_via_sret when the only observable effect is writes through the sret-pointer parameter. */
void tcc_ir_analyze_pure_via_sret(struct TCCIRState *ir, struct Sym *func_sym);

/* Per-pointer-parameter must-write byte map; run at end-of-IR-opts into a TU-scoped Sym*-keyed table for dead_init_via_call. */
void tcc_ir_compute_func_write_summary(struct TCCIRState *ir, struct Sym *func_sym);
void tcc_ir_func_write_summary_clear_all(void);

/* Per-function record of static globals read/address-taken, static globals written, and statically called Syms. */
void tcc_ir_collect_tu_func_summary(struct TCCIRState *ir, struct Sym *func_sym);
/* Pre-optimization static-read scan. */
void tcc_ir_collect_tu_static_reads_preopt(struct TCCIRState *ir);
void tcc_ir_tu_func_summary_clear_all(void);
/* End-of-parse: call-graph reachability + live static-read set; marks dead statics and their writer functions. */
void tcc_ir_tu_analyze_dead_statics(void);

/* End-of-parse: sets func_late_reopt on token-preserving callers whose callee turned out noreturn. */
void tcc_ir_tu_propagate_noreturn_to_callers(void);

/* NOPs stores to statics the end-of-TU analysis confirmed have no reachable readers. */
int tcc_ir_opt_dead_static_store_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_static_store_elim_ex(struct IROptCtx *ctx);

/* Merges STORE clusters to same-section globals into one LEA + STORE_INDEXED, dropping per-store literal loads. */
int tcc_ir_opt_global_base_share(struct TCCIRState *ir);
int tcc_ir_opt_global_base_share_ex(struct IROptCtx *ctx);

/* Kills stack-slot stores whose bytes a later CALL's callee write summary fully overwrites. */
int tcc_ir_opt_dead_init_via_call(struct TCCIRState *ir);
int tcc_ir_opt_dead_local_slot_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_local_slot_elim_ex(struct IROptCtx *ctx);
int tcc_ir_opt_dead_temp_local_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_temp_local_elim_ex(struct IROptCtx *ctx);
int tcc_ir_opt_call_chain_rename(struct TCCIRState *ir);
int tcc_ir_opt_stackoff_addr_cse(struct TCCIRState *ir);
/* Late variant (run from regalloc, after alias-sensitive passes): park the
 * STACKOFF base of an already-fused LOAD/STORE_INDEXED in a register instead of
 * re-materializing `add rX, sp, #off` at every access. */
int tcc_ir_opt_stackoff_indexed_base_cse(struct TCCIRState *ir);
int tcc_ir_opt_lea_fold(struct TCCIRState *ir);
int tcc_ir_opt_lea_rmw_fold(struct TCCIRState *ir);
int tcc_ir_opt_add_deref_fold(struct TCCIRState *ir);
void tcc_ir_barrel_shift_fusion(struct TCCIRState *ir);
int tcc_ir_opt_shift_pair_to_ubfx(struct TCCIRState *ir);
int tcc_ir_opt_stack_addr_cse(struct TCCIRState *ir);
int tcc_ir_opt_entry_store_prop(struct TCCIRState *ir);
int tcc_ir_opt_float_branch_fold(struct TCCIRState *ir);
int tcc_ir_opt_redundant_loop_check(struct TCCIRState *ir);
int tcc_ir_opt_float_narrowing(struct TCCIRState *ir);
int tcc_ir_opt_jump_threading(struct TCCIRState *ir);
int tcc_ir_opt_bool_diamond_branch(struct TCCIRState *ir);
int tcc_ir_opt_orphan_cmp_elim(struct TCCIRState *ir);

/* `JUMPIF C -> A; JUMP -> B` -> `JUMPIF !C -> B`; allow_backward=0 pre-RA keeps rotation's body->latch JUMP intact. */
int tcc_ir_opt_jumpif_invert(struct TCCIRState *ir, int allow_backward);

/* Replaces the memset(0)+stores pattern with a BLOCK_COPY from rodata. */
int tcc_ir_opt_block_copy_init(struct TCCIRState *ir);

/* memset(stack, N<=8, 0) -> one or two direct STORE #0, for when block_copy_init did not fire. */
int tcc_ir_opt_small_memset_to_store(struct TCCIRState *ir);

/* memset(&global[off], 0, N) -> a single naturally-aligned direct STORE #0 when N is exactly one store's width. */
int tcc_ir_opt_small_global_memset_to_store(struct TCCIRState *ir);

/* Runs the memory-init lowering trio on raw IR, before CFG/SSA construction. */
int ssa_opt_mem_init(struct TCCIRState *ir);

/* Duplicate `RETURNVALUE #imm` -> JUMP to the first, leaving one mov+branch and N-1 branches. */
int tcc_ir_opt_returnvalue_merge(struct TCCIRState *ir);

int tcc_ir_opt_eliminate_fallthrough(struct TCCIRState *ir);

/* JUMPIF exit + ASSIGNs + JUMP body -> ASSIGNs + inverted JUMPIF body, saving one branch per loop. */
int tcc_ir_opt_backedge_phi_hoist(struct TCCIRState *ir);

/* Post-RA: per noreturn callee keep the first guarded call as a shared sink and retarget later guards to it; TCC_NO_ABORT_MERGE disables. */
int tcc_ir_opt_abort_tail_merge(struct TCCIRState *ir);

int tcc_ir_opt_const_prop_tmp_ex(struct IROptCtx *ctx);
int tcc_ir_opt_single_value_tmp_ex(struct IROptCtx *ctx);
int tcc_ir_opt_known_bits_ex(struct IROptCtx *ctx);
int tcc_ir_opt_dead_lea_store_elim_ex(struct IROptCtx *ctx);
int tcc_ir_opt_const_aggregate_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_const_var_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_global_init_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_symref_const_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_value_tracking_ex(struct IROptCtx *ctx);
int tcc_ir_opt_add_reassoc_ex(struct IROptCtx *ctx);
int tcc_ir_opt_cmp_stack_addr_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_cmp_expr_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_cmp_xor_cancel_ex(struct IROptCtx *ctx);
int tcc_ir_opt_const_string_calls_ex(struct IROptCtx *ctx);
int ssa_const_string_fold_flat_ex(struct IROptCtx *ctx);
int tcc_ir_opt_mem_inline(struct TCCIRState *ir);
int tcc_ir_opt_mem_inline_ex(struct IROptCtx *ctx);
int tcc_ir_opt_setif_branch_fuse_ex(struct IROptCtx *ctx);
int tcc_ir_opt_stack_bool_diamond_ex(struct IROptCtx *ctx);
int tcc_ir_opt_float_narrowing_ex(struct IROptCtx *ctx);
int tcc_ir_opt_pack64_ex(struct IROptCtx *ctx);
int tcc_ir_opt_pack64_tautology_ex(struct IROptCtx *ctx);
int tcc_ir_opt_pack64_from_stack_stores_ex(struct IROptCtx *ctx);
int tcc_ir_opt_cmp_narrow_64_ex(struct IROptCtx *ctx);
int tcc_ir_opt_cmp_imm_swap_ex(struct IROptCtx *ctx);
int tcc_ir_opt_and64_narrow_ex(struct IROptCtx *ctx);
int tcc_ir_opt_sl_forward_ex(struct IROptCtx *ctx);
int tcc_ir_opt_entry_store_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_assign_fuse_ex(struct IROptCtx *ctx);
int tcc_ir_opt_var_to_tmp_ex(struct IROptCtx *ctx);
int tcc_ir_opt_switch_to_data_ex(struct IROptCtx *ctx);
int tcc_ir_opt_switch_to_data(struct TCCIRState *ir);
int tcc_ir_opt_switch_collapse_ex(struct IROptCtx *ctx);
int tcc_ir_opt_switch_collapse(struct TCCIRState *ir);
int tcc_ir_opt_redundant_loop_check_ex(struct IROptCtx *ctx);
int tcc_ir_opt_float_branch_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_eliminate_fallthrough_ex(struct IROptCtx *ctx);
int tcc_ir_opt_uninit_local_ub(struct TCCIRState *ir);
int tcc_ir_opt_uninit_local_ub_ex(struct IROptCtx *ctx);
int tcc_ir_opt_uninit_dominates_return(struct TCCIRState *ir);
int tcc_ir_opt_uninit_dominates_return_ex(struct IROptCtx *ctx);
int tcc_ir_opt_ub_only_body_elide(struct TCCIRState *ir);
int tcc_ir_opt_ub_only_body_elide_ex(struct IROptCtx *ctx);
int tcc_ir_opt_local_only_body_elide(struct TCCIRState *ir);
int tcc_ir_opt_local_only_body_elide_ex(struct IROptCtx *ctx);
int tcc_ir_opt_const_return_uninit_elide(struct TCCIRState *ir);
int tcc_ir_opt_const_return_uninit_elide_ex(struct IROptCtx *ctx);
int tcc_ir_opt_null_store_dom_return(struct TCCIRState *ir);
int tcc_ir_opt_null_store_dom_return_ex(struct IROptCtx *ctx);
int tcc_ir_opt_redundant_var_assign_ex(struct IROptCtx *ctx);
int tcc_ir_opt_dead_var_store_elim_ex(struct IROptCtx *ctx);
int tcc_ir_opt_dead_addrvar_elim_ex(struct IROptCtx *ctx);
int tcc_ir_opt_dead_trailing_addrvar_store_elim_ex(struct IROptCtx *ctx);
int tcc_ir_opt_rmw_byte_clear(struct TCCIRState *ir);
int tcc_ir_opt_byte_store_merge(struct TCCIRState *ir);
int tcc_ir_opt_byte_store_merge_ex(struct IROptCtx *ctx);
int tcc_ir_opt_const_memcpy_to_dest(struct TCCIRState *ir);
int tcc_ir_opt_const_memcpy_to_dest_ex(struct IROptCtx *ctx);
int tcc_ir_opt_local_copy_prop(struct TCCIRState *ir);
int tcc_ir_opt_local_copy_prop_ex(struct IROptCtx *ctx);
/* Drops a `memmove(B,A,N); memmove(A,B,N)` pair where B is a dead round-trip temp and A is unmodified between. */
int tcc_ir_opt_struct_copy_roundtrip_elim(struct TCCIRState *ir);
/* `memmove(local, &global, N)` into a private read-only slot: reads the global directly and drops the copy. */
int tcc_ir_opt_memmove_global_load_fwd(struct TCCIRState *ir);
int tcc_ir_opt_addrof_var_fwd_ex(struct IROptCtx *ctx);
int tcc_ir_opt_global_sl_fwd_ex(struct IROptCtx *ctx);

typedef struct TCCOptStats
{
  int dce_removed;
  int dse_removed;
  int const_folded;
  int copies_propagated;
  int cse_eliminated;
  int stores_forwarded;
} TCCOptStats;

void tcc_ir_opt_stats_get(TCCOptStats *stats);

void tcc_ir_opt_stats_reset(void);

void tcc_ir_opt_fp_cache_init(struct TCCIRState *ir);

void tcc_ir_opt_fp_cache_clear(struct TCCIRState *ir);

void tcc_ir_opt_fp_cache_free(struct TCCIRState *ir);

/* Lookup an offset in the FP cache; returns the register or -1. */
int tcc_ir_opt_fp_cache_lookup(struct TCCIRState *ir, int offset, int *phys_reg);

void tcc_ir_opt_fp_cache_record(struct TCCIRState *ir, int offset, int phys_reg);

void tcc_ir_opt_fp_cache_invalidate_reg(struct TCCIRState *ir, int phys_reg);

int tcc_ir_find_defining_instruction(struct TCCIRState *ir, int32_t vreg, int before_idx);

int tcc_ir_vreg_has_single_use(struct TCCIRState *ir, int32_t vreg, int exclude_idx);

/* Inserts a quad before an index, renumbering jump + switch-table targets. */
int tcc_ir_insert_instruction_before(struct TCCIRState *ir, int before_idx, struct IRQuadCompact *new_q);

/* Returns 1 and fills value/btype when the optimized IR reduces to a constant return value. */
int tcc_ir_detect_const_result(struct TCCIRState *ir, int64_t *value, int *btype);

void tcc_ir_cache_const_result(struct TCCState *s, int func_token, int64_t value, int btype);
int tcc_ir_lookup_const_result(struct TCCState *s, int func_token, int64_t *value, int *btype);

/* Returns the number of calls replaced. */
int tcc_ir_opt_const_call_replace(struct TCCIRState *ir);

/* Opaque replay snapshot of a static, side-effect-free one-scalar-param function body. */
struct TCCFuncSwitchSnapshot;
typedef struct TCCFuncSwitchSnapshot TCCFuncSwitchSnapshot;

/* Returns 1 and stores an owned snapshot in *out; returns 0 and leaves *out unchanged on failure. */
int tcc_ir_detect_switch_func(struct TCCIRState *ir, TCCFuncSwitchSnapshot **out);

void tcc_ir_switch_func_snapshot_free(TCCFuncSwitchSnapshot *snap);

/* Takes ownership of `snap` even on failure (freed if the cache is full or already has an entry). */
void tcc_ir_cache_switch_func(struct TCCState *s, int func_token, TCCFuncSwitchSnapshot *snap);

/* Returns NULL if not found. */
const TCCFuncSwitchSnapshot *tcc_ir_lookup_switch_func(struct TCCState *s, int func_token);

/* Returns 1 and stores the result in *out_value (and *out_btype if non-NULL); 0 on unsupported op / unknown vreg / step limit. */
int tcc_ir_simulate_switch_func(const TCCFuncSwitchSnapshot *snap, int64_t arg_value,
                                int64_t *out_value, int *out_btype);

/* Also collects snapshot-op indices the caller must replay for side effects; replay_indices=NULL rejects any function needing replay. */
int tcc_ir_simulate_switch_func_ex(const TCCFuncSwitchSnapshot *snap, int64_t arg_value,
                                   int64_t *out_value, int *out_btype,
                                   int *replay_indices, int *replay_count);

void tcc_ir_free_switch_func_cache(struct TCCState *s);

/* Replaces constant-argument FUNCCALLVALs to a cached switch-value function; counts call sites rewritten. */
int tcc_ir_opt_switch_call_replace(struct TCCIRState *ir);

/* Per-pass timing instrumentation (opt-in via TCC_PASS_TIMING env var). */
extern signed char tcc_pass_timing_on;
void tcc_pass_timing_init(void);
unsigned long tcc_pass_clk_us(void);
void tcc_pass_timing_dump(void);

/* Scoped timer.  Nested frames of the same pass name are suppressed, so a pass
 * that times itself and is also timed by the pipeline driver is counted once.
 * Reported time is exclusive of nested timed passes, which is what makes the
 * cascade slots (kb_cascade, const_cascade, ...) readable. */
typedef struct TCCPassTimer {
  unsigned long start_us;
  unsigned long saved_child_us;
  const char *name;
  signed char active;
} TCCPassTimer;

void tcc_pass_timing_begin(TCCPassTimer *t, const char *name);
/* changes < 0 means "this pass does not report a change count". */
void tcc_pass_timing_end(TCCPassTimer *t, int changes);

/* Time `call` under `name` and leave its value in `res`.  The `== 0` test is
 * the off-by-default fast path: tcc_pass_timing_on is -1 until the first
 * tcc_pass_timing_begin() resolves it, so every later call skips the timer
 * entirely and the instrumentation costs one predictable branch. */
#define TCC_PASS_TIMED(res, name, call)                                        \
  do {                                                                         \
    if (tcc_pass_timing_on == 0) {                                             \
      (res) = (call);                                                          \
      break;                                                                   \
    }                                                                          \
    TCCPassTimer _tcc_pt;                                                      \
    tcc_pass_timing_begin(&_tcc_pt, (name));                                   \
    (res) = (call);                                                            \
    tcc_pass_timing_end(&_tcc_pt, (int)(res));                                 \
  } while (0)

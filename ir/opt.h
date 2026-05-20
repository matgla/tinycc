/*
 *  TCC IR - Optimization Passes
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_OPT_H
#define TCC_IR_OPT_H

struct TCCIRState;
struct TCCState;
struct IRLoops;
struct IROptCtx;

/* ============================================================================
 * Optimization Pass Functions
 * ============================================================================
 * Each pass has a legacy signature (TCCIRState *ir) and a pipeline-ready
 * _ex variant (IROptCtx *ctx).  The legacy version wraps the _ex version
 * with a temporary context. */

/* Dead Code Elimination - remove unreachable instructions */
int tcc_ir_opt_dce(struct TCCIRState *ir);
int tcc_ir_opt_dce_ex(struct IROptCtx *ctx);

/* NOP Compaction - remove NOP instructions, shrink array, fix jump targets.
 * Returns number of NOPs removed. */
int tcc_ir_opt_compact_nops(struct TCCIRState *ir);
int tcc_ir_opt_compact_nops_ex(struct IROptCtx *ctx);

/* Useless Function Body - NOP the entire body when no instruction has an
 * observable side effect (no STORE, no CALL, no RETURNVALUE, no volatile
 * sym read, etc.). */
int tcc_ir_opt_useless_function_body(struct TCCIRState *ir);
int tcc_ir_opt_useless_function_body_ex(struct IROptCtx *ctx);

/* No-Return Function Collapse - if the function never returns (no RETURN op
 * anywhere) and has no calls/asm/volatile/setjmp/trap, then nothing outside
 * the function can observe its writes; collapse the body to `b .`. */
int tcc_ir_opt_noreturn_collapse(struct TCCIRState *ir);
int tcc_ir_opt_noreturn_collapse_ex(struct IROptCtx *ctx);

/* Dead Store Elimination - remove stores to dead variables */
int tcc_ir_opt_dse(struct TCCIRState *ir);
int tcc_ir_opt_dse_ex(struct IROptCtx *ctx);

/* Dead address-taken VAR elimination - remove writes to VARs with no live reads */
int tcc_ir_opt_dead_addrvar_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_var_store_elim(struct TCCIRState *ir);

/* Redundant VAR ASSIGN elimination - kill assigns overwritten before next read */
int tcc_ir_opt_redundant_var_assign(struct TCCIRState *ir);

/* Constant Propagation - fold constant expressions */
int tcc_ir_opt_const_prop(struct TCCIRState *ir);

/* Constant Propagation (temporary variables only) */
int tcc_ir_opt_const_prop_tmp(struct TCCIRState *ir);

/* Constant fold string builtin calls such as `strcmp` and `strncmp` */
int tcc_ir_opt_const_string_calls(struct TCCIRState *ir);

/* Eliminate memcpy/memmove(dst, src, n) calls where dst and src compute the
 * same value — the copy is a provable no-op.  Triggered by `*p = *p`-style
 * aggregate self-assignments. */
int tcc_ir_opt_self_copy_elim(struct TCCIRState *ir);

/* Eliminate memmove/memcpy(dst_ptr, &stack_tmp, N) calls when the only writes
 * to stack_tmp[0..N) are local STOREs preceding the call.  Each contributing
 * STORE is rewritten to a STORE_INDEXED targeting the destination pointer at
 * the original offset; the memmove call and its params + the LEA &stack_tmp
 * are NOPed.  Eliminates a memmove call from hot loops doing complex/struct
 * assignments through a pointer destination. */
int tcc_ir_opt_memmove_to_indexed_stores(struct TCCIRState *ir);

/* Value Tracking through Arithmetic - track constants through ADD/SUB */
int tcc_ir_opt_value_tracking(struct TCCIRState *ir);

/* Constant Branch Folding - fold branches with constant conditions */
int tcc_ir_opt_branch_folding(struct TCCIRState *ir);

/* Boolean Materialization Peephole - fuse CMP+SETIF+TEST_ZERO+JUMPIF into CMP+JUMPIF */
int tcc_ir_opt_setif_branch_fuse(struct TCCIRState *ir);

/* Stack-Boolean-Diamond Peephole - collapse STORE/JUMP/STORE/TEST_ZERO/JUMPIF
 * written to a single-use stack slot into two direct branches. */
int tcc_ir_opt_stack_bool_diamond(struct TCCIRState *ir);

/* OR-bool-diamond — fold `acc |= (cond ? 1 : 0)` into a conditional OR. */
int tcc_ir_opt_or_bool_diamond(struct TCCIRState *ir);

/* VAR → TMP local forwarding. After STORE V ← T, rewrite subsequent reads of
 * V within the same BB to use T directly, avoiding the spill/reload round-trip. */
int tcc_ir_opt_var_tmp_fwd(struct TCCIRState *ir);

/* Local Load CSE. Within a basic block, when a VAR/PARAM is loaded twice into
 * different TEMPs, the second load is replaced with a copy of the first TEMP. */
int tcc_ir_opt_local_load_cse(struct TCCIRState *ir);

/* Local ALU CSE. Within a basic block, dedupe pure arithmetic ops (ADD, SUB,
 * MUL, MLA, AND, OR, XOR, SHL, SHR, SAR, ROR) with identical operands. Catches
 * cases the SSA GVN cannot: VARs that happen to be unchanged within the BB
 * (e.g. loop induction vars used in repeated `arr[i]` indexing), and MLAs
 * created by post-SSA fusion. */
int tcc_ir_opt_local_alu_cse(struct TCCIRState *ir);

/* Single-BB VAR → TMP promotion. For a non-address-taken VAR with exactly one
 * def and in-BB lval-ASSIGN reads only, redirect the def's dest to a fresh
 * TEMP and rewrite each read into a pure register copy. Copy prop + DCE then
 * collapse the remaining chain. */
int tcc_ir_opt_var_to_tmp(struct TCCIRState *ir);

/* ADD/SUB Constant Reassociation - normalize ADD chains */
int tcc_ir_opt_add_reassoc(struct TCCIRState *ir);

/* VAR self-update chain fold: combine `V = V ± C1; V = V ± C2; ...` into
 * a single `V = V ± sum`.  Catches unrolled pointer-increment loops where
 * each iteration becomes a self-update ADD and add_reassoc can't combine. */
int tcc_ir_opt_var_self_add_chain_fold(struct TCCIRState *ir);

/* CMP stack-address fold: fold `CMP V, Addr[StackLoc[Y]]` (and the
 * following JUMPIF/SELECT) when V provably equals Addr[StackLoc[X]] + N
 * with X+N == Y. */
int tcc_ir_opt_cmp_stack_addr_fold(struct TCCIRState *ir);

/* CMP Expression-Equality Fold - fold CMP when both operands are provably equal */
int tcc_ir_opt_cmp_expr_fold(struct TCCIRState *ir);

/* Copy Propagation - replace copies with originals */
int tcc_ir_opt_copy_prop(struct TCCIRState *ir);

/* Legacy copy propagation function - wrapper for tcc_ir_opt_copy_prop */
int tcc_ir_copy_propagation(struct TCCIRState *ir);

/* PACK64 peephole - collapse ZEXT + SHL #32 + ZEXT + OR -> PACK64 */
int tcc_ir_opt_pack64(struct TCCIRState *ir);

/* PACK64 tautology fold - collapse PACK64(low(X), X>>32) -> ASSIGN X */
int tcc_ir_opt_pack64_tautology(struct TCCIRState *ir);

/* ASSIGN fusion - fold `T_new = X OP Y; T_final = T_new` into one op */
int tcc_ir_opt_assign_fuse(struct TCCIRState *ir);

/* CMP narrowing - rewrite `CMP T_u64, u64_imm_with_hi_0` to 32-bit
 * when T's hi half is provably zero (SHR>=32 or ZEXT). */
int tcc_ir_opt_cmp_narrow_64(struct TCCIRState *ir);

/* Global LOAD value CSE - deduplicate loads from the same global within a BB */
int tcc_ir_opt_cse_global_load(struct TCCIRState *ir);

/* GlobalSym CSE - hoist repeated global symbol addresses to a single TEMP */
int tcc_ir_opt_globalsym_cse(struct TCCIRState *ir);

/* Identical-block loop re-rolling - collapse macro-unrolled runs into a loop */
int tcc_ir_opt_reroll(struct TCCIRState *ir);

/* Narrow CSE: deduplicate PARAM/VAR + #constant expressions */
int tcc_ir_opt_cse_param_add(struct TCCIRState *ir);

/* Deref forwarding - reuse loaded deref value in adjacent CMP */
int tcc_ir_opt_deref_fwd(struct TCCIRState *ir);

/* Boolean CSE (hash-table based, BB-scoped) */
int tcc_ir_opt_bool_cse(struct TCCIRState *ir);

/* Store-Load Forwarding */
int tcc_ir_opt_sl_forward(struct TCCIRState *ir);

/* Forward const ASSIGN to a VAR through &V LEA into deref uses.  Handles the
 * addr-taken local pattern (e.g. __attribute__((cleanup))) that var_to_tmp
 * intentionally skips. */
int tcc_ir_opt_addrof_var_fwd(struct TCCIRState *ir);

/* Forward STORE GlobalSym(X) <- T_val into subsequent in-BB deref reads of X.
 * Cross-block invalidation via calls / aliasing stores / BB boundaries. */
int tcc_ir_opt_global_sl_fwd(struct TCCIRState *ir);

/* Param-Addrof Constant-Store Fold - collapse the spill/addr/store/reload
 * sequence produced by `f(int v){ helper(&v); return v; }` after helper
 * inlining writes a known constant through &v. */
int tcc_ir_opt_param_addrof_const_fold(struct TCCIRState *ir);

/* Local-Addrof Constant-Store Fold - analogue of the param version but for
 * local variables: collapses `V = c0; helper(&V); use(V)` where helper
 * inlines into a constant STORE through &V, replacing reads of V with the
 * stored constant. */
int tcc_ir_opt_local_addrof_const_fold(struct TCCIRState *ir);

/* Constant VAR Propagation - propagate constant VARs exposed by store-load forwarding */
int tcc_ir_opt_const_var_prop(struct TCCIRState *ir);

/* Global-initializer constant propagation - replace LOAD of a static global
 * whose initializer is known and has not been written with ASSIGN of the
 * constant value. */
int tcc_ir_opt_global_init_prop(struct TCCIRState *ir);

/* Complex Constant Param Folding - pack a _Complex float local that is
 * initialized to constants and only used as one FUNCPARAMVAL into a packed
 * 64-bit complex immediate, eliminating the stack round-trip at the call. */
int tcc_ir_opt_complex_const_param_fold(struct TCCIRState *ir);

/* Dead Call Result Elimination - convert FUNCCALLVAL → FUNCCALLVOID when
 * the call's destination TEMP has no remaining reads.  Skips the
 * post-call moves the codegen would otherwise emit.
 * (moved to ir/opt_gens_call_result.c — engine generator) */

/* Pure-via-sret analysis - infer whether the current function's only
 * observable side effect is writes through its sret-pointer parameter.
 * Sets sym->f.func_pure_via_sret on success so subsequent callers can
 * apply dead-sret-call elimination at their call sites. */
void tcc_ir_analyze_pure_via_sret(struct TCCIRState *ir, struct Sym *func_sym);

/* Function write summary - per-pointer-parameter must-write byte map.
 * Computed at end-of-IR-opts (before codegen), stored in a TU-scoped
 * side table keyed by Sym*.  Consulted by tcc_ir_opt_dead_init_via_call. */
void tcc_ir_compute_func_write_summary(struct TCCIRState *ir, struct Sym *func_sym);
void tcc_ir_func_write_summary_clear_all(void);

/* Dead Init Via Call - kill stack-slot stores whose bytes are fully
 * overwritten by a subsequent CALL whose callee summary covers them. */
int tcc_ir_opt_dead_init_via_call(struct TCCIRState *ir);

/* Dead Sret Call Elimination - remove FUNCCALLVOID (or FUNCCALLVAL with
 * unused result) when the callee is func_pure_via_sret and its sret
 * target (PARAM0 = Addr[StackLoc[X]]) is a local that is never read
 * after the call.  Also nops the call's preceding FUNCPARAM ops.
 * (moved to ir/opt_gens_call_result.c — engine generator) */

/* fold_call_result_store: moved to ir/opt_gens_call_result.c — engine generator */

/* Redundant Store Elimination */
int tcc_ir_opt_store_redundant(struct TCCIRState *ir);

/* Dead Local Slot Elimination - remove writes to stack-locals that are never
 * read and whose address never escapes (except as memset PARAM0).  Also
 * removes the memset call when its target is entirely dead. */
int tcc_ir_opt_dead_local_slot_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_local_slot_elim_ex(struct IROptCtx *ctx);

/* Dead TEMP_LOCAL Elimination - remove non-call writes to anonymous
 * temp_local slots (vreg in [-9,-2]) when no later op references the slot.
 * Companion to dead_call_result's TEMP_LOCAL branch for non-CALL shapes. */
int tcc_ir_opt_dead_temp_local_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_temp_local_elim_ex(struct IROptCtx *ctx);

/* Displacement Load/Store Fusion - fuse ADD(base, #imm) + LOAD/STORE/ASSIGN-lval
 * into indexed memory op with constant index and scale=0. */
/* tcc_ir_opt_disp_fusion -> ir_gen_disp_fusion in opt_gens_fusion.c */

/* Indexed-chain fold - fuse a constant-immediate ADD that feeds an existing
 * scale=0 _INDEXED memory op into the indexed op's offset. */
/* tcc_ir_opt_indexed_chain -> ir_gen_indexed_chain in opt_gens_fusion.c */

/* Indexed-pair reorder - sink FUNCPARAMVAL past the next LOAD/STORE_INDEXED
 * so LDRD/STRD-pairable ops become adjacent for the codegen peephole. */
/* tcc_ir_opt_indexed_pair_reorder -> ir_gen_indexed_pair_reorder in opt_gens_fusion.c */

/* Call-chain result rename - rename `CALL → V; PARAMVAL[0] V` pairs to a
 * fresh TEMP per pair so the regalloc can keep the value in r0 across
 * the chain instead of moving it through a callee-saved reg each call. */
int tcc_ir_opt_call_chain_rename(struct TCCIRState *ir);

/* Stack-address ADD-operand CSE - hoist literal Addr[StackLoc[X]] operands
 * appearing in ADDs with a vreg other operand into a single TEMP per
 * unique offset, exposing SHL+ADD indexed-memory fusion. */
int tcc_ir_opt_stackoff_addr_cse(struct TCCIRState *ir);

/* LEA + deref fold - collapse `LEA Addr[StackLoc[-N]] + [ADD #K] + deref-use`
 * into a direct StackLoc access, eliminating the address-materialization op. */
int tcc_ir_opt_lea_fold(struct TCCIRState *ir);
int tcc_ir_opt_add_deref_fold(struct TCCIRState *ir);

/* Combined fusion pass: mla_fusion + indexed_memory_fusion in one loop (shared IROptDU) */
/* tcc_ir_opt_fusion_pass replaced by generators in opt_gens_fusion.c */

/* Rotation fusion: SHL(x,n) + SHR(x,32-n) + OR → ROR(x,32-n) */
/* tcc_ir_opt_rotate_fusion replaced by ir_gen_rotate_fusion in opt_gens_fusion.c */

/* Late barrel shift fusion: populates ir->barrel_shifts[] side-table.
 * Must run immediately before codegen — no passes may run between. */
void tcc_ir_barrel_shift_fusion(struct TCCIRState *ir);


/* Deref-in-ALU indexed fusion: extract deref operands into LOAD_INDEXED when
 * the address is computed by SHL+ADD (array table lookup pattern). */
/* tcc_ir_opt_deref_indexed_fusion -> ir_gen_deref_indexed_fusion in opt_gens_fusion.c */


/* Post-Increment Load/Store Fusion - fuse LOAD/STORE + ADD into post-increment op */
int tcc_ir_opt_postinc_fusion(struct TCCIRState *ir);

/* Loop-Aware Post-Increment Fusion - fuse embedded deref + latch ADD across basic blocks */
int tcc_ir_opt_loop_postinc_fusion(struct TCCIRState *ir);

/* Stack Address CSE - hoist repeated stack address computations */
int tcc_ir_opt_stack_addr_cse(struct TCCIRState *ir);

/* Stack address non-null branch folding - fold CMP(Addr[StackLoc], 0) + JUMPIF */
int tcc_ir_opt_stack_addr_nonnull_fold(struct TCCIRState *ir);

/* Entry-block store propagation - forward struct field constants across loops */
int tcc_ir_opt_entry_store_prop(struct TCCIRState *ir);

/* Non-negative value tracking & branch folding */
int tcc_ir_opt_nonneg_branch_fold(struct TCCIRState *ir);

/* Float comparison / pure boolean branch folding */
int tcc_ir_opt_float_branch_fold(struct TCCIRState *ir);

/* Value Range Propagation: derive range constraints from branch fall-through
 * paths and fold comparisons whose outcome is determined by the range. */
int tcc_ir_opt_vrp(struct TCCIRState *ir);

/* Redundant loop check elimination - fold CMP+JUMPIF in loop body when
 * implied by the loop exit condition */
int tcc_ir_opt_redundant_loop_check(struct TCCIRState *ir);

/* Float narrowing - replace double-precision math with float when safe */
int tcc_ir_opt_float_narrowing(struct TCCIRState *ir);

/* Jump Threading - forward jump targets through NOPs and jump chains */
int tcc_ir_opt_jump_threading(struct TCCIRState *ir);

/* Block Copy Init - replace memset(0)+stores pattern with BLOCK_COPY from rodata */
int tcc_ir_opt_block_copy_init(struct TCCIRState *ir);

/* Post-Increment Assign Folding - fold T=V[lval]; V=T OP x into V=V OP x */
int tcc_ir_opt_postinc_assign_fold(struct TCCIRState *ir);

/* RETURNVALUE merge - convert duplicate RETURNVALUE #imm into JUMP-to-first
 * so the codegen emits one `mov r0, imm; b epilogue` and N-1 single branches
 * instead of N copies of the 2-instruction sequence. */
int tcc_ir_opt_returnvalue_merge(struct TCCIRState *ir);

/* Conditional Select - replace if/else diamond with SELECT (ITE on ARM) */
int tcc_ir_opt_select(struct TCCIRState *ir);

/* Eliminate Fall-Through Jumps - remove redundant unconditional jumps */
int tcc_ir_opt_eliminate_fallthrough(struct TCCIRState *ir);

/* Decrement-to-Zero - transform count-up loops to count-down-to-zero */
int tcc_ir_opt_decrement_to_zero(struct TCCIRState *ir);

/* Redundant Init Elimination - remove function-entry VAR inits killed before use */
int tcc_ir_opt_redundant_init_elim(struct TCCIRState *ir);

/* Back-Edge Phi Hoisting - transform JUMPIF exit + ASSIGNs + JUMP body into
 * ASSIGNs + inverted JUMPIF body, eliminating one branch per loop */
int tcc_ir_opt_backedge_phi_hoist(struct TCCIRState *ir);

/* Forward-Diamond JUMPIF inversion (post-regalloc): when phi copies on the
 * fall-through path coalesce into no-ops, invert the JUMPIF and skip the
 * redundant bridging unconditional JUMP. */
int tcc_ir_opt_post_ra_forward_diamond(struct TCCIRState *ir);

/* ============================================================================
 * Pipeline-ready _ex variants (accept IROptCtx* for pass manager integration)
 * ============================================================================ */
int tcc_ir_opt_const_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_const_prop_tmp_ex(struct IROptCtx *ctx);
int tcc_ir_opt_const_var_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_global_init_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_value_tracking_ex(struct IROptCtx *ctx);
int tcc_ir_opt_add_reassoc_ex(struct IROptCtx *ctx);
int tcc_ir_opt_var_self_add_chain_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_cmp_stack_addr_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_cmp_expr_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_const_string_calls_ex(struct IROptCtx *ctx);
int tcc_ir_opt_self_copy_elim_ex(struct IROptCtx *ctx);
int tcc_ir_opt_copy_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_branch_folding_ex(struct IROptCtx *ctx);
int tcc_ir_opt_stack_addr_nonnull_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_setif_branch_fuse_ex(struct IROptCtx *ctx);
int tcc_ir_opt_stack_bool_diamond_ex(struct IROptCtx *ctx);
int tcc_ir_opt_or_bool_diamond_ex(struct IROptCtx *ctx);
int tcc_ir_opt_float_narrowing_ex(struct IROptCtx *ctx);
int tcc_ir_opt_pack64_ex(struct IROptCtx *ctx);
int tcc_ir_opt_pack64_tautology_ex(struct IROptCtx *ctx);
int tcc_ir_opt_cmp_narrow_64_ex(struct IROptCtx *ctx);
int tcc_ir_opt_sl_forward_ex(struct IROptCtx *ctx);
int tcc_ir_opt_deref_fwd_ex(struct IROptCtx *ctx);
int tcc_ir_opt_entry_store_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_postinc_fusion_ex(struct IROptCtx *ctx);
int tcc_ir_opt_assign_fuse_ex(struct IROptCtx *ctx);
int tcc_ir_opt_var_to_tmp_ex(struct IROptCtx *ctx);
int tcc_ir_opt_var_tmp_fwd_ex(struct IROptCtx *ctx);
int tcc_ir_opt_switch_to_data_ex(struct IROptCtx *ctx);
int tcc_ir_opt_switch_to_data(struct TCCIRState *ir);
int tcc_ir_opt_redundant_loop_check_ex(struct IROptCtx *ctx);
int tcc_ir_opt_vrp_ex(struct IROptCtx *ctx);
int tcc_ir_opt_nonneg_branch_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_float_branch_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_jump_threading_ex(struct IROptCtx *ctx);
int tcc_ir_opt_eliminate_fallthrough_ex(struct IROptCtx *ctx);
int tcc_ir_opt_dead_loop_elim_ex(struct IROptCtx *ctx);
int tcc_ir_opt_uninit_local_ub(struct TCCIRState *ir);
int tcc_ir_opt_uninit_local_ub_ex(struct IROptCtx *ctx);
int tcc_ir_opt_ub_only_body_elide(struct TCCIRState *ir);
int tcc_ir_opt_ub_only_body_elide_ex(struct IROptCtx *ctx);
int tcc_ir_opt_local_only_body_elide(struct TCCIRState *ir);
int tcc_ir_opt_local_only_body_elide_ex(struct IROptCtx *ctx);
int tcc_ir_opt_const_return_uninit_elide(struct TCCIRState *ir);
int tcc_ir_opt_const_return_uninit_elide_ex(struct IROptCtx *ctx);
int tcc_ir_opt_redundant_var_assign_ex(struct IROptCtx *ctx);
int tcc_ir_opt_dead_var_store_elim_ex(struct IROptCtx *ctx);
int tcc_ir_opt_dead_addrvar_elim_ex(struct IROptCtx *ctx);
int tcc_ir_opt_store_redundant_ex(struct IROptCtx *ctx);
int tcc_ir_opt_addrof_var_fwd_ex(struct IROptCtx *ctx);
int tcc_ir_opt_global_sl_fwd_ex(struct IROptCtx *ctx);

/* ============================================================================
 * Optimization Statistics
 * ============================================================================ */

typedef struct TCCOptStats
{
  int dce_removed;
  int dse_removed;
  int const_folded;
  int copies_propagated;
  int cse_eliminated;
  int stores_forwarded;
} TCCOptStats;

/* Get optimization statistics */
void tcc_ir_opt_stats_get(TCCOptStats *stats);

/* Reset optimization statistics */
void tcc_ir_opt_stats_reset(void);

/* ============================================================================
 * FP Offset Cache Optimization
 * ============================================================================ */

/* Initialize FP offset cache */
void tcc_ir_opt_fp_cache_init(struct TCCIRState *ir);

/* Clear FP offset cache */
void tcc_ir_opt_fp_cache_clear(struct TCCIRState *ir);

/* Free FP offset cache */
void tcc_ir_opt_fp_cache_free(struct TCCIRState *ir);

/* Lookup offset in FP cache, return register or -1 */
int tcc_ir_opt_fp_cache_lookup(struct TCCIRState *ir, int offset, int *phys_reg);

/* Record offset -> register mapping in FP cache */
void tcc_ir_opt_fp_cache_record(struct TCCIRState *ir, int offset, int phys_reg);

/* Invalidate register entry in FP cache */
void tcc_ir_opt_fp_cache_invalidate_reg(struct TCCIRState *ir, int phys_reg);

/* ============================================================================
 * Helper Functions (defined in tccir.c, used by optimization passes)
 * ============================================================================ */

/* Find the defining instruction for a vreg before a given index */
int tcc_ir_find_defining_instruction(struct TCCIRState *ir, int32_t vreg, int before_idx);

/* Check if a vreg has exactly one use (excluding a specific index) */
int tcc_ir_vreg_has_single_use(struct TCCIRState *ir, int32_t vreg, int exclude_idx);

/* ============================================================================
 * Strength Reduction for Multiply (Phase 3 of FUNCTION_CALLS_OPTIMIZATION_PLAN)
 * ============================================================================ */

/* Transform MUL by constant into shift/add/sub sequence
 * Returns number of instructions generated (0 if not transformable) */
int tcc_ir_strength_reduce_mul(struct TCCIRState *ir, int instr_idx);

/* Run strength reduction on all MUL instructions in function */
int tcc_ir_opt_strength_reduction(struct TCCIRState *ir);

/* ============================================================================
 * Induction Variable Strength Reduction (ARRAY_SUM_OPTIMIZATION_PLAN Phase 1)
 * ============================================================================ */

/* Transform array access via index into pointer increment:
 *   ptr = base + iv*stride  ->  ptr = base (in preheader); ptr += stride (in body)
 * This is the key optimization for array sum loops.
 * Returns number of transformations applied. */
int tcc_ir_opt_iv_strength_reduction(struct TCCIRState *ir);

/* IV strength reduction with pre-detected loops from LICM.
 * This avoids re-detecting loops and ensures correct indices after LICM hoisting. */
int tcc_ir_opt_iv_strength_reduction_with_loops(struct TCCIRState *ir, struct IRLoops *loops);

/* Loop Bound Rematerialization - recompute SP-relative loop bounds inside
 * the loop instead of hoisting them into callee-saved registers.
 * Returns number of rematerialized loop bounds. */
int tcc_ir_opt_loop_bound_remat(struct TCCIRState *ir);

/* Loop Unrolling - fully unroll small constant-trip-count loops.
 * Returns number of loops unrolled. */
int tcc_ir_opt_loop_unroll(struct TCCIRState *ir);

/* Loop Rotation - convert top-tested (while) loops to bottom-tested (do-while).
 * Eliminates 2 branches per iteration. Returns number of loops rotated. */
int tcc_ir_opt_loop_rotation(struct TCCIRState *ir);

/* Dead Loop Elimination - remove loops whose body has no side effects and
 * whose result VARs have constant values. Returns number of loops eliminated. */
int tcc_ir_opt_dead_loop_elim(struct TCCIRState *ir);

/* Detect whether optimized IR reduces to a constant return value.
 * Returns 1 and fills value/btype if the function is a constant. */
int tcc_ir_detect_const_result(struct TCCIRState *ir, int64_t *value, int *btype);

/* Cache/lookup constant function results for interprocedural constant propagation. */
void tcc_ir_cache_const_result(struct TCCState *s, int func_token, int64_t value, int btype);
int tcc_ir_lookup_const_result(struct TCCState *s, int func_token, int64_t *value, int *btype);

/* Replace calls to known-constant functions with their return value.
 * Returns number of calls replaced. */
int tcc_ir_opt_const_call_replace(struct TCCIRState *ir);

#endif /* TCC_IR_OPT_H */

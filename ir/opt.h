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

/* ============================================================================
 * Optimization Pass Functions
 * ============================================================================
 * Each pass has a legacy signature (TCCIRState *ir) and a pipeline-ready
 * _ex variant (IROptCtx *ctx).  The legacy version wraps the _ex version
 * with a temporary context. */

/* Dead Code Elimination - remove unreachable instructions */
int tcc_ir_opt_dce(struct TCCIRState *ir);
int tcc_ir_opt_dce_ex(struct IROptCtx *ctx);

/* Returns 1 if the callee never returns (noreturn attribute or known
 * noreturn libc names: abort/exit/_Exit/quick_exit). */
int tcc_ir_callee_is_noreturn(struct Sym *callee);

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

/* Infinite Loop Body Simplification - collapse infinite loops with no
 * externally-observable side effects to tight self-jumps. */
int tcc_ir_opt_infinite_loop_simplify(struct TCCIRState *ir);
int tcc_ir_opt_infinite_loop_simplify_ex(struct IROptCtx *ctx);

/* Dead-Code-Before-Infinite-Loop Elimination - once control enters a
 * side-effect-free infinite loop the function never returns, so stores (and
 * the address-takes / branches feeding them) that precede the loop on the
 * never-returning path are unobservable; reroute their entry edges to the loop
 * sink and NOP the rest. */
int tcc_ir_opt_dead_before_infinite_loop(struct TCCIRState *ir);
int tcc_ir_opt_dead_before_infinite_loop_ex(struct IROptCtx *ctx);

/* Return-Constant Register Reuse - a `RETURNVALUE C` reached only via the
 * equality edge of a `TEST_ZERO V` / `CMP V,#C` returns V (which provably
 * equals C there) so the backend reuses V's register instead of
 * rematerializing the constant. */
int tcc_ir_opt_return_const_reuse(struct TCCIRState *ir);
int tcc_ir_opt_return_const_reuse_ex(struct IROptCtx *ctx);

/* Trap-Only Body Suppression - after constprop folds a constant `x / 0` into
 * TCCIR_OP_TRAP and DCE NOPs the rest, the surviving body is a lone TRAP.
 * Reset dirty_registers/leaffunc/noreturn/need_frame_pointer (and let caller
 * reset `loc`) so the prologue/epilogue collapse to nothing. */
int tcc_ir_opt_trap_only_body_suppress(struct TCCIRState *ir);
int tcc_ir_opt_trap_only_body_suppress_ex(struct IROptCtx *ctx);

/* Zero-Size VLA Elimination - convert VLA_ALLOC ops whose size operand is
 * compile-time 0 (e.g. `T a[n][0]`) into NOPs and remove the matching
 * VLA_SP_SAVE/VLA_SP_RESTORE pair when nothing else changes SP between them. */
int tcc_ir_opt_zero_vla_elim(struct TCCIRState *ir);
int tcc_ir_opt_zero_vla_elim_ex(struct IROptCtx *ctx);

/* Dead-VLA-Struct Elimination - when a VLA_ALLOC's base pointer is captured
 * into a stack slot whose only readers are address-arithmetic ops that end in
 * STORE destinations (never a LOAD via the derived address, never an escape
 * via CALL / RETURN / STORE-as-value), NOP the VLA_ALLOC, the inner
 * VLA_SP_SAVE, the address-derivation chain, and the dead STOREs.  Matches
 * GCC -O2 on gcc.c-torture/execute/20040308-1.c. */
int tcc_ir_opt_dead_vla_struct_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_vla_struct_elim_ex(struct IROptCtx *ctx);

/* alloca-load forwarding: when a VLA_SP_SAVE is immediately followed by a
 * LOAD that reads back the same slot (and nothing else touches that slot),
 * retarget the VLA_SP_SAVE's destination from the slot to the LOAD's vreg
 * and NOP the LOAD.  The backend's VLA_SP_SAVE handler then emits a single
 * `mov vreg_reg, sp` instead of the `mov scratch, sp; str scratch, [slot];
 * ldr vreg, [slot]` three-op sequence.  Targets the __builtin_alloca
 * lowering. */
int tcc_ir_opt_alloca_load_fwd(struct TCCIRState *ir);
int tcc_ir_opt_alloca_load_fwd_ex(struct IROptCtx *ctx);

/* Dead-alloca elimination for VREG-target VLA_SP_SAVE — companion to
 * dead_vla_struct_elim that handles the shape produced by alloca_load_fwd
 * (VLA_SP_SAVE writes a vreg instead of a stack slot). */
int tcc_ir_opt_dead_alloca_vreg_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_alloca_vreg_elim_ex(struct IROptCtx *ctx);

/* Infinite Self-Recursion Collapse - if the function unconditionally calls
 * itself before any return path, by induction it never returns.  Collapse
 * the body to `b .`.  Matches GCC -O2 on patterns like
 * gcc.c-torture/compile/pr10153-1.c. */
int tcc_ir_opt_infinite_self_recursion(struct TCCIRState *ir, struct Sym *func_sym);

/* Noreturn-Call Epilogue Suppress - after DCE has eliminated everything past
 * a call to a noreturn callee, the function itself never returns from that
 * path.  If every RETURN op in the function has been DCE'd / removed, set
 * ir->noreturn = 1 so codegen omits the unreachable epilogue. */
int tcc_ir_opt_noreturn_call_epilogue_suppress(struct TCCIRState *ir);

/* Dead Store Elimination - remove stores to dead variables */
int tcc_ir_opt_dse(struct TCCIRState *ir);
int tcc_ir_opt_dse_ex(struct IROptCtx *ctx);

/* Dead address-taken VAR elimination - remove writes to VARs with no live reads */
int tcc_ir_opt_dead_addrvar_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_var_store_elim(struct TCCIRState *ir);

/* Trailing-dead-store elimination for addr-taken VARs — picks up writes
 * that follow the LAST read of the VAR (which addrvar misses because the
 * VAR appears live overall). */
int tcc_ir_opt_dead_trailing_addrvar_store_elim(struct TCCIRState *ir);

/* Redundant VAR ASSIGN elimination - kill assigns overwritten before next read */
int tcc_ir_opt_redundant_var_assign(struct TCCIRState *ir);

/* Constant Propagation (temporary variables only) */
int tcc_ir_opt_const_prop_tmp(struct TCCIRState *ir);
/* Ungated core of the above (no pass-disable check / timing); shared with the
 * SSA-time analog ssa_opt_const_prop_tmp so both call one implementation. */
int tcc_ir_opt_const_prop_tmp_core(struct TCCIRState *ir);

/* Known-Bits Propagation — tracks per-TMP known_zero/known_one bit masks
 * to fold bitfield insert/extract chains that const_prop misses. */
int tcc_ir_opt_known_bits(struct TCCIRState *ir);

/* DSE for STOREs through a LEA-temp deref (e.g. T = Addr[StackLoc[X]];
 * STORE T***DEREF***).  Complements dead_local_slot_elim, which only NOPs
 * STOREs whose dest is a direct StackLoc[X] operand. */
int tcc_ir_opt_dead_lea_store_elim(struct TCCIRState *ir);

/* Constant-fold read-modify-write chains (e.g. `u.e.a++` -> __aeabi_dadd) on
 * non-escaping local aggregates by propagating the slot's constant value
 * across calls and folding the dadd/dsub.
 * See source/opt/flat/scalar/const_aggregate.c. */
int tcc_ir_opt_const_aggregate_fold(struct TCCIRState *ir);

/* Constant fold string builtin calls such as `strcmp` and `strncmp` */
int tcc_ir_opt_const_string_calls(struct TCCIRState *ir);

/* Eliminate memmove/memcpy(dst_ptr, &stack_tmp, N) calls when the only writes
 * to stack_tmp[0..N) are local STOREs preceding the call.  Each contributing
 * STORE is rewritten to a STORE_INDEXED targeting the destination pointer at
 * the original offset; the memmove call and its params + the LEA &stack_tmp
 * are NOPed.  Eliminates a memmove call from hot loops doing complex/struct
 * assignments through a pointer destination. */
int tcc_ir_opt_memmove_to_indexed_stores(struct TCCIRState *ir);

/* Value Tracking through Arithmetic - track constants through ADD/SUB */
int tcc_ir_opt_value_tracking(struct TCCIRState *ir);

/* Boolean Materialization Peephole - fuse CMP+SETIF+TEST_ZERO+JUMPIF into CMP+JUMPIF */
int tcc_ir_opt_setif_branch_fuse(struct TCCIRState *ir);

/* Stack-Boolean-Diamond Peephole - collapse STORE/JUMP/STORE/TEST_ZERO/JUMPIF
 * written to a single-use stack slot into two direct branches. */
int tcc_ir_opt_stack_bool_diamond(struct TCCIRState *ir);

/* SETIF OR-chain tautology fold — fold OR chains of CMP+SETIF results whose
 * conditions together cover every LT/EQ/GT outcome into ASSIGN #1. */
int tcc_ir_opt_setif_or_tautology(struct TCCIRState *ir);

/* VAR → TMP local forwarding (tcc_ir_opt_var_tmp_fwd) moved to
 * source/opt/flat/scalar/var_tmp_fwd.c — declared in opt/flat/var_tmp_fwd.h. */

/* Local Load CSE. Within a basic block, when a VAR/PARAM is loaded twice into
 * different TEMPs, the second load is replaced with a copy of the first TEMP. */
int tcc_ir_opt_local_load_cse(struct TCCIRState *ir);

/* Single-BB VAR → TMP promotion. For a non-address-taken VAR with exactly one
 * def and in-BB lval-ASSIGN reads only, redirect the def's dest to a fresh
 * TEMP and rewrite each read into a pure register copy. Copy prop + DCE then
 * collapse the remaining chain. */
int tcc_ir_opt_var_to_tmp(struct TCCIRState *ir);

/* ADD/SUB Constant Reassociation - normalize ADD chains */
int tcc_ir_opt_add_reassoc(struct TCCIRState *ir);

/* CMP stack-address fold: fold `CMP V, Addr[StackLoc[Y]]` (and the
 * following JUMPIF/SELECT) when V provably equals Addr[StackLoc[X]] + N
 * with X+N == Y. */
int tcc_ir_opt_cmp_stack_addr_fold(struct TCCIRState *ir);

/* Stack-address simplification: rewrite derefs through known stack-address
 * temps to direct StackLoc accesses and fold stack-address differences. */
int tcc_ir_opt_stack_addr_simplify(struct TCCIRState *ir);

/* CMP Expression-Equality Fold - fold CMP when both operands are provably equal */
int tcc_ir_opt_cmp_expr_fold(struct TCCIRState *ir);

/* PACK64 peephole - collapse ZEXT + SHL #32 + ZEXT + OR -> PACK64 */
int tcc_ir_opt_pack64(struct TCCIRState *ir);

/* PACK64 peephole (implicit-ZEXT variant) - collapse `(X_hi SHL #32) OR X_lo`
 * -> PACK64 when both halves are 32-bit values relying on implicit
 * zero-extension into the i64 OR. */
int tcc_ir_opt_pack64_implicit(struct TCCIRState *ir);

/* PACK64 tautology fold - collapse PACK64(low(X), X>>32) -> ASSIGN X */
int tcc_ir_opt_pack64_tautology(struct TCCIRState *ir);

/* PACK64 from adjacent narrow stack stores - rewrite a 64-bit LOAD from
 * StackLoc[A] as PACK64(val_lo, val_hi) when the LOAD is preceded by two
 * 32-bit STOREs to StackLoc[A] (lo) and StackLoc[A+4] (hi).  Eliminates
 * the spill+ldrd that ARM param prologues emit when a function takes a
 * pair-passed argument and returns it directly as a long long / 8-byte
 * scalar (e.g. `long long f(V2SI x) { return (long long)x; }`). */
int tcc_ir_opt_pack64_from_stack_stores(struct TCCIRState *ir);

/* SHL32-OR chain fold - collapse `((X SHL 32) OR Y) SHL 32` -> `Y SHL 32`
 * and `((X SHL 32) OR Y) AND #0xFFFFFFFF` -> `Y AND #0xFFFFFFFF`.  Cuts
 * out the dead sign-extension high half from the i32→i64 widen idiom when
 * the consumer shifts/masks it out anyway. */
int tcc_ir_opt_shl32_or_chain(struct TCCIRState *ir);

/* ASSIGN fusion - fold `T_new = X OP Y; T_final = T_new` into one op */
int tcc_ir_opt_assign_fuse(struct TCCIRState *ir);

/* CMP narrowing - rewrite `CMP T_u64, u64_imm_with_hi_0` to 32-bit
 * when T's hi half is provably zero (SHR>=32 or ZEXT). */
int tcc_ir_opt_cmp_narrow_64(struct TCCIRState *ir);

/* Dead-half annotation for 64-bit shifts: flags SHL/SHR/SAR results whose low
 * or high word is provably unread, so codegen skips the dead half-write. */
int tcc_ir_opt_shift64_dead_half(struct TCCIRState *ir);

/* Global LOAD value CSE - deduplicate loads from the same global within a BB */
int tcc_ir_opt_cse_global_load(struct TCCIRState *ir);

/* Identical-block loop re-rolling - collapse macro-unrolled runs into a loop */
int tcc_ir_opt_reroll(struct TCCIRState *ir);

/* Negation-chain CSE - collapse repeated `T = -T` chains by tracking each
 * TEMP's canonical (base, sign) pair.  After the first two unique negation
 * states are seen, subsequent SUBs in the chain are rewritten as ASSIGN to
 * the earliest TEMP with that form so copy-prop + DCE can collapse them.
 * Targets goto-chain idioms like gcc.c-torture/compile/961126-1.c. */
int tcc_ir_opt_neg_chain_cse(struct TCCIRState *ir);
int tcc_ir_opt_neg_chain_cse_ex(struct IROptCtx *ctx);

/* Redundant bitfield insert/extract elimination: `((V<<n) | low) >> n` -> V
 * (when low < 2^n and V < 2^(32-n)).  Collapses the read-back of a just-poked
 * bitfield in a dead local struct (gcc.c-torture/execute/20040709-1.c). */
int tcc_ir_opt_bitfield_insert_extract(struct TCCIRState *ir);
int tcc_ir_opt_bitfield_insert_extract_ex(struct IROptCtx *ctx);

/* Bitfield insert -> ARM BFI: (W & ~field) | (V << lsb) for a contiguous field
 * (V < 2^width) becomes BFI Rd, V, #lsb, #width.  Lowers the observed-insert
 * idiom (`s.k += x` global-bitfield RMW, fn3* in 20040709-2.c) that the extract
 * fold above cannot reach.  Must run before barrel_shift_fusion.  Records
 * lsb/width in ir->bfi_params[orig_index]. */
int tcc_ir_opt_bitfield_insert_to_bfi(struct TCCIRState *ir);

/* Aggregate field-compare fusion: a run of `a.fi != b.fi` bitfield compares
 * all branching to the same label -> one masked word XOR compare.  Runs in
 * propagation (before shift-into-CMP fusion makes the two sides asymmetric). */
int tcc_ir_opt_cmp_field_fuse(struct TCCIRState *ir);
int tcc_ir_opt_cmp_field_fuse_ex(struct IROptCtx *ctx);


/* Store-Load Forwarding */
int tcc_ir_opt_sl_forward(struct TCCIRState *ir);

/* Forward const ASSIGN to a VAR through &V LEA into deref uses.  Handles the
 * addr-taken local pattern (e.g. __attribute__((cleanup))) that var_to_tmp
 * intentionally skips. */
int tcc_ir_opt_addrof_var_fwd(struct TCCIRState *ir);

/* Forward STORE GlobalSym(X) <- T_val into subsequent in-BB deref reads of X.
 * Cross-block invalidation via calls / aliasing stores / BB boundaries. */
int tcc_ir_opt_global_sl_fwd(struct TCCIRState *ir);

/* Invariant Global LOAD Hoist - cross-BB CSE for ASSIGN/LOAD of globals when
 * the function has only forward control flow and no aliasing stores.  Catches
 * unrolled goto-chain patterns (gcc.c-torture/compile/961126-1.c) where the
 * same `*p` is reloaded at every conditional check. */
int tcc_ir_opt_invariant_global_load_hoist(struct TCCIRState *ir);
int tcc_ir_opt_invariant_global_load_hoist_ex(struct IROptCtx *ctx);

/* Invariant TEMP-deref Hoist - companion to the global load hoist.  Inserts
 * one explicit `T_v = T***DEREF***` after a singly-defined pointer TEMP and
 * rewrites later `T***DEREF***` uses to T_v non-lval, so the repeated
 * deref-in-CMP pattern compiles to one LDR + many CMP. */
int tcc_ir_opt_invariant_temp_deref_hoist(struct TCCIRState *ir);
int tcc_ir_opt_invariant_temp_deref_hoist_ex(struct IROptCtx *ctx);

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

/* Symref-constant propagation - propagate ASSIGN T = symref(S, +A) into
 * subsequent uses of T within the same straight-line block, so that
 * `T***DEREF***` becomes `symref(S,+A)***DEREF***` for the global-init
 * pass to fold against the section data. */
int tcc_ir_opt_symref_const_prop(struct TCCIRState *ir);

/* Complex Constant Param Folding - pack a _Complex float local that is
 * initialized to constants and only used as one FUNCPARAMVAL into a packed
 * 64-bit complex immediate, eliminating the stack round-trip at the call. */
int tcc_ir_opt_complex_const_param_fold(struct TCCIRState *ir);

/* Dead Call Result Elimination - convert FUNCCALLVAL → FUNCCALLVOID when
 * the call's destination TEMP has no remaining reads.  Skips the
 * post-call moves the codegen would otherwise emit.
 * (moved to source/opt/flat/scalar/call_result.c — engine generator) */

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

/* TU-wide read/call summary - per-function record of:
 *   - which static globals are read or address-taken
 *   - which static globals are written
 *   - which functions (Sym*) are statically called
 * Computed at end-of-IR-opts (from optimized IR).  Consumed at end-of-TU by
 * tcc_ir_tu_analyze_dead_statics to mark static globals with no reachable
 * readers — their stores can then be eliminated during late_reopt. */
void tcc_ir_collect_tu_func_summary(struct TCCIRState *ir, struct Sym *func_sym);
/* Pre-optimization static-read scan; see tu_source_reads in opt.c. */
void tcc_ir_collect_tu_static_reads_preopt(struct TCCIRState *ir);
void tcc_ir_tu_func_summary_clear_all(void);
/* TU end-of-parse analysis: build call graph reachability, compute live
 * static-global read set, and mark dead statics + their writer functions. */
void tcc_ir_tu_analyze_dead_statics(void);

/* TU end-of-parse propagation: for any caller that preserved its tokens
 * via func_keep_tokens_for_noreturn (set by the gen_function trigger),
 * check if any callee turned out to be func_noreturn; if so, set
 * func_late_reopt on the caller so gen_late_reopt_functions re-emits it
 * with the noreturn-call-DCE optimization applied. */
void tcc_ir_tu_propagate_noreturn_to_callers(void);

/* Dead-static-global store elimination - NOP stores to statics that the
 * end-of-TU analysis confirmed have no reachable readers. */
int tcc_ir_opt_dead_static_store_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_static_store_elim_ex(struct IROptCtx *ctx);

/* Global Base Sharing - merge clusters of STOREs to same-section globals into
 * a single LEA + STORE_INDEXED sequence, eliminating per-store PC-relative
 * literal-pool loads of symbol addresses. */
int tcc_ir_opt_global_base_share(struct TCCIRState *ir);
int tcc_ir_opt_global_base_share_ex(struct IROptCtx *ctx);

/* Dead Init Via Call - kill stack-slot stores whose bytes are fully
 * overwritten by a subsequent CALL whose callee summary covers them. */
int tcc_ir_opt_dead_init_via_call(struct TCCIRState *ir);
int tcc_ir_opt_dead_local_slot_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_local_slot_elim_ex(struct IROptCtx *ctx);
int tcc_ir_opt_dead_temp_local_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_temp_local_elim_ex(struct IROptCtx *ctx);
int tcc_ir_opt_call_chain_rename(struct TCCIRState *ir);
int tcc_ir_opt_stackoff_addr_cse(struct TCCIRState *ir);
int tcc_ir_opt_lea_fold(struct TCCIRState *ir);
int tcc_ir_opt_lea_rmw_fold(struct TCCIRState *ir);
int tcc_ir_opt_add_deref_fold(struct TCCIRState *ir);
void tcc_ir_barrel_shift_fusion(struct TCCIRState *ir);
int tcc_ir_opt_shift_pair_to_ubfx(struct TCCIRState *ir);
int tcc_ir_opt_stack_addr_cse(struct TCCIRState *ir);
int tcc_ir_opt_stack_addr_nonnull_fold(struct TCCIRState *ir);
int tcc_ir_opt_entry_store_prop(struct TCCIRState *ir);
int tcc_ir_opt_float_branch_fold(struct TCCIRState *ir);
int tcc_ir_opt_vrp(struct TCCIRState *ir);
int tcc_ir_opt_redundant_loop_check(struct TCCIRState *ir);
int tcc_ir_opt_float_narrowing(struct TCCIRState *ir);
int tcc_ir_opt_jump_threading(struct TCCIRState *ir);
int tcc_ir_opt_orphan_cmp_elim(struct TCCIRState *ir);

/* JUMPIF inversion - fold `JUMPIF C -> A; JUMP -> B` (A = next real instr
 * after the JUMP) into `JUMPIF !C -> B`.  Cascade sibling of jump_threading.
 * allow_backward=0 pre-RA keeps rotation's body->latch JUMP intact. */
int tcc_ir_opt_jumpif_invert(struct TCCIRState *ir, int allow_backward);

/* Block Copy Init - replace memset(0)+stores pattern with BLOCK_COPY from rodata */
int tcc_ir_opt_block_copy_init(struct TCCIRState *ir);

/* Small zero-memset to direct STORE - replace memset(stack, N<=8, 0) with one or
 * two direct STORE #0 instructions when block_copy_init didn't fire. */
int tcc_ir_opt_small_memset_to_store(struct TCCIRState *ir);

/* Small zero-memset to a GLOBAL (symref) destination - replace
 * memset(&global[off], 0, N) with a single naturally-aligned direct STORE #0
 * (strb/strh/str/strd) when N is exactly a single store's width. */
int tcc_ir_opt_small_global_memset_to_store(struct TCCIRState *ir);

/* ssa:mem_init - flat-region driver (regalloc entry) that runs the frontend
 * memory-init lowering trio (block_copy_init + the two small-memset passes)
 * on raw IR before CFG/SSA construction.  See docs/plan_legacy_flat_ir_ssa_retire.md. */
int ssa_opt_mem_init(struct TCCIRState *ir);

/* RETURNVALUE merge - convert duplicate RETURNVALUE #imm into JUMP-to-first
 * so the codegen emits one `mov r0, imm; b epilogue` and N-1 single branches
 * instead of N copies of the 2-instruction sequence. */
int tcc_ir_opt_returnvalue_merge(struct TCCIRState *ir);

/* Conditional Select (if/else diamond -> SELECT) and setif_neg_to_select now
 * live in source/opt/flat/cfg/if_convert.c; declared in opt/flat/if_convert.h */

/* Eliminate Fall-Through Jumps - remove redundant unconditional jumps */
int tcc_ir_opt_eliminate_fallthrough(struct TCCIRState *ir);

/* Back-Edge Phi Hoisting - transform JUMPIF exit + ASSIGNs + JUMP body into
 * ASSIGNs + inverted JUMPIF body, eliminating one branch per loop */
int tcc_ir_opt_backedge_phi_hoist(struct TCCIRState *ir);

/* Forward-Diamond JUMPIF inversion (post-regalloc) now lives in
 * source/opt/flat/cfg/if_convert.c; declared in opt/flat/if_convert.h */

/* Abort tail-merge + body-invert (post-regalloc): per distinct noreturn callee,
 * keep the first guarded call inline as a shared sink and invert+retarget every
 * later guard to branch to it, NOPing the duplicate calls.  Matches GCC's single
 * shared `bl abort` shape.  Disabled by TCC_NO_ABORT_MERGE. */
int tcc_ir_opt_abort_tail_merge(struct TCCIRState *ir);

/* ============================================================================
 * Pipeline-ready _ex variants (accept IROptCtx* for pass manager integration)
 * ============================================================================ */
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
int tcc_ir_opt_const_string_calls_ex(struct IROptCtx *ctx);
int tcc_ir_opt_stack_addr_nonnull_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_setif_branch_fuse_ex(struct IROptCtx *ctx);
int tcc_ir_opt_stack_bool_diamond_ex(struct IROptCtx *ctx);
int tcc_ir_opt_setif_or_tautology_ex(struct IROptCtx *ctx);
int tcc_ir_opt_float_narrowing_ex(struct IROptCtx *ctx);
int tcc_ir_opt_pack64_ex(struct IROptCtx *ctx);
int tcc_ir_opt_pack64_tautology_ex(struct IROptCtx *ctx);
int tcc_ir_opt_pack64_from_stack_stores_ex(struct IROptCtx *ctx);
int tcc_ir_opt_cmp_narrow_64_ex(struct IROptCtx *ctx);
int tcc_ir_opt_sl_forward_ex(struct IROptCtx *ctx);
int tcc_ir_opt_entry_store_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_assign_fuse_ex(struct IROptCtx *ctx);
int tcc_ir_opt_var_to_tmp_ex(struct IROptCtx *ctx);
int tcc_ir_opt_switch_to_data_ex(struct IROptCtx *ctx);
int tcc_ir_opt_switch_to_data(struct TCCIRState *ir);
int tcc_ir_opt_switch_collapse_ex(struct IROptCtx *ctx);
int tcc_ir_opt_switch_collapse(struct TCCIRState *ir);
int tcc_ir_opt_redundant_loop_check_ex(struct IROptCtx *ctx);
int tcc_ir_opt_vrp_ex(struct IROptCtx *ctx);
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
/* Struct-copy round-trip elimination — drop a `memmove(B,A,N); memmove(A,B,N)`
 * pair (the inlined identity `y = retme(y)` shape) where B is a pure dead
 * round-trip temp and A is unmodified between the two copies. */
int tcc_ir_opt_struct_copy_roundtrip_elim(struct TCCIRState *ir);
/* Init-copy-from-global load forwarding — when a `memmove(local, &global, N)`
 * fills a private read-only stack slot, rewrite the slot's loads to read the
 * global directly and drop the copy (the `struct y = global; return y.f` idiom). */
int tcc_ir_opt_memmove_global_load_fwd(struct TCCIRState *ir);
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

/* Insert a quad before an index, renumbering jump + switch-table targets */
int tcc_ir_insert_instruction_before(struct TCCIRState *ir, int before_idx, struct IRQuadCompact *new_q);

/* ============================================================================
 * Induction Variable Strength Reduction (ARRAY_SUM_OPTIMIZATION_PLAN Phase 1)
 * ============================================================================ */

/* IV strength reduction is owned by the SSA pass ssa:iv_strength_reduction
 * (ir/opt/ssa_opt_loop.c), which drives the shared engine
 * iv_strength_reduction_core (ir/opt_loop_utils.c) from the regalloc-time flat
 * region; the legacy pre-SSA drivers tcc_ir_opt_iv_strength_reduction / _with_loops
 * were removed. See docs/plan_legacy_loop_iv_strength_reduction_ssa.md. */

/* Dead-loop elimination is owned by the SSA pass ssa:dead_loop
 * (ir/opt/ssa_opt_dead_loop.c); the legacy pre-SSA tcc_ir_opt_dead_loop_elim
 * was retired 2026-07-07 (proven inert at its tccgen site). See
 * docs/plan_legacy_loop_dead_loop_elim_ssa.md. */

/* Detect whether optimized IR reduces to a constant return value.
 * Returns 1 and fills value/btype if the function is a constant. */
int tcc_ir_detect_const_result(struct TCCIRState *ir, int64_t *value, int *btype);

/* Cache/lookup constant function results for interprocedural constant propagation. */
void tcc_ir_cache_const_result(struct TCCState *s, int func_token, int64_t value, int btype);
int tcc_ir_lookup_const_result(struct TCCState *s, int func_token, int64_t *value, int *btype);

/* Replace calls to known-constant functions with their return value.
 * Returns number of calls replaced. */
int tcc_ir_opt_const_call_replace(struct TCCIRState *ir);

/* "Switch-value function" snapshot: a static, side-effect-free function with
 * one scalar parameter whose body lowers to ASSIGN/CMP/JUMP/JUMPIF/RETURNVALUE.
 * The snapshot holds a compact replay of the body so callers can evaluate the
 * return value for any constant argument via simulation.  Opaque to callers
 * outside opt_constfold.c. */
struct TCCFuncSwitchSnapshot;
typedef struct TCCFuncSwitchSnapshot TCCFuncSwitchSnapshot;

/* Try to classify `ir` as a switch-value function and snapshot it.
 * On success, returns 1 and stores an owned snapshot in *out.
 * On failure, returns 0 and leaves *out unchanged. */
int tcc_ir_detect_switch_func(struct TCCIRState *ir, TCCFuncSwitchSnapshot **out);

/* Free a snapshot previously returned by tcc_ir_detect_switch_func. */
void tcc_ir_switch_func_snapshot_free(TCCFuncSwitchSnapshot *snap);

/* Cache a snapshot under `func_token`.  Takes ownership of `snap` even on
 * failure (it will be freed if the cache is full or already has an entry). */
void tcc_ir_cache_switch_func(struct TCCState *s, int func_token, TCCFuncSwitchSnapshot *snap);

/* Look up a previously cached switch-value snapshot.  Returns NULL if not found. */
const TCCFuncSwitchSnapshot *tcc_ir_lookup_switch_func(struct TCCState *s, int func_token);

/* Simulate the snapshot with `arg_value` for its single parameter.
 * On success, returns 1 and stores the return value in *out_value (and *out_btype if non-NULL).
 * On failure (unsupported op, unknown vreg, step limit), returns 0. */
int tcc_ir_simulate_switch_func(const TCCFuncSwitchSnapshot *snap, int64_t arg_value,
                                int64_t *out_value, int *out_btype);

/* Extended simulate: also collects an in-order list of snapshot-op indices the
 * caller must replay to preserve side effects (loads/stores/arithmetic on
 * constant-symref globals).  Pass `replay_indices`=NULL to reject any function
 * whose execution would require replay (matches the pure-folding contract).
 * `replay_count` receives the number of recorded indices. */
int tcc_ir_simulate_switch_func_ex(const TCCFuncSwitchSnapshot *snap, int64_t arg_value,
                                   int64_t *out_value, int *out_btype,
                                   int *replay_indices, int *replay_count);

/* Release all cached switch-value snapshots in `s`.  Called from tcc_delete. */
void tcc_ir_free_switch_func_cache(struct TCCState *s);

/* Replace FUNCCALLVAL to a cached switch-value function with the constant
 * return value when the single argument is a constant.  Sister of
 * tcc_ir_opt_const_call_replace; counts call sites it rewrote. */
int tcc_ir_opt_switch_call_replace(struct TCCIRState *ir);

/* Per-pass timing instrumentation (opt-in via TCC_PASS_TIMING env var). */
extern signed char tcc_pass_timing_on;
void tcc_pass_timing_init(void);
unsigned long tcc_pass_clk_us(void);
void tcc_pass_timing_add(const char *name, unsigned long us);
void tcc_pass_timing_dump(void);


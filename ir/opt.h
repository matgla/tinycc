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

/* Orphan CMP/TEST_ZERO elimination - remove flag-setting ops whose flags
 * are not consumed by any SETIF/JUMPIF before the next clobber or BB end. */
int tcc_ir_opt_orphan_cmp_elim(struct TCCIRState *ir);
int tcc_ir_opt_orphan_cmp_elim_ex(struct IROptCtx *ctx);

/* Dead address-taken VAR elimination - remove writes to VARs with no live reads */
int tcc_ir_opt_dead_addrvar_elim(struct TCCIRState *ir);
int tcc_ir_opt_dead_var_store_elim(struct TCCIRState *ir);

/* Trailing-dead-store elimination for addr-taken VARs — picks up writes
 * that follow the LAST read of the VAR (which addrvar misses because the
 * VAR appears live overall). */
int tcc_ir_opt_dead_trailing_addrvar_store_elim(struct TCCIRState *ir);

/* Redundant VAR ASSIGN elimination - kill assigns overwritten before next read */
int tcc_ir_opt_redundant_var_assign(struct TCCIRState *ir);

/* Constant Propagation - fold constant expressions */
int tcc_ir_opt_const_prop(struct TCCIRState *ir);

/* Constant Propagation (temporary variables only) */
int tcc_ir_opt_const_prop_tmp(struct TCCIRState *ir);

/* Known-Bits Propagation — tracks per-TMP known_zero/known_one bit masks
 * to fold bitfield insert/extract chains that const_prop misses. */
int tcc_ir_opt_known_bits(struct TCCIRState *ir);

/* DSE for STOREs through a LEA-temp deref (e.g. T = Addr[StackLoc[X]];
 * STORE T***DEREF***).  Complements dead_local_slot_elim, which only NOPs
 * STOREs whose dest is a direct StackLoc[X] operand. */
int tcc_ir_opt_dead_lea_store_elim(struct TCCIRState *ir);

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

/* SETIF OR-chain tautology fold — fold OR chains of CMP+SETIF results whose
 * conditions together cover every LT/EQ/GT outcome into ASSIGN #1. */
int tcc_ir_opt_setif_or_tautology(struct TCCIRState *ir);

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

/* Stack-address simplification: rewrite derefs through known stack-address
 * temps to direct StackLoc accesses and fold stack-address differences. */
int tcc_ir_opt_stack_addr_simplify(struct TCCIRState *ir);

/* CMP Expression-Equality Fold - fold CMP when both operands are provably equal */
int tcc_ir_opt_cmp_expr_fold(struct TCCIRState *ir);

/* Self-expression arithmetic identity fold: x/x→1, x%x→0 */
int tcc_ir_opt_self_arith_fold(struct TCCIRState *ir);
int tcc_ir_opt_self_arith_fold_ex(struct IROptCtx *ctx);

/* CMP Constant-Offset Fold - fold CMP when one operand is the other plus a
 * known constant (e.g. `(x + 1) >= x` → always true under signed-overflow UB) */
int tcc_ir_opt_cmp_const_offset_fold(struct TCCIRState *ir);

/* Copy Propagation - replace copies with originals */
int tcc_ir_opt_copy_prop(struct TCCIRState *ir);

/* Legacy copy propagation function - wrapper for tcc_ir_opt_copy_prop */
int tcc_ir_copy_propagation(struct TCCIRState *ir);

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

/* Global LOAD value CSE - deduplicate loads from the same global within a BB */
int tcc_ir_opt_cse_global_load(struct TCCIRState *ir);

/* GlobalSym CSE - hoist repeated global symbol addresses to a single TEMP */
int tcc_ir_opt_globalsym_cse(struct TCCIRState *ir);

/* Identical-block loop re-rolling - collapse macro-unrolled runs into a loop */
int tcc_ir_opt_reroll(struct TCCIRState *ir);

/* Negation-chain CSE - collapse repeated `T = -T` chains by tracking each
 * TEMP's canonical (base, sign) pair.  After the first two unique negation
 * states are seen, subsequent SUBs in the chain are rewritten as ASSIGN to
 * the earliest TEMP with that form so copy-prop + DCE can collapse them.
 * Targets goto-chain idioms like gcc.c-torture/compile/961126-1.c. */
int tcc_ir_opt_neg_chain_cse(struct TCCIRState *ir);
int tcc_ir_opt_neg_chain_cse_ex(struct IROptCtx *ctx);

/* Narrow CSE: deduplicate PARAM/VAR + #constant expressions */
int tcc_ir_opt_cse_param_add(struct TCCIRState *ir);

/* Deref forwarding - reuse loaded deref value in adjacent CMP */
int tcc_ir_opt_deref_fwd(struct TCCIRState *ir);

/* Pointer-deref load CSE - eliminate redundant loads through same pointer */
int tcc_ir_opt_ptr_load_cse(struct TCCIRState *ir);

/* Pointer store-to-load forwarding - forward stored values to subsequent
 * loads from the same pointer dereference within a basic block */
int tcc_ir_opt_ptr_store_load_fwd(struct TCCIRState *ir);

/* Boolean CSE (hash-table based, BB-scoped) */
int tcc_ir_opt_bool_cse(struct TCCIRState *ir);

/* Store-Load Forwarding */
int tcc_ir_opt_sl_forward(struct TCCIRState *ir);

/* Diamond Store Forwarding - when both branches of an if/else diamond store
 * the same constant through a computed address, forward the constant to the
 * post-merge LOAD_INDEXED from the same address. */
int tcc_ir_opt_diamond_store_fwd(struct TCCIRState *ir);

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

/* TU-wide read/call summary - per-function record of:
 *   - which static globals are read or address-taken
 *   - which static globals are written
 *   - which functions (Sym*) are statically called
 * Computed at end-of-IR-opts (from optimized IR).  Consumed at end-of-TU by
 * tcc_ir_tu_analyze_dead_statics to mark static globals with no reachable
 * readers — their stores can then be eliminated during late_reopt. */
void tcc_ir_collect_tu_func_summary(struct TCCIRState *ir, struct Sym *func_sym);
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

/* LEA CSE — within a basic block, collapse repeated LEAs of the same stack
 * address into a single canonical LEA + ASSIGN copies, exposing copy_prop +
 * DCE follow-up.  Targets the per-element address-of pattern emitted for
 * unrolled vector temp writes. */
int tcc_ir_opt_lea_cse(struct TCCIRState *ir);

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

/* Small zero-memset to direct STORE - replace memset(stack, N<=8, 0) with one or
 * two direct STORE #0 instructions when block_copy_init didn't fire. */
int tcc_ir_opt_small_memset_to_store(struct TCCIRState *ir);

/* CMP+SETIF CSE - within a basic block, replace a second CMP+SETIF whose
 * operands and cond match an earlier one with ASSIGN-from-prior-vreg.
 * Cuts a redundant compare-and-set when the same boolean is computed twice. */
int tcc_ir_opt_cmp_setif_cse(struct TCCIRState *ir);

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
int tcc_ir_opt_single_value_tmp_ex(struct IROptCtx *ctx);
int tcc_ir_opt_known_bits_ex(struct IROptCtx *ctx);
int tcc_ir_opt_dead_lea_store_elim_ex(struct IROptCtx *ctx);
int tcc_ir_opt_const_var_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_global_init_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_symref_const_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_value_tracking_ex(struct IROptCtx *ctx);
int tcc_ir_opt_add_reassoc_ex(struct IROptCtx *ctx);
int tcc_ir_opt_var_self_add_chain_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_cmp_stack_addr_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_cmp_expr_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_cmp_const_offset_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_const_string_calls_ex(struct IROptCtx *ctx);
int tcc_ir_opt_self_copy_elim_ex(struct IROptCtx *ctx);
int tcc_ir_opt_copy_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_branch_folding_ex(struct IROptCtx *ctx);
int tcc_ir_opt_stack_addr_nonnull_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_setif_branch_fuse_ex(struct IROptCtx *ctx);
int tcc_ir_opt_stack_bool_diamond_ex(struct IROptCtx *ctx);
int tcc_ir_opt_or_bool_diamond_ex(struct IROptCtx *ctx);
int tcc_ir_opt_setif_or_tautology_ex(struct IROptCtx *ctx);
int tcc_ir_opt_float_narrowing_ex(struct IROptCtx *ctx);
int tcc_ir_opt_pack64_ex(struct IROptCtx *ctx);
int tcc_ir_opt_pack64_tautology_ex(struct IROptCtx *ctx);
int tcc_ir_opt_pack64_from_stack_stores_ex(struct IROptCtx *ctx);
int tcc_ir_opt_cmp_narrow_64_ex(struct IROptCtx *ctx);
int tcc_ir_opt_sl_forward_ex(struct IROptCtx *ctx);
int tcc_ir_opt_deref_fwd_ex(struct IROptCtx *ctx);
int tcc_ir_opt_ptr_load_cse_ex(struct IROptCtx *ctx);
int tcc_ir_opt_ptr_store_load_fwd_ex(struct IROptCtx *ctx);
int tcc_ir_opt_entry_store_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_postinc_fusion_ex(struct IROptCtx *ctx);
int tcc_ir_opt_assign_fuse_ex(struct IROptCtx *ctx);
int tcc_ir_opt_var_to_tmp_ex(struct IROptCtx *ctx);
int tcc_ir_opt_var_tmp_fwd_ex(struct IROptCtx *ctx);
int tcc_ir_opt_switch_to_data_ex(struct IROptCtx *ctx);
int tcc_ir_opt_switch_to_data(struct TCCIRState *ir);
int tcc_ir_opt_switch_collapse_ex(struct IROptCtx *ctx);
int tcc_ir_opt_switch_collapse(struct TCCIRState *ir);
int tcc_ir_opt_redundant_loop_check_ex(struct IROptCtx *ctx);
int tcc_ir_opt_vrp_ex(struct IROptCtx *ctx);
int tcc_ir_opt_nonneg_branch_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_float_branch_fold_ex(struct IROptCtx *ctx);
int tcc_ir_opt_jump_threading_ex(struct IROptCtx *ctx);
int tcc_ir_opt_eliminate_fallthrough_ex(struct IROptCtx *ctx);
int tcc_ir_opt_dead_loop_elim_ex(struct IROptCtx *ctx);
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
int tcc_ir_opt_store_redundant_ex(struct IROptCtx *ctx);
int tcc_ir_opt_rmw_byte_clear(struct TCCIRState *ir);
int tcc_ir_opt_byte_store_merge(struct TCCIRState *ir);
int tcc_ir_opt_byte_store_merge_ex(struct IROptCtx *ctx);
int tcc_ir_opt_local_copy_prop(struct TCCIRState *ir);
int tcc_ir_opt_local_copy_prop_ex(struct IROptCtx *ctx);
int tcc_ir_opt_addrof_var_fwd_ex(struct IROptCtx *ctx);
int tcc_ir_opt_global_sl_fwd_ex(struct IROptCtx *ctx);
int tcc_ir_opt_loop_const_sim(struct TCCIRState *ir);
int tcc_ir_opt_loop_const_sim_ex(struct IROptCtx *ctx);

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

/* First-iteration-exit loop peeling.  When a top-tested loop's exit test is
 * provably true on entry from the preheader, rewrite its conditional JUMPIF
 * into an unconditional JUMP to the exit target; subsequent DCE removes the
 * unreachable body.  Returns number of loops eliminated. */
int tcc_ir_opt_loop_dead_first_iter(struct TCCIRState *ir);

/* Pointer-IV exit-value substitution - for loops with a constant trip count,
 * replace post-loop uses of pointer induction variables with their closed-form
 * exit value `Addr[StackLoc[init_off + step * trip_count]]`.  Pairs with
 * cmp_stack_addr_fold to collapse post-loop `if (p != &a[N])` checks. */
int tcc_ir_opt_loop_ptr_iv_exit_subst(struct TCCIRState *ir);

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

#endif /* TCC_IR_OPT_H */

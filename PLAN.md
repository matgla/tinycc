# SSA Optimization Plan: Fold Inlined Check Functions to 7 Instructions

## Goal

Reduce `main` in `test_llong_load_signed.c` from 100 instructions to 7 (matching GCC -O2).
GCC's output is: `push; ldr; bl puts; ldr; bl puts; movs r0,#0; pop`.

All 3 inlined `check_s64` comparisons must be proven always-equal and eliminated.

## Current State

After pre-SSA optimizations and loop rotation, the SSA optimizer receives this IR for `main`:

```
0000: PARAM0[call_1] ...
0001: CALL puts                          ;; puts("Testing...")
0002: V0 <-- #0
0003: T0 <-- GlobalSym(g1)***DEREF***    ;; T0 = g1
0004: T1 <-- GlobalSym(g2)***DEREF***    ;; T1 = g2
0005: StackLoc[-16] <-- T1               ;; arr[1] = g2
0006: StackLoc[-8] <-- #-1099511627776   ;; arr[2] = -(1LL<<40)

;; --- inlined check_s64("arr0", arr[0], g1) ---
0012: V2 <-- "arr0"
0013: T5 <-- T0                          ;; got = T0 (= g1, from arr[0] forwarded by pre-SSA)
0014: V3 <-- T5
0015: T6 <-- GlobalSym(g1)***DEREF***    ;; exp = reload g1
0016: V4 <-- T6
0017: CMP T5, T6                         ;; ← SHOULD FOLD: both are g1
0018: JMP == skip1
    ... printf FAIL path + RETURNVALUE #1 ...
skip1:

;; --- inlined check_s64("arr1", arr[1], g2) ---
0030: T12 <-- T11***DEREF***             ;; got = *Addr[StackLoc[-16]] = arr[1]
0034: T14 <-- GlobalSym(g2)***DEREF***   ;; exp = g2
0036: CMP T13, T14                       ;; ← SHOULD FOLD: T12 loaded from StackLoc[-16] which holds T1 = g2
    ... printf FAIL path + RETURNVALUE #1 ...

;; --- inlined check_s64("local", local, -(1LL<<40)) ---
0046: T18 <-- &V0
0047: V9 <-- T18
0050: T20 <-- V9                         ;; T20 = &V0
0051: T21 <-- V10                        ;; T21 = -(1LL<<40) (from StackLoc[-8])
0052: T20***DEREF*** <-- T21             ;; *&V0 = -(1LL<<40), i.e. V0 = -(1LL<<40)
0054: T22 <-- V0 [LOAD]                  ;; T22 = V0 = -(1LL<<40)
0056: CMP T22, #-1099511627776           ;; ← SHOULD FOLD: both are -(1LL<<40)
    ... printf FAIL path + RETURNVALUE #1 ...

0066: PARAM0 ...
0067: CALL puts                          ;; puts("PASS")
0068: RETURNVALUE #0
```

After SSA optimization, **nothing folds** — all 3 CMPs and their dead error paths survive.

---

## Three Comparisons, Three Root Causes

### CMP 1: `CMP T5, T6` — Global Load CSE not firing

**What happens:** T0 and T6 both load `GlobalSym(g1)***DEREF***`. No store to g1 between them.

**Root cause:** `ssa_opt_load_cse` correctly tracks global loads, but there is an intervening `CALL puts` at instruction 1 which **invalidates all tracked loads** (line 86-88 of `ssa_opt_load_cse.c`). The T0 load at instruction 3 is registered, then `CALL puts` at instruction 1... wait, the CALL is before T0.

Actually, re-reading the IR: the CALL at line 1 is before T0 at line 3. So T0 is registered after the call. T6 is at line 15. Between lines 3 and 15, there are no CALLs or aliasing stores. **Load CSE should fire.**

**Actual root cause:** The load CSE pass handles this correctly in theory. But between T0 (line 3) and T6 (line 15), there is a **basic block boundary** (the JUMPIF at line 18 creates a branch). T0 is in the entry block; T6 is in a dominated block after the first check_s64 branch. Since `gload_process_block` passes `state` by value to domtree children, the T0 entry should be visible in the block containing T6.

**But wait:** between T0 and T6, there's a `CALL GlobalSym(printf)` at line 23 (the error path) which invalidates the load state. However, that CALL is in a **different basic block** (the error arm). Since load_cse walks the dominator tree, the error block is a child that gets its own copy of state. The continuation block (post-CMP) should still see T0.

**Investigation needed:** Check if the CFG/dominator tree structure puts T6 in a block dominated by T0's block, and that the error-arm invalidation doesn't leak into the continuation. **Likely a dominator-tree issue or block-boundary issue.**

### CMP 2: `CMP T13, T14` — Stack store-load forwarding through pointer

**What happens:**
- `StackLoc[-16] <-- T1` stores g2 to arr[1]
- Later: `T10 <-- Addr[StackLoc[-16]]; V5 <-- T10; T11 <-- V5; T12 <-- T11***DEREF***` loads arr[1] through a pointer chain
- T14 loads `GlobalSym(g2)` again

**Root cause:** The load of arr[1] goes through a VAR-indirected pointer (`T11 = V5 = Addr[StackLoc[-16]]`), not a direct `StackLoc[-16]` load. The pre-SSA SL-forward and the SSA optimizer don't resolve the pointer chain to recognize this is a stack load.

**Fix:** After SSA cprop resolves `T11 → V5 → T10 → Addr[StackLoc[-16]]`, the load `T12 <-- T11***DEREF***` becomes `T12 <-- *Addr[StackLoc[-16]]` = load from StackLoc[-16]. Then:
1. Stack store-load forwarding: StackLoc[-16] holds T1 → T12 = T1
2. Load CSE: T14 = GlobalSym(g2) = T1 (since T1 was loaded from g2)
3. CMP T12, T14 → CMP T1, T1 → fold

**This requires:** SSA cprop to propagate through VARs into pointer dereferences, and then a store-load forwarding pass for stack slots.

### CMP 3: `CMP T22, #-1099511627776` — SCCP through store-via-pointer

**What happens:**
- `V0 = 0` initially
- `T20 = &V0; *T20 = -(1LL<<40)` stores through pointer
- `T22 = V0` loads the value

**Root cause:** SCCP's `sccp_resolve_var` scans backward for store-through-pointer patterns. It finds `T20***DEREF*** <-- T21 [STORE]` and tries to trace T20 back to `&V0`. But T20 is defined as `T20 = V9`, and V9 is a VAR (not `&V0` directly). The backward scan doesn't follow through VAR indirection.

**Fix:** Either:
- (a) Run cprop before SCCP so T20 is simplified to `T20 = &V0` directly, or
- (b) Teach `sccp_resolve_var`'s backward pointer scan to follow through ASSIGN chains and VAR stores

---

## Implementation Plan

### Step 1: Fix pass ordering — run cprop before SCCP

In `tcc_ir_ssa_opt_run` ([ssa_opt.c:405](ir/opt/ssa_opt.c#L405)):

```c
// Current:
changes += ssa_opt_sccp(ctx);
changes += ssa_opt_cprop(ctx);

// Change to:
changes += ssa_opt_cprop(ctx);
changes += ssa_opt_sccp(ctx);
```

**Why:** cprop resolves copy chains like `T20 = V9 = T18 = &V0` into direct `T20 = &V0`. SCCP's backward pointer scan then finds the `&V0` pattern directly.

This alone should fix **CMP 3** (the `-(1LL<<40)` constant case).

### Step 2: Extend SCCP to resolve store-through-pointer with LOAD sources

In `sccp_resolve_var` ([ssa_opt_sccp.c:138](ir/opt/ssa_opt_sccp.c#L138)), when handling `STORE *T = src`:

Currently, only constant-immediate sources are handled (`if (irop_is_immediate(src))`). Extend to resolve `src` through the SCCP lattice:

```c
/* After finding *T = src where T = &V: */
IROperand src = tcc_ir_op_get_src1(ir, q);
if (irop_is_immediate(src)) {
    *out = irop_get_imm64_ex(ir, src);
    return SCCP_CONST;
}
/* NEW: check if src TEMP has a known constant in the lattice */
int32_t src_vr = irop_get_vreg(src);
SCCPCell *src_cell = sccp_cell(s, src_vr);
if (src_cell && src_cell->state == SCCP_CONST) {
    *out = src_cell->value;
    return SCCP_CONST;
}
return SCCP_BOTTOM;
```

**Why:** The stored value may come from a LOAD of a constant stack slot (e.g., `T21 = V10 [LOAD]` where V10 was assigned from `StackLoc[-8]` which holds `-(1LL<<40)`). After cprop + earlier SCCP iterations, T21 may be known-constant in the lattice.

### Step 3: Add CMP-of-same-vreg folding to ssa_opt_branch

In `ssa_fold_cmp_jumpif` ([ssa_opt_branch.c:44](ir/opt/ssa_opt_branch.c#L44)):

Already implemented at lines 66-77 — checks `vr1 == vr2`. This handles the case where load_cse converts the second load to an ASSIGN from the first, and cprop propagates it. **No change needed here.**

### Step 4: Debug/fix load_cse dominator-tree traversal

The `ssa_opt_load_cse` pass should already deduplicate the two `GlobalSym(g1)` loads (T0 at line 3, T6 at line 15). Verify that:

1. The CFG correctly places T0 and T6 in blocks where T0's block dominates T6's block
2. No CALL or aliasing STORE between T0 and T6 invalidates the entry
3. The `GLoadState` passed by value to child blocks preserves T0's entry

If load_cse fires correctly:
- T6 becomes `ASSIGN T0`
- cprop propagates: CMP T5, T6 → CMP T0, T0
- branch fold: CMP identical vregs → always equal → JMP/NOP
- DCE removes the dead error path

**Test:** Add `fprintf(stderr, ...)` in `gload_process_block` to trace tracked entries and invalidations per block.

### Step 5: Add SSA stack store-load forwarding (for CMP 2)

Create a new pass or extend `ssa_opt_load_cse` to handle stack-slot forwarding:

**Pattern:**
```
StackLoc[N] <-- Tx [STORE]
...
Ty <-- *Addr[StackLoc[N]]    ;; after cprop resolved pointer chain
```

**Transform:** Replace `Ty` with `Tx` (the stored value).

**Implementation in `ssa_opt_load_cse`:** Track `(StackLoc offset → result_vreg)` alongside global loads. On a LOAD where src1 is a TEMP that was assigned `Addr[StackLoc[N]]`, look up whether StackLoc[N] was previously stored. If so, replace the LOAD with ASSIGN from the stored vreg.

**Invalidation:** Any STORE to the same StackLoc or any CALL or any aliasing store (to a non-local address) invalidates the entry. Stack-local stores to *different* offsets are safe.

### Step 6: Cascading cleanup (already implemented)

After Steps 1-5 make the CMPs foldable, the existing passes cascade:

1. **load_cse** → T6 = T0 (dedup global loads)
2. **cprop** → propagate copies
3. **branch** → CMP x,x → fold to always-equal → JUMP/NOP
4. **dce** → remove dead printf paths + RETURNVALUE #1
5. **dce** → remove dead VAR stores (V2, V3, V4, etc.)

Result: `main` = `puts + puts + RETURNVALUE #0` = 7 instructions.

---

## Execution Order

| # | Task | File(s) | Risk | Impact |
|---|------|---------|------|--------|
| 1 | Reorder cprop before sccp | `ssa_opt.c` | Low | Fixes CMP 3 |
| 2 | Extend SCCP lattice lookup for store sources | `ssa_opt_sccp.c` | Low | Strengthens CMP 3 |
| 3 | Debug/fix load_cse for CMP 1 | `ssa_opt_load_cse.c` | Medium | Fixes CMP 1 |
| 4 | Add stack store-load forwarding | `ssa_opt_load_cse.c` | Medium | Fixes CMP 2 |
| 5 | Run full test suite | — | — | Verify no regressions |

## Verification

```bash
./scripts/compare_disasm.py tests/ir_tests/test_llong_load_signed.c
# Expected: main = 7 instructions, Ratio = 1.00x

make test -j16        # IR test suite
make test-asm -j16    # Assembly tests
```

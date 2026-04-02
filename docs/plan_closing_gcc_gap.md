# Plan: Closing the TCC–GCC Code Size Gap

## Current State

Benchmark of TCC -O2 vs GCC -O2 across IR test suite (ARM Thumb-2, Cortex-M33):

| Test / Function               | TCC | GCC | Ratio  | Root Cause               |
|-------------------------------|-----|-----|--------|--------------------------|
| test_llong_load_unsigned/main | 102 |   8 | 12.75x | Inlining + const fold    |
| test_u64_shift_add/main       | 117 |  26 |  4.50x | Inlining + const fold    |
| test_fp_offset_cache/mixed    |  15 |   5 |  3.00x | Const fold + DCE         |
| test_return64/main            |  38 |  14 |  2.71x | Inlining + const fold    |
| test_dcmp/main                |  21 |   8 |  2.62x | Inlining + const fold    |
| test_fp_offset_cache/loop     |  61 |  27 |  2.26x | Loop opts + addr reuse   |
| test_double_arith/main        |  49 |  22 |  2.23x | Inlining + const fold    |
| test_fp_offset_cache/swap     |  52 |  27 |  1.93x | Loop opts + cond exec    |
| bubble_sort                   |  44 |  27 |  1.63x | Addr modes + cond exec   |
| test_f2d_bits/main            |  48 |  30 |  1.60x | Inlining                 |

TCC already matches or beats GCC on leaf functions: test_simple_return (1.00x),
test_llong_mul_unsigned (0.88x), test_semihosting (0.60x), test_aeabi_dneg (0.65x).

### What GCC does for 12.75x case

`test_llong_load_unsigned` defines `load_through_ptr`, `store_through_ptr`, `check_u64`
(all static, <20 lines) and calls them from `main` with known global/constant args.

GCC: inlines everything → propagates `load_through_ptr(&g1) == g1` → folds
`check_u64("g1", g1, g1)` to return 0 → eliminates all dead branches → only
two `puts` calls and `return 0` remain (8 instructions).

### What TCC does today

Token-stream auto-inlining IS working: `load_through_ptr` (len=13) and `check_u64`
(len=54) are registered as inline candidates and replayed at call sites.

Constant evaluation also works for calls with all-VT_CONST args:
- `load_through_ptr(&g1)` → evaluated, folded ✓ (first two calls)
- `load_through_ptr(&arr[0])` → FAILS: stack address not VT_CONST ✗
- `check_u64("g1", <reg>, g1)` → FAILS: inlined result in register, not VT_CONST ✗

`store_through_ptr` is not appearing in inline candidate list (cause TBD — likely
the void return + VT_LLONG param combination).

After token-replay inlining, the full check_u64 body (including the printf error
path) stays in the IR. The IR optimizer cannot prove the comparison always succeeds
because it lacks store-load forwarding through memory: `arr[0] = g1; *(&arr[0])`
does not resolve to `g1` at the IR level.

---

## Step 1: Improve Post-Inline Constant Propagation

**Goal:** After token-replay inlining of `check_u64`, fold `got != exp` to false
when both operands trace back to the same value.

**What to do:**
1. In `ir/opt.c`, extend `tcc_ir_opt_const_prop` to handle the pattern:
   `STORE val → addr` followed by `LOAD addr → tmp` → replace tmp with val.
   This is store-load forwarding for the *same* basic block (intra-BB).
2. Extend the existing `tcc_ir_opt_sl_forward` to handle 64-bit (LLONG) values
   stored/loaded via `strd`/`ldrd` patterns.
3. After forwarding, existing branch folding + DCE eliminates the dead printf path.

**Test:** `test_llong_load_unsigned` — first two `check_u64` calls (with global
addresses) should be fully eliminated from the IR.

**Expected improvement:** 12.75x → ~4x (eliminates 2 of 5 check blocks).

**Files:** `ir/opt.c` (store-load forwarding), `tccir.h` (if new flags needed)

---

## Step 2: Propagate Constants Through Local Arrays

**Goal:** After `arr[0] = g1`, resolve `load_through_ptr(&arr[0])` to `g1`.

**What to do:**
1. Track stores to local array elements with constant indices in a shadow map
   during constant propagation: `stack_offset + idx*size → stored_value`.
2. When a LOAD from a known stack address matches a previous STORE to the same
   address (no intervening aliasing store), forward the value.
3. Handle the specific pattern: `LEA(stack, offset)` passed as arg to inlined
   `load_through_ptr` which does `LOAD(arg)` — after inlining, this becomes
   `LOAD(LEA(stack, offset))` which can resolve via the shadow map.

**Test:** `test_llong_load_unsigned` — all `check_u64` calls with arr elements
should be eliminated.

**Expected improvement:** 12.75x → ~2x (eliminates arr-based checks, only
`store_through_ptr` + final check remain).

**Files:** `ir/opt.c`

---

## Step 3: Fix store_through_ptr Not Being Inlined

**Goal:** Ensure void functions with VT_LLONG parameters are auto-inlined.

**What to do:**
1. Add INLINE_STRUCT logging around `auto_inline_sig_ok` rejection path to
   identify exactly why `store_through_ptr` is being skipped.
2. Fix the rejection (likely in `auto_inline_sig_ok` parameter loop or the
   void+LLONG combination).
3. After inlining `store_through_ptr(&local, arr[2])`, Step 2's forwarding can
   propagate `local == 0xffffffffffffffff` to the final `check_u64`.

**Test:** `test_llong_load_unsigned` — final code should match GCC: two `puts`
calls + `return 0`.

**Expected improvement:** 12.75x → ~1.0x for this specific test.

**Files:** `tccgen.c` (auto_inline_sig_ok, call-site inline logic)

---

## Step 4: Fix LICM Instruction Index Bug

**Goal:** Re-enable loop-invariant code motion.

**Current state:** LICM is disabled at `tccgen.c:25176`. The old pattern-based
`hoist_from_loop` returns 0 unconditionally (`licm.c:590`). A new dominance-based
`tcc_ir_opt_licm_ex` exists but the old pass is dead. The bug is documented:
> instruction indices are not adjusted by total_inserted when reading original
> instructions during the insertion loop, causing operand_base corruption

**What to do:**
1. The dominance-based LICM (`tcc_ir_opt_licm_ex`) is already implemented with
   CFG + dominator tree. Verify it handles instruction index adjustment correctly.
2. Remove the `return 0` guard in `hoist_from_loop` OR remove the old pass
   entirely and rely on the dominance-based version.
3. Enable LICM by removing the comment/guard at `tccgen.c:25176` (set
   `opt_licm=1` at `-O1`+).
4. Run full test suite to validate: `make test -j16 && make test-gcc-torture-compile`.

**Test:** `test_fp_offset_cache/test_loop_access` (2.26x), bubble_sort (1.63x).

**Expected improvement:** ~15-25% reduction in loop-heavy functions.

**Files:** `ir/licm.c`, `tccgen.c` (optimization pipeline)

---

## Step 5: Copy Coalescing in Register Allocator

**Goal:** Eliminate redundant `mov` instructions from ASSIGN IR ops.

**Current state:** The linear scan allocator in `tccls.c` assigns physical registers
independently. The optimized IR contains many identity assigns like:
```
R0(T1) <-- R5(V0) [ASSIGN]    →  mov r0, r5
R1(T9) <-- R4(V0) [ASSIGN]    →  mov r1, r4
```

**What to do:**
1. After liveness analysis (`ir/live.c`), add a coalescing pre-pass that merges
   virtual register live ranges connected by ASSIGN when they don't interfere.
2. Specifically: for `Tx <-- Vy [ASSIGN]`, if Tx and Vy have non-overlapping live
   ranges (or Vy dies at this instruction), assign the same physical register.
3. After coalescing, the ASSIGN becomes a no-op and can be eliminated by DCE.

Alternative lighter approach: add a post-regalloc peephole in `arm-thumb-gen.c`
that eliminates `mov Rx, Rx` (same register).

**Test:** Every function — count `mov` instructions before/after.

**Expected improvement:** ~15-20% across the board. In bubble_sort: 44 → ~35.

**Files:** `tccls.c` (register allocator), `ir/live.c` (liveness)

---

## Step 6: If-Conversion for Small Conditional Blocks (IT Blocks)

**Goal:** Replace short branch-over patterns with ARM IT conditional execution.

**Current state:** TCC generates full branch diamonds even for single-instruction
if-then bodies. GCC uses IT blocks:
```
; GCC bubble sort swap:
cmp   r2, r1
it    gt
strdgt r1, r2, [r3, #-4]     ; 1 conditional instruction, no branch

; TCC bubble sort swap:
cmp   r1, r2
ble   .skip
; ... 10 instructions for swap ...
.skip:
```

**What to do:**
1. Add an IR-level if-conversion pass that detects diamond/triangle patterns where
   the "then" block has 1-4 instructions and no side effects beyond stores.
2. Convert to `SELECT` IR ops (already defined in `tccir.h`) or emit IT blocks
   directly in `arm-thumb-gen.c`.
3. ARM Thumb-2 IT blocks support up to 4 conditional instructions. Focus on the
   common pattern: compare + conditional store (swap, min/max).

**Test:** bubble_sort, test_swap_pattern, any conditional move patterns.

**Expected improvement:** ~10-15% in branch-heavy inner loops. Bubble sort: 35 → ~28.

**Files:** `ir/opt.c` (new pass), `arm-thumb-gen.c` (IT block emission)

---

## Step 7: Improved Induction Variable Strength Reduction

**Goal:** Convert `base + i*4` recomputed each iteration into pointer increment.

**Current state:** IV strength reduction exists (`tcc_ir_opt_iv_strength_reduction`)
but doesn't catch all patterns, especially when the same array index is used
multiple times in a loop body (like swap: `arr[j]`, `arr[j+1]` used in load, store,
and recomputed independently).

**What to do:**
1. Extend IV SR to identify groups of array accesses sharing the same base and
   induction variable: `arr[j]`, `arr[j+1]` → single pointer `p` with `p[0]`,
   `p[1]`, incremented once per iteration.
2. After the pointer is introduced, existing indexed load fusion
   (`LOAD_INDEXED`) handles the rest.
3. Requires LICM (Step 4) to hoist the base address first.

**Test:** bubble_sort, test_loop_access, test_swap_pattern.

**Expected improvement:** ~10% additional on loop-heavy code.

**Files:** `ir/opt.c` (IV strength reduction)

---

## Execution Order & Dependencies

```
Step 1  ──→ Step 2 ──→ Step 3     (inlining + const prop chain)
   │
   │        Step 4 ──→ Step 7     (LICM enables better IV SR)
   │
   │        Step 5                 (independent: regalloc)
   │
   │        Step 6                 (independent: if-conversion)
   ↓
  Steps 4-7 can run in parallel with Steps 1-3
```

Steps 1-3 are the highest leverage: they address the 12.75x/4.50x/2.71x outliers.
Steps 4-7 improve the 1.5x-2.3x cases (loops, branches, register pressure).

## Validation

After each step, run:
```bash
make test -j16                              # IR tests pass
make test-gcc-torture-compile               # no regressions
python3 scripts/compare_disasm.py tests/ir_tests/test_llong_load_unsigned.c  # track ratio
python3 scripts/compare_disasm.py bubble    # track ratio
```

## Target

| Test                          | Current | After Steps 1-3 | After All |
|-------------------------------|---------|------------------|-----------|
| test_llong_load_unsigned/main | 12.75x  | ~1.0x            | ~1.0x     |
| test_u64_shift_add/main       |  4.50x  | ~2.0x            | ~1.5x     |
| test_return64/main            |  2.71x  | ~1.2x            | ~1.0x     |
| test_fp_offset_cache/loop     |  2.26x  | ~2.26x           | ~1.3x     |
| bubble_sort                   |  1.63x  | ~1.63x           | ~1.1x     |

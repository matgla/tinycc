# Handoff: Re-enabling loop unroll + rotation (Finding #15 follow-up)

**Branch:** `heapOverflowBug`  ·  baseline commit `61233c33` ("checkpoint before hard-float VFP work")
**Date:** 2026-06-27
**Status:** ✅ COMPLETE. All known bugs fixed & verified; both passes enabled; all
temporary scaffolding removed; full validation green (see "Final validation"
below). Seeds 49 and 244 fixed, plus one extra pre-existing loop-elim wrong-code
bug (`count()`) surfaced by the codegen_asm suite.

---

## Final resolution (the landed fixes)

1. **Seed 244** (scratch clobbers a loop-carried reg) — `ir/regalloc.c`,
   new `ra_refine_live_regs_accurate()`.  The interval-derived
   `live_regs_by_instruction` bitmap models each value as one contiguous
   `[def,last-use]` range, leaving a loop-carried value's loop-header prefix
   uncovered → the scratch picker reused its register inside the loop.  Fix: a
   real CFG backward-liveness dataflow does **loop-liveness completion** — for
   each back-edge, every register live-in at the loop header is OR'd into the
   bitmap across the whole loop body `[header, back-edge]`.  Scoped to loop
   bodies on purpose (a blanket per-instruction live-out refinement over-marked
   straight-line liveness and perturbed unrelated functions — seed 221).
2. **Seed 49** (doubly-rotated nested loops miscompiled) — `ir/opt_loop_utils.c`,
   `try_rotate_loop`.  Rotating BOTH an outer loop and an inner loop nested in it
   produces a shape a later pass miscompiles (rotating EITHER alone is correct).
   Fix: decline to rotate a loop nested inside an ALREADY-ROTATED loop (detected
   via a backward-branching JUMPIF that strictly encloses `[hi, backedge]`).
3. **`count()` zero-trip wrong-code** (pre-existing, surfaced by
   `test_control_branch_conditional_and_loop`) — `ir/opt_loop_utils.c`,
   `try_eliminate_loop_symbolic`.  Its fallback wrote unconditional closed forms
   (`i = limit`) for a SYMBOLIC limit, ignoring the zero-trip case of a
   top-tested `while` (`i=0; while(i<n) i++; return i` is `max(n,0)`, not `n`).
   Fix: bail (leave the loop intact) for every non-SELECT-path case — only the
   guarded SELECT path handles the zero-trip case.

## Final validation (all green)

- Fuzz olevels `0-299`: failing set ⊂ baseline; **50/183/211 now fixed**, zero new.
- Fuzz vs-gcc `0-199`: only the pre-existing O0-vs-O1 seeds, zero new.
- `test_qemu.py`: 1496 passed / 0 failed.  `test_codegen_asm.py`: 14 passed.
- gcc-torture O1/O2: 7521 passed / 0 failed.  Unit: 1090 / 0.
- ASAN build clean (no overflow/UAF) on the seed paths; `make cross fp-libs` builds.

---

### Historical notes (superseded by the resolution above)

---

## Goal (unchanged from the task brief)

`ir/opt_loop.c` disabled two passes with an early `(void)ir; return 0;`:
- `tcc_ir_opt_loop_unroll`   (~line 563)
- `tcc_ir_opt_loop_rotation` (~line 741)

Re-enable BOTH and fix the underlying codegen bug(s) so the differential fuzzer is
clean with ZERO regressions, then run the full validation suite (see "Validation"
at the bottom).

---

## TL;DR of where we are

| Seed | Pass that exposes it | Bug class | Status |
|------|---------------------|-----------|--------|
| 23, 37 | rotation | scratch picker hands out a loop-carried reg the **bitmap** omitted | **FIXED** (union fix in `tccls.c`) |
| 18 | unroll (`try_unroll_loop_ex`) | `collect_body_instructions` **silently truncated** an over-cap body | **FIXED** (`opt_loop_utils.c`) |
| 50, 183, 211 | (pre-existing, passes OFF) | same scratch-liveness class as 23/37 | **FIXED as a bonus** by the union fix |
| **244** | **rotation** | scratch picker clobbers a loop-carried reg that **even the interval scan misses** (live across the back-edge; no covering interval) | **OPEN** — needs `loop_phi_locked` protection in the scratch liveness |
| **49** | **rotation** | NOT a scratch bug (paranoid-scratch doesn't fix it) — rotation transform or another pass (SCCP/DCE/etc.) producing wrong IR for a **nested** rotated loop | **OPEN** — root cause not yet isolated |

The two OPEN seeds are NEW regressions: they pass at baseline (passes OFF) and fail
only once rotation is re-enabled. They MUST be resolved (or rotation made
conservative for their shape) to meet "zero regressions".

---

## The real code changes (KEEP these — they are the fixes)

Only 3 source files have real changes. `arm-thumb-gen.c` is net-zero (a candidate
"fix B" was tried and fully reverted — the union fix subsumed it).

### 1. `tccls.c` — `tcc_ls_find_free_scratch_reg` — the liveness UNION fix
The scratch picker trusted `ls->live_regs_by_instruction[idx]` (built by
`ra_build_live_regs_bitmap`, which **skips intervals with `stack_location != 0`**).
A loop-carried value kept in a register *and* assigned a spill slot
(`r0 >= 0 && stack_location != 0`) is dropped from the bitmap → the picker thinks
its register is free → clobbers it. `tcc_ls_compute_live_regs` (interval scan) does
report it. Fix = **union the bitmap with the interval scan** (strictly conservative;
can only add live bits, never remove → cannot introduce a new clobber).

This single fix cleared seeds 23, 37 AND pre-existing 50, 183, 211.

### 2. `ir/opt_loop_utils.c` — `collect_body_instructions` — the truncation fix
The scan loop was `for (i=start; i<=end && count<max_body; i++)`. When a loop body
has > `UNROLL_MAX_BODY_INSNS` (=32) real instructions, it returned exactly 32 (a
truncated prefix) and the caller's `body_count > UNROLL_MAX_BODY_INSNS` guard
(`32 > 32` = false) never fired → `try_unroll_loop_ex` unrolled a **truncated body**,
dropping everything past the cap (including an inner loop's control flow that lives
in the tail). Seed 18's 203-insn body had a straight-line first-32 prefix that
slipped past the JUMPIF/call rejection. Fix = scan the FULL range and **reject**
(`return -1`) when `count >= max_body` (rather than truncate). Strictly conservative:
only declines to unroll over-cap bodies, which were always miscompiled anyway.

### 3. `ir/opt_loop.c` — re-enabled both passes
Deleted the `(void)ir; return 0;` + stale Finding #15 comment blocks in
`tcc_ir_opt_loop_unroll` and `tcc_ir_opt_loop_rotation`.

---

## TEMPORARY scaffolding currently in the tree — REMOVE before finalizing

These were added for bisection/classification and are env-gated (default OFF, so they
don't affect normal builds), but they must be deleted for the final diff:

- `tccls.c:273` — `if (getenv("TCC_PARANOID_SCRATCH")) return PREG_NONE;`
  (forces push/pop for every scratch — used to prove 244 is a scratch bug and 49 is not)
- `ir/opt_loop.c:565` — `if (getenv("TCC_NO_UNROLL")) return 0;`
- `ir/opt_loop.c:738` — `if (getenv("TCC_NO_ROTATE")) return 0;`
- `ir/opt_loop.c:682-686` — the `!getenv("TCC_NO_ELIM"/"TCC_NO_SYM"/"TCC_NO_UEX")` guards
  on the `try_eliminate_loop / try_eliminate_loop_symbolic / try_unroll_loop_ex` chain
  (restore to the plain `if/else if/else`)

A previous temp diagnostic block (`TCC_DIAG_SCRATCH`) in `tccls.c` was already removed.
The pre-existing `test_init_struct_from_struct` debug `fprintf` in `tccls.c` (~line 307)
is NOT mine — leave it.

---

## Build / environment state at handoff

- **`config.mak` is currently `--disable-asan`.** The ASAN config was saved to
  `/tmp/config.mak.asan`. Restore with `cp /tmp/config.mak.asan config.mak` (or re-run
  `./configure`, which defaults to ASAN-on) when you want the ASAN build back.
- Rationale for `--disable-asan` during fuzzing: the ASAN build has **pre-existing
  frontend leaks** (e.g. `vstore`←`expr_eq`←`block_1`←`gen_function`, the
  `decl_initializer_alloc` family documented in memory note
  `yasos-tcc-ir-suite-asan-leak-blocks-validation`) that make the fuzz harness count
  *compile failures* as divergences. Validate **codegen** on `--disable-asan`; use the
  ASAN build separately to confirm no NEW compiler memory bugs (my 3-file change adds no
  allocations, so it cannot introduce leaks — `collect_body`'s `return -1` routes through
  `unroll_cleanup` which frees `_ubuf`).
- The currently-built `armv8m-tcc` is a `--disable-asan` release build WITH the temp env
  gates compiled in (they're inert unless the env vars are set).

---

## How to reproduce / classify (all verified working)

```bash
cd libs/tinycc
# generate a seed
python3 tests/fuzz/gen_c.py --seed 49 -o /tmp/seed49.c

# reliable serial differential for specific seeds (O0/O1/O2 under QEMU):
python3 scripts/diff_olevels.py --seed 49 --seed 244 --require-qemu

# broad sweep (parallel); strip ANSI; ~4s for 300 seeds on 32 cores (tiny semihosting progs):
FUZZ_OLEVEL_SEEDS=0-299 python3 -m pytest tests/fuzz/test_random_c_olevels.py -n 16 -q

# build one seed's ELF, keeping artifacts, and run it:
D=/tmp/d49; mkdir -p $D
make -s -C tests/ir_tests/qemu/mps2-an505 OUTPUT=$D TEST_FILES=/tmp/seed49.c \
     CC=$PWD/armv8m-tcc TARGET=$D/m.elf EXTRA_CFLAGS=-O2
qemu-system-arm -machine mps2-an505 -nographic -semihosting -kernel $D/m.elf

# IR dump needs a debug build:
make clean && make cross CFLAGS+="-DCONFIG_TCC_DEBUG -DTCC_LOG_LOOP_OPT=1"
make -s -C tests/ir_tests/qemu/mps2-an505 OUTPUT=$D TEST_FILES=/tmp/seed49.c \
     CC=$PWD/armv8m-tcc TARGET=$D/m.elf EXTRA_CFLAGS="-O2 -dump-ir"   # IR to stdout, LOOP_OPT log to stderr
# main's "IR AFTER LOOP ROTATION" = correct shape; "IR AFTER OPTIMIZATIONS" = post-regalloc
```

### Decisive classification done so far
- **Bisecting which pass:** `TCC_NO_ROTATE=1` fixes BOTH 49 and 244; `TCC_NO_UNROLL=1`
  fixes neither → both are **rotation**-exposed.
- **Scratch vs not:** `TCC_PARANOID_SCRATCH=1` (force push/pop for every scratch)
  → **fixes 244** (so 244 IS a scratch-liveness clobber) but **does NOT fix 49**
  (so 49 is NOT a scratch bug).

Correct checksums (O0==O1): seed 49 = `0005b6d8`, seed 244 = `ce53d2eb`
(seed 244 at O2 currently HardFaults: "Lockup: can't escalate 3 to HardFault").

---

## Next steps

### Seed 244 (scratch-liveness, loop-carried reg) — the tractable one
The clobbered register holds a **loop-carried** value that is live across the
back-edge into the next iteration, so NO `[start,end]` interval covers the clobber
instruction → neither the bitmap nor the interval scan reports it. The SSA allocator
already flags these intervals with **`loop_phi_locked`** (`ir/regalloc.c:49`,
`SSAInterval`; "carries a loop-carried value across the whole loop body; must not be
evicted"). The scratch picker is unaware of it.

Plan: make `live_regs_by_instruction` (or the scratch picker) treat a
`loop_phi_locked` interval's register as live across the **whole loop body**, not just
`[start,end]`. Where to wire it:
- `ra_build_live_regs_bitmap` (`ir/regalloc.c:3479`) builds the bitmap but iterates
  `ir->ls.intervals` which are **`LSLiveInterval`** (no `loop_phi_locked` field) — the
  flag lives on the SSA-side `SSAInterval`. Find where `SSAInterval`s are lowered into
  `ir->ls.intervals` and either (a) propagate `loop_phi_locked` onto `LSLiveInterval`,
  or (b) in the SSA path, OR each `loop_phi_locked` register into
  `live_regs_by_instruction[k]` for every `k` in the loop body range.
- Note there is already related machinery right above the bitmap build: a "post-phi
  back-edge extend" loop (`ir/regalloc.c:3451-3473`) that extends intervals whose range
  ends before a backward jump whose target they cover. **Check whether 244's
  loop-carried interval is being missed by this extension** — it may be the cleanest fix
  point (the existing `loop_phi_locked` fix family lives here too).
- Validation that you've got it: `TCC_PARANOID_SCRATCH=1` already proves a correct
  liveness fix will fix 244; your real fix should match that result WITHOUT the global
  push/pop cost.

### Seed 49 (NOT scratch — rotation transform or downstream pass)
Nested loops: `for g6<11 { ...; for g8<4 { cs = csmix(cs, ...) } ... }`, both
accumulate into `cs`. BOTH loops get rotated (the post-rotation IR for `main` shows
guard+tail CMP pairs for both `CMP V11,#11` and `CMP V21,#4`). Need to determine
whether the **rotation output IR itself is wrong** (compare "IR AFTER LOOP ROTATION"
semantics — is the loop-carried `cs` phi/merge wired correctly across the nested
back-edges?) or whether a later pass (SCCP / DCE / coalescing — the memory note
`yasos-tcc-disabled-loop-opts-unrotated-shape-bugs` lists dead_loop/SCCP/regalloc as
historically rotation-exposed) miscompiles the rotated shape.
- Suggested approach: dump IR for seed 49 (debug build, recipe above), read main's
  "IR AFTER LOOP ROTATION" (lines ~713-889 in the last dump) and check the inner-loop
  `cs` accumulator threading; then diff against "IR AFTER OPTIMIZATIONS" to see if a
  specific optimization corrupts it. The env gates `TCC_NO_ELIM/SYM/UEX` are present if
  you need to bisect the unroll-pass internals, but 49 is rotation-caused, so also
  consider gating individual post-rotation optimizations.
- If the rotation transform is wrong for nested loops specifically, the safe fix may be
  to have `try_rotate_loop` (`ir/opt_loop_utils.c`) decline the problematic nested shape
  (conservative) rather than emit wrong IR.

### Then: finalize
1. Remove ALL temp env gates (list above).
2. `cp /tmp/config.mak.asan config.mak` (or `./configure`) → ASAN-on default, `make clean && make cross fp-libs`.
3. Re-run the three target seeds (18, 23, 37) + the two new ones (49, 244) → all consistent.
4. Re-run the broad sweep 0-299 on `--disable-asan`; confirm the failing set is a SUBSET
   of the baseline failing set (baseline passes-OFF set was: 50, 51, 52, 89, 100, 118,
   132, 151, 183, 202, 211, 215, 251, 281, 294). I.e. NO new entries beyond baseline,
   and ideally 50/183/211 stay fixed. (Pre-existing 51,52,89,100,118,132,151,202,215,
   251,281,294 are O0-vs-O1 bugs unrelated to loops — out of scope, but do not regress
   them.)

---

## Remaining validation checklist (from the task brief — NOT yet run)

1. Fuzz differential, BOTH oracles: `tests/fuzz/test_random_c_olevels.py` (done, see above)
   **and** `tests/fuzz/test_random_c_vs_gcc.py` (NOT yet run). 0 NEW divergences.
2. gcc-torture O1/O2 (ASAN build): `cd tests/ir_tests && pytest test_gcc_torture_ir.py -k "O1 or O2"`.
   Baseline 7485 passed / 0 failed — must stay 0 failed.
3. QEMU IR suite: `pytest test_qemu.py` (1497) + `test_codegen_asm.py`.
4. Unit tests: `cd tests/unit && make clean && make run`.  ✅ **DONE: 1090 tests, 0 failed.**
5. `make cross fp-libs` builds.  ✅ done (fp-libs build as part of `make cross`).

## On landing (from the task brief)
- Delete the `(void)ir; return 0;` in both functions (✅ done) and stale Finding #15 comments (✅ done).
- Flip `test_forward_branch_conditional_still_wide` in `tests/ir_tests/test_codegen_asm.py`:
  with rotation back ON the back-edge becomes a tight `blt.n` again → assert `blt.n >= 1`
  (currently expects the un-rotated `b.n`). **NOT yet done.**
- Update memory `yasos-tcc-disabled-loop-opts-unrotated-shape-bugs` (mark backend fix
  landed) and PASS_COVERAGE Finding #15. **NOT yet done.**

## Key files
- `tccls.c` — `tcc_ls_find_free_scratch_reg` (union fix + temp paranoid gate),
  `tcc_ls_compute_live_regs`
- `ir/regalloc.c` — `ra_build_live_regs_bitmap` (~3479), `SSAInterval.loop_phi_locked`
  (49), post-phi back-edge extend (~3451), the existing `loop_phi_locked` eviction guard (~2487)
- `ir/opt_loop.c` — pass entry points (re-enabled) + temp bisection gates
- `ir/opt_loop_utils.c` — `collect_body_instructions` (truncation fix),
  `try_unroll_loop_ex` (~2871), `try_rotate_loop`
- `tests/fuzz/` — `gen_c.py`, `test_random_c_*.py`; `scripts/diff_olevels.py`,
  `scripts/diff_vs_gcc.py`

# Plan — real bug hunting for the tinycc optimizer (before legacy↔SSA)

Goal: actively **find latent miscompiles now**, before the legacy→SSA optimizer merge —
not just lock in current behavior. Slots *before* the legacy↔SSA equivalence work
(Phase F in `plan_optimizer_test_coverage.md`); in fact Track 4 below becomes the
substrate that makes Phase F nearly free.

## The core principle
The per-pass suites we wrote are **characterization tests**: expectations were derived
from the implementation, so they agree with the code by construction and rarely *find*
bugs. Bug *hunting* requires an **independent oracle** — an expected result computed
without reference to the code under test. Four oracle families below, ordered by ROI.

Grounding (verified available on this machine): `arm-none-eabi-gcc` ✓, `qemu-system-arm` ✓,
ASAN on by default (`config.mak`) ✓, `csmith` ✗ (not installed — random C must be homegrown
or csmith added). No existing host IR interpreter or real IR verifier (`check_*.c` are
throwaway debug printf programs) — Track 4 builds one.

**Rules (unchanged):** test/tooling only, **no production edits**. Every confirmed miscompile
→ *Findings* in `PASS_COVERAGE.md` with a minimal repro; hand off to bug-fix work. Differential
failures MUST be triaged for undefined behavior / known-skips before being called bugs.

---

## Track 1 — ASAN + UBSan corpus sweep  (memory-safety class) — LOW effort, PROVEN, do first
- Oracle: the sanitizer. Any ASAN/UBSan report during compilation is a bug, full stop.
- Build the x86 cross with ASAN (default) **and** add `-fsanitize=undefined`; compile the whole
  corpus (gcc-torture compile + execute, tests2, ir_tests inputs) at `-O0/-O1/-O2`; collect reports.
- Proven technique here — previously found 3 heap overflows (memory `yasos-tcc-asan-sweep-fixes`).
- Deliverable: `scripts/asan_sweep.sh` (corpus × O-levels → grep sanitizer output → dedup by stack).
- Effort ~1 day. Localizes to file:line via backtrace (not always to a pass, but high ROI).
- Parallelizable: shard the corpus across agents.

## Track 2 — Optimization-level self-consistency differential  (miscompile class) — LOW–MED effort, no external oracle
- Oracle: **a program's observable output must be identical at -O0/-O1/-O2.** Divergence ⇒ an
  optimization changed behavior ⇒ miscompile.
- Harness: compile each program with tcc at O0/O1/O2 → run under QEMU (reuse ir_tests/mps2-an505
  infra) → diff stdout + exit code.
- Expand coverage with a homegrown **random C generator** (csmith absent): UB-free expressions over
  int/uint/char/short/long — arithmetic, bitwise, shifts, comparisons, `if`/`while`/`for`, small
  arrays/structs, calls. Print a checksum of computed values so output is sensitive to miscompiles.
- Strength: catches pass + regalloc + codegen *interaction*; pins the offending O-level; no gcc needed.
- Deliverable: `scripts/diff_olevels.py` + `tests/fuzz/gen_c.py`. Effort ~2–3 days.

## Track 3 — Differential vs arm-none-eabi-gcc  (wrong-at-all-levels class) — MED effort, strongest end-to-end
- Oracle: **gcc** (trusted). Same C compiled by tcc (O0/O1/O2) and `arm-none-eabi-gcc -O2`, both run
  under the same QEMU harness, compare outputs.
- Catches what Track 2 cannot: bugs where all tcc levels agree but are wrong (frontend/ABI/codegen
  constants the optimizer never touched).
- UB trap: only feed UB-free inputs (the generator guarantees it). For the fixed corpus, the
  gcc-torture *execute* tests are already self-checking (they `abort()` on wrong results) — run
  tcc-compiled torture-execute and treat non-zero exits as candidate miscompiles, then triage against
  the known-skip list. This sharpens the existing `test-gcc-torture-execute` into a bug oracle.
- Deliverable: `scripts/diff_vs_gcc.py` (reuses Track 2 plumbing). Effort ~2–3 days.

## Track 4 — IR metamorphic / semantics-preservation fuzzer  (per-pass, host, LOCALIZING) — HIGH effort, the flagship
- The only **host-fast** oracle that pins a bug to **one pass** and a **minimal IR**. Built on `ir_build.h`.
- Components:
  1. `ir_eval.h` — a small reference **interpreter** over the TccIrOp subset: phase 1 arithmetic/logic/
     shift/cmp/assign/zext/ubfx/bfi (straight-line); phase 2 add load/store to a modeled stack; phase 3
     add jump/jumpif/return control flow. Computes a result vector from input register/memory vectors.
  2. `ir_gen.h` — a **random well-formed IR generator** (valid `irop_config` slots, single-def temps,
     type-consistent operands, in-range jump targets), seedable via a fixed RNG seed for reproducibility.
  3. Driver `test_metamorphic.c` — for each random fn `f`, each pass `P`, each of N random input vectors:
     assert `eval(f) == eval(P(f))`. Mismatch ⇒ `P` miscompiles. Also assert structural invariants after
     `P` (operand counts vs `irop_config`, vreg ranges, jump targets in bounds).
  4. **Delta-reducer** — shrink a failing `f` (drop instructions, lower operand magnitudes) while the
     mismatch persists → minimal repro for *Findings*.
- Run the whole thing under ASAN (folds in pass-level robustness fuzzing — the byte-drop/OOB class).
- Risk: the evaluator/generator are real code and can have their *own* bugs (false positives). Mitigate:
  start with the tiny phase-1 subset; cross-validate `eval()` against QEMU execution on a handful of
  hand-written cases; expand incrementally.
- Strength: isolates exact pass + minimal IR, no QEMU, runs in CI in seconds.
- Deliverable: `tests/unit/arm/armv8m/{ir_eval.h,ir_gen.h,test_metamorphic.c}`. Effort ~1–2 weeks, incremental.

---

## Recommended sequencing
1. **Track 1** (ASAN/UBSan sweep) — immediate, proven, parallelizable; flush the memory-safety class first.
2. **Track 2** (O-level diff) — quick win on existing QEMU infra; finds optimization miscompiles end-to-end.
3. **Track 4** (IR metamorphic) — start the evaluator small in parallel; the flagship + the foundation for Phase F.
4. **Track 3** (gcc diff) — cheap once Track 2 plumbing exists; catches the all-levels-agree-but-wrong class.

## Why this comes before legacy↔SSA (and feeds it)
Phase F equivalence is `eval(legacy(f)) == eval(ssa(f))`. That is **Track 4 with two pipelines instead of
one pass**. Building Track 4's generator + evaluator + reducer now means Phase F is a 1-line variation, and
any bug Tracks 1–4 surface gets fixed *before* the merge muddies attribution.

## Parallelization map (for a fan-out)
- Track 1: shard corpus across agents; each runs a slice, reports deduped sanitizer hits.
- Track 4: one agent builds `ir_eval.h`, others build `ir_gen.h` op-class generators; then per-pass drivers.
- Tracks 2/3: one harness, then a fleet generating + triaging random programs concurrently.

## Effort / risk summary
| Track | Oracle | Localizes to | Effort | Notes |
|---|---|---|---|---|
| 1 ASAN/UBSan | sanitizer | file:line | ~1d | proven; memory class only |
| 2 O-level diff | self-consistency | O-level | ~2–3d | needs random C gen (no csmith) |
| 3 gcc diff | arm-none-eabi-gcc | program | ~2–3d | UB triage required |
| 4 IR metamorphic | reference interpreter | **single pass + minimal IR** | ~1–2w | flagship; foundation for Phase F |

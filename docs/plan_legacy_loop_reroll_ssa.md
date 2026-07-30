# Plan: Replace `tcc_ir_opt_reroll` with `ssa:reroll`

**Status:** SSA replacement landing + legacy retirement (2026-07-07) ·
**Created:** 2026-07-07

Parent tracker: [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md).
Sibling precedent (the closest model): [`plan_legacy_loop_licm_ssa.md`](plan_legacy_loop_licm_ssa.md)
— a thin `ssa_opt_*` driver over a retained, regression-proven flat-IR engine,
relocated into the `tcc_ir_ssa_regalloc` pre-SSA flat region, with a
`TCC_DISABLE_PASS` knob and a `tcc_ir_dump_after_pass` dump name.

## What reroll is

`tcc_ir_opt_reroll` (`ir/opt_reroll.c`) is **identical-block loop re-rolling**:
it scans the linear instruction stream for runs of N ≥ `REROLL_MIN_REPEATS`(=4)
consecutive structurally-identical blocks of period P ∈ [`REROLL_MIN_PERIOD`(=3),
`REROLL_MAX_PERIOD`(=32)] — where vregs *defined inside* the block may be
consistently renamed across iterations — and re-rolls the run into a single
counted loop (`counter=0; body; counter++; CMP counter,N; JUMPIF LT →body`). Its
motivating idiom is macro-unrolled `__attribute__((cleanup))` scopes
(`tests/ir_tests/113_reroll_basic.c`), where thousands of identical blocks
otherwise explode code size.

It is **not** a natural-loop transform: it *creates* a loop from straight-line
code. So, unlike the other twelve tracker passes, it has no CFG/dominator loop
detector to upgrade — the engine's linear structural scan **is** the whole pass.

## Step 0: Placement experiment (decision gate)

The legacy pass sits at `tccgen.c:29497`, gated `opt_reroll` (`-O2` /
`-freroll-blocks`), **before** the tccgen propagation pipeline, with the standing
comment that propagation "can rewrite operand encodings in ways that vary across
iterations and would defeat structural matching." The decisive question for a
migration is whether the engine survives relocation to the post-propagation
`tcc_ir_ssa_regalloc` flat region (where every other migrated pass now lives).

Method: an env-gated A/B — disable the tccgen call, add a temporary call at the
regalloc-region entry — compiled at `-O2` with `-DTCC_LOG_REROLL=1`.

| shape | tccgen site (pre-prop) | regalloc site (post-prop) |
|---|---|---|
| `__cleanup__` ×8 (`INCR_GI`) | **fires** `P=9 N=8` | **inert** |
| opaque calls `ping()` ×6 (period 2) | inert (P<3) | inert (P<3) |

And a code-size measurement of the cleanup idiom (`arm-none-eabi-size`):

| | reroll ON | reroll OFF |
|---|---|---|
| 8 cleanup blocks | 84 B | **72 B** |
| 64 cleanup blocks | 84 B | **72 B** |

### Findings

1. **The pre-propagation firing is counterproductive.** The modern downstream
   optimizer (const-prop + IPC + DCE) already collapses N identical *foldable*
   blocks to constants (`glob_i += 64` — no loop, optimal); reroll's counter-loop
   *inhibits* that fold and yields **larger** code. The 64-blocks-equals-8-blocks
   size confirms downstream handles the collapse without reroll.
2. **Propagation only defeats matching for *foldable* runs.** The dump of the
   opaque-call idiom at regalloc shows the blocks stay byte-identical modulo
   `call_id` (`#0, #65536, #131072, …` = `k<<16`), which the matcher's
   `call_meta_src2_equiv` already masks. So the SSA region is a *valid* home for
   the engine. Foldable runs (the cleanup idiom) are gone by regalloc precisely
   because downstream folded them — so a post-propagation reroll correctly
   **declines** them instead of fighting the folder.
3. **Firing is rare on real code** — 0 firings across 133 corpus compiles; only
   the specific identical-block idiom triggers the legacy pass.

### Outcome

**Relocate to the post-propagation `ssa:reroll` position.** This is strictly
better than the legacy placement: it (a) stops preempting the downstream fold of
foldable idioms (finding 1), and (b) only fires on non-foldable repetition that
genuinely survives propagation (finding 2), which is the only case where
re-rolling is a real code-size win. Firing is rare (finding 3); that is
acceptable and documented — the concrete, always-on value of the migration is
removing the counterproductive pre-propagation firing plus SSA-region
consolidation and a proper disable/dump knob.

**Non-goals for this migration (explicit, to bound fuzz risk):** the matcher's
firing criteria are **unchanged** — no lowering of `REROLL_MIN_PERIOD`, no
relaxing of `body_calls_balanced`/`op_is_unsafe_for_reroll`, no new supported
shapes. Broadening the firing set (e.g. period-2 opaque-call runs) is a separate
plan with its own fuzz gate; re-rolling calls is exactly the seed-110274 /
test-299 phase-split hazard.

## SSA Replacement Design

### Placement

First pass of the `tcc_ir_ssa_regalloc` flat region, before `ssa:licm`,
preserving reroll's legacy relative order (it was the earliest loop transform in
the tccgen tail). Its synthetic back-edge is marked `no_unroll`, so the
downstream `ssa:loop_unroll` will not re-expand it; its body (the only surviving
case is non-foldable, e.g. calls) declines `try_eliminate_loop`/`loop_const_sim`
by their own memory/call guards. The rewrite uses the same flat-IR primitives
(`insert_instr_at`, `write_instr_at_nop`, `tcc_ir_get_vreg_var`, `is_jump_target`)
that `ssa:loop_unroll`/`ssa:loop_const_sim` already use safely at this position.

### Naming, files, observability

- Engine stays `tcc_ir_opt_reroll` in `ir/opt_reroll.c` — position-independent,
  unchanged, still called directly by the unit tests.
- Thin driver `int ssa_opt_reroll(struct TCCIRState *ir)` added in
  `ir/opt_reroll.c`, declared in `ir/opt/ssa_opt.h` (the SSA loop-pass header the
  other drivers use), returning the engine's reroll count.
- Wired from `ir/regalloc.c` gated `tcc_state->opt_reroll` and
  `!tcc_ir_opt_pass_disabled("ssa:reroll")`, followed by
  `tcc_ir_dump_after_pass(ir, "ssa:reroll")` — observable via
  `-dump-ir-passes=ssa:reroll`, isolable via `TCC_DISABLE_PASS=ssa:reroll` (both
  new; the legacy pass had only `-fno-reroll-blocks`).
- Gate unchanged: `opt_reroll` (`-O2` default; `-freroll-blocks` at lower levels;
  `-fno-reroll-blocks` still disables).

### Comment policy

Per [[comments-max-one-liner]]: new code carries no comment blocks; touched
legacy comments are deleted or compressed. The fuzz-seed narratives stay in this
plan and in the pinning IR tests, not in source.

## Tests

- **Unit (`tests/unit/arm/armv8m/test_opt_reroll.c`):** the existing 14 tests
  drive the engine directly and are position-independent — they cover the exact
  matcher + rewrite that `ssa:reroll` runs, so they carry over unchanged. Add one
  `ssa_`-surface test driving `ssa_opt_reroll` to pin the wrapper contract
  (fires → count 1, same rewritten shape) and one idempotence/no-op check.
- **IR regressions (`tests/ir_tests`):** `113_reroll_basic.c`,
  `114_reroll_negative.c`, `299_fuzz_reroll_call_phase_split.c` stay in
  `TEST_FILES`. They pin *runtime correctness* across the migration; post-migration
  113 compiles via the downstream fold (no reroll loop) and still returns the
  correct `glob_i==8`, which is the intended improvement.

## Migration Steps

- [x] Step 0 placement experiment (above).
- [ ] Add `ssa_opt_reroll` driver + `ssa_opt.h` declaration.
- [ ] Wire `ssa:reroll` first in the `tcc_ir_ssa_regalloc` flat region, with gate
  + `pass_disabled` + `dump_after_pass`.
- [ ] Remove the legacy `tccgen.c:29497` call site (tombstone pointer comment).
- [ ] Add the `ssa_` unit test; keep the 14 engine tests + 3 IR regressions.
- [ ] Update the parent tracker (enumerate reroll, tick it).

## Acceptance

- [ ] `make cross -j$(nproc)`.
- [ ] `make test-ir -j16` (green; 113/114/299 correct).
- [ ] Unit suite green (engine tests + new `ssa_` test).
- [ ] Corpus no-crash spot check at `-O2` with `TCC_DISABLE_PASS=ssa:reroll`
  parity.
- [ ] **Maintainer-run:** `diff_olevels --seeds 0-5000 --require-qemu` → 0 new
  divergences before the legacy engine's flag is removed.

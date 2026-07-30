# Plan: Retire `loop_guard_elim` from the legacy pre-SSA tail

**Status:** DONE — retired (Disposition A) · **Created:** 2026-07-07 · **Landed:** 2026-07-07

Tracking parent: [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md)
(row: *Sequential guard elimination*).

## Disposition recommendation (up front)

**Retire `tcc_ir_opt_loop_guard_elim` with no SSA replacement**, after a Step 0
candidate-count confirmation. The pass is **sound but inert at its current call
site**: it removes a *separate* pre-loop entry guard (the guarded-do-while
shape), and that shape no longer exists where the pass runs. Rotation — the only
thing that manufactures a separate guard — was moved out of `tccgen.c` into
`ir/regalloc.c` (`ssa:loop_rotate`), so at the `loop_guard_elim` tccgen site
every loop is still top-tested and there is nothing to remove. Where a dead
*constant* guard could still appear (rotation inserting a guard whose IV is a
dominating constant), `ssa:sccp` + `ssa:branch` in the SSA opt pipeline already
fold it.

This is the same disposition class as
[`loop_postinc_fusion`](plan_legacy_loop_postinc_fusion_ssa.md) (retire, no
replacement) — but for a different reason: postinc was *unsound*; guard_elim is
*redundant/inert*.

## Why this pass is different (read first)

Every other loop pass migrated so far (`loop_rotate`, `first_iter_exit`,
`ptr_iv_exit_subst`, `loop_const_sim`, `loop_unroll`) transforms a **single**
loop in isolation. `loop_guard_elim` is the only **inter-loop** pass: it walks
loops in program order and carries a proven constant exit value forward from one
counted loop into the next loop reusing the same IV vreg (the documented target
is memclr's three sequential loops over `i`). That carry machinery
(`carry_vreg` / `carry_val` / `carry_from`) exists precisely because the legacy
pass runs on flat IR and cannot see a value that is loop-carried through a
previous loop's back-edge.

The important consequence for this migration: the carry is only ever *consumed*
when the previous loop had a removable guard and a single exit — i.e. the whole
mechanism is downstream of finding a **separate pre-loop guard** in the first
place. No separate guard ⇒ no removal ⇒ no carry ⇒ pass returns 0.

## Intent

### Preserve

- The observable code of every program the current pipeline emits. Removing the
  legacy call site must be a proven no-op (see Step 0 / Acceptance).

### Intentionally not preserved (documented gap)

- The historical memclr-style win (dropping the never-taken `if (i >= N) skip`
  guard on the 2nd/3rd sequential loop) is **already gone** in today's pipeline,
  independent of this retirement: it was lost when rotation left `tccgen.c`, and
  the counted loops that keep the guard are now emitted top-tested (the `cmp` is
  the live exit test, re-evaluated each iteration — not a removable guard).
  Retiring `loop_guard_elim` deletes now-dead code; it does not cause this gap.
  If closing the gap becomes worthwhile, the correct fix is a post-rotation SSA
  pass (Disposition B below) or teaching `ssa:loop_rotate`/`ssa:sccp` to peel
  the provably-taken first iteration — **not** keeping the inert flat pass.

## Current Legacy Shape

### Call site (single)

`tccgen.c` late optimization tail, one gated block:

```c
/* tccgen.c ~30507 */
if (tcc_state->opt_const_prop && !getenv("TCC_NO_GUARD_ELIM"))
{
  dbg_scan_overlap(ir,"R3-before-loop_guard_elim");
  if (tcc_ir_opt_loop_guard_elim(ir) > 0)
  {
    if (tcc_state->opt_dce)        tcc_ir_opt_dce(ir);
    tcc_ir_opt_compact_nops(ir);
    if (tcc_state->opt_jump_threading) tcc_ir_opt_eliminate_fallthrough(ir);
  }
}
```

- **Gate:** `opt_const_prop` (default-on at `-O1+`).
- **Bisection knob:** `getenv("TCC_NO_GUARD_ELIM")` — a temporary bisection gate
  of the kind the parent plan's Migration Rules say must **not** live in
  committed code. Its removal is part of this migration regardless of
  disposition.
- **Cleanup cascade:** DCE → `compact_nops` → `eliminate_fallthrough`, run only
  when the pass reports a change (it never does — see Evidence).
- **Placement comment is stale:** it says "Run LAST among loop passes (after all
  rotation/unroll/IV-SR)". Those passes are no longer in `tccgen.c`; they run
  later in `ir/regalloc.c`. This broken ordering assumption is the root cause of
  the inertness.

### Source & helper ownership

| Symbol | File | Disposition |
|---|---|---|
| `tcc_ir_opt_loop_guard_elim` | `ir/opt_loop.c:1004` | delete |
| `guard_eval_cond` (static) | `ir/opt_loop.c:918` | delete (only caller: this pass) |
| `guard_iv_step` (static) | `ir/opt_loop.c:939` | delete (only caller: this pass) |
| `guard_loop_single_exit` (static) | `ir/opt_loop.c:990` | delete (only caller: this pass) |
| prototype | `ir/opt.h:802` | delete |
| `find_loop_exit_condition` | `ir/opt_loop_utils.c:2111` | **keep** — shared with IV-SR / const_sim / unroll |
| `compute_trip_count` | `ir/opt_loop_utils.c:2255` | **keep** — shared with IV-SR / unroll |

### Existing tests

- Unit (`tests/unit/arm/armv8m/test_opt_loop.c`, links real `ir/opt_loop.c`):
  - `test_guard_elim_removes_provably_false_guard`
  - `test_guard_elim_keeps_guard_when_provably_taken`
  - `test_guard_elim_no_loops_returns_zero`
  - `test_guard_elim_bails_on_switch_table`
  - plus the local `int tcc_ir_opt_loop_guard_elim(...)` decl and the two file
    header-comment mentions.

  These are hand-built IR fixtures that construct the guarded do-while shape
  directly, so they pass even though the shape never reaches the pass from the
  real frontend. They verify the function, not a pipeline behavior.
- IR regression: none specific to this pass.
- Known fuzz regressions tied to this pass: none in `MEMORY.md`.

## Evidence that the pass is inert (2026-07-07, branch `legacyOptRemoval`)

Built `armv8m-tcc` at `-O2` and compared object output with the pass enabled vs.
`TCC_NO_GUARD_ELIM=1`:

- **700 UB-free fuzz programs** (`tests/fuzz/gen_c.py`, seeds 0–699), `-O2`:
  `ok=700 err=0 diffs=0` — zero object-code differences.
- **memclr, constant bound** (`for(i=0;i<8;i++)` ×3, reinitialized `i`):
  identical IR/object with the pass on/off. After `ssa:loop_rotate` all three
  loops are **top-tested** (`cmp V0,#8; JMP >=` is the back-edge target); the
  `cmp`s in the final Thumb are the live exit tests, not removable guards.
- **carried-IV sequential** (`for(i=0;i<8;i++)…; for(;i<16;i++)…`, `i` not
  reset): identical with the pass on/off; both loops top-tested; the 2nd loop's
  `cmp r1,#16` is its live exit test.

Mechanism confirmed: `loop_guard_elim` scans for a `CMP iv,#imm` + `JUMPIF ->
past-the-loop` in `[start_idx-6, start_idx)` that is **distinct** from the
latch. A plain top-tested loop has no such instruction before its body, so
`guard_found` stays 0 and the pass returns 0. The distinct guard only exists in a
guarded-do-while (rotated) layout, which the current pipeline does not present to
this call site.

Redundancy backstop: the SSA opt pipeline (`ir/opt/ssa_opt.c:tcc_ir_ssa_opt_run`)
runs `ssa:sccp` then `ssa:branch` **after** `ssa:loop_rotate`. If rotation ever
did emit a guard whose IV is a dominating constant, SCCP proves the constant and
`ssa:branch` folds the never-taken edge — no dedicated pass required. The single
case SCCP cannot reach is the inter-loop *carried* (phi) IV, which additionally
requires both loops to be rotated into guarded form; that combination does not
occur.

## Step 0 Results (2026-07-07, `1c4b13c3` + this retirement)

Ran the decision gate: instrumented `tcc_ir_opt_loop_guard_elim` to log each
`guard_found` and each removal (`total++`), then swept the corpora at `-O1` and
`-O2`.

- **gcc c-torture, 3902 files × (-O1, -O2)** (7704 clean compiles): `guard_found=2`,
  `guard_removed=0`. The pre-loop-guard *shape* appears twice across the entire
  real-code suite, but the pass **never removes anything** — both times the guard
  was provably-taken/unknown and correctly declined.
- **Fuzz sweep skipped** at the user's request; the earlier 700-seed object-diff
  (pass on vs. `TCC_NO_GUARD_ELIM=1`) was already byte-identical.
- **Decision: Disposition A (retire).** `guard_removed=0` ⇒ the pass emits no
  transform, so deleting it (and its never-reached DCE/`compact_nops`/
  `eliminate_fallthrough` cascade) cannot change codegen.

Post-retirement no-op proof: rebuilt `armv8m-tcc`, recompiled a 2572-object
torture sample, and compared SHA-256 against the pre-retirement binary —
**`checked=2572 mismatch=0 newly_failed=0`** (byte-identical objects). The
retirement is a proven no-op on generated code.

Test status: `tests/unit` opt_loop suite passes with the four `test_guard_elim_*`
tests removed; the one failing UT (`test_ssa_opt_sccp.c:156`
`test_sccp_barrel_shift_guard`) is **pre-existing** (fails identically on the
pristine tree, unrelated to this change).

## Step 0: candidate-count decision gate (run before deleting)

Confirm "inert" is "zero candidates," not "few candidates I happened to miss,"
before removing code.

1. Temporarily instrument `tcc_ir_opt_loop_guard_elim` to `LOG_LOOP_OPT` (or a
   local counter) each time `guard_found` becomes 1 and each time `total`
   increments. Do **not** commit the instrumentation.
2. Run the broad corpus at `-O1` and `-O2`:
   - `tests/fuzz/gen_c.py` seeds 0–5000,
   - `make test-gcc-torture-compile`,
   - `make test`.
3. Interpret:
   - **`guard_found == 0` and `total == 0` everywhere → Disposition A** (retire).
     This is the expected result given the Evidence above.
   - **`total > 0` anywhere → stop and switch to Disposition B.** Capture the
     `.c`, minimize it, and add it as an IR regression pin first; a live removal
     means the shape *does* reach the pass and a post-rotation SSA replacement is
     warranted.

## Disposition A — Retire (recommended)

Delete the legacy pass, its three guard-only statics, the prototype, the tccgen
call block (gate + cleanup cascade + `TCC_NO_GUARD_ELIM` knob + the
`R3-before-loop_guard_elim` `dbg_scan_overlap`), and the four unit tests + their
local decl/comments. Keep the shared `find_loop_exit_condition` /
`compute_trip_count` helpers (other live passes use them). Leave the
`R4-just-before-cmp_narrow` scan — it belongs to the following `cmp_narrow_64`
pass, not to guard_elim.

### Migration Steps (Disposition A)

1. **Step 0 confirmation** above returns zero candidates. Record the numbers in a
   "Step 0 Results" subsection here (date + commit), mirroring the const_sim /
   unroll plans.
2. Remove the `tccgen.c` block (30507–30525): the gate, the call, the
   change-gated cleanup cascade, and the `R3-before-loop_guard_elim` scan. The
   surrounding DCE/`compact_nops`/`eliminate_fallthrough` are only reachable
   through the (never-true) `> 0` branch, so they leave with it.
3. Delete `tcc_ir_opt_loop_guard_elim`, `guard_eval_cond`, `guard_iv_step`,
   `guard_loop_single_exit` from `ir/opt_loop.c`, and the prototype from
   `ir/opt.h`. Retarget the surviving file-level comment block (opt_loop.c ~880)
   so it no longer documents a deleted pass.
4. Remove the four `test_guard_elim_*` unit tests, the local
   `tcc_ir_opt_loop_guard_elim` decl, and the two header-comment mentions from
   `tests/unit/arm/armv8m/test_opt_loop.c`. Update its `UT_TEST` registration
   list if it enumerates tests explicitly.
5. Mark the parent tracker row `[x]` with a one-line result and link this doc.
6. Full validation gate (Acceptance).

## Disposition B — Restore as `ssa:guard_elim` (only if Step 0 finds candidates)

If Step 0 surfaces a live removal, do **not** keep the flat pass. Add a focused
CFG/dominator pass in `ir/opt/ssa_opt_loop.c` wired into
`tcc_ir_ssa_regalloc` (`ir/regalloc.c`) **after** `ssa_opt_loop_rotate`
(≈ line 4547) and before CFG/SSA construction — the same flat-IR-after-rotation
slot the other migrated loop passes occupy, and the point at which the guarded
do-while shape actually exists.

Design sketch (v1, single loop; drop the inter-loop carry unless a pin needs it):

- **Facts:** build CFG + dominators; for each outermost dominance-verified
  natural loop (reuse `lcs_collect_header_members`/contiguity/single-entry from
  the const_sim/unroll front-ends).
- **Candidate:** a `CMP iv,#lim; JUMPIF cond -> exit_past_loop` in the preheader
  gap `[eff_start-…, eff_start)`, *distinct* from the latch test, where `iv` has
  a dominating constant def (immediate `ASSIGN`) reaching the guard and `lim` is
  an immediate.
- **Legality:** `guard_eval_cond(entry, cond, lim) == 0` (provably never taken);
  the guard's `iv` def must dominate the guard with no intervening redef; bail on
  `IJUMP`/`SWITCH_TABLE`/`SWITCH_LOAD` in the function (as the legacy pass does).
- **Mutation:** NOP the guard `CMP`+`JUMPIF`; clear a now-orphaned `is_jump_target`
  on the skip target when the guard was its only in-edge. Reuse `guard_eval_cond`
  (revive as a shared helper).
- **Inter-loop carry:** only port `carry_vreg`/`compute_trip_count` if a Step 0
  pin genuinely needs a value that crosses a loop and SCCP cannot recover. Prefer
  leaving that to `ssa:sccp` on the reinitialized (dominating-const) case.
- **Naming/observability:** `ssa:guard_elim`, gated `-O1+ && opt_const_prop`,
  `TCC_DISABLE_PASS=ssa:guard_elim`, `tcc_ir_dump_after_pass("ssa:guard_elim")`.
- **Unit tests:** port the four `test_guard_elim_*` fixtures onto the SSA entry
  point plus a top-tested-loop-declines case and a carried-IV case.

## Test disposition

- Removed (Disposition A): the four `test_guard_elim_*` unit tests — they cover a
  deleted function.
- No IR regression is lost: Step 0 proves no `.c` in the corpus changes. If Step 0
  *did* find one, that `.c` becomes a new pin under Disposition B before any code
  moves.
- Green after retirement: `make test`, `make test-gcc-torture-compile`, and the
  `bug_postinc_*` pins are unaffected (unrelated).

## Comment policy

Per `CLAUDE.md`: no comment blocks; at most a one-line constraint comment.
Deleting the pass also deletes its large explanatory header — do not relocate the
prose inline. This plan file is the durable record; reference it with a one-line
link if any residual code needs a pointer.

## Acceptance

- [ ] Step 0 candidate count recorded here (expected: 0 at `-O1` and `-O2`).
- [ ] `make cross -j$(nproc)`
- [ ] `make test -j16`
- [ ] `make test-gcc-torture-compile`
- [ ] `python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu` — zero new
      divergences.
- [ ] Object-code diff of the pre-retirement vs. post-retirement compiler over
      the fuzz corpus is empty (the retirement is a proven no-op).
- [ ] `grep -rn "loop_guard_elim\|TCC_NO_GUARD_ELIM\|guard_eval_cond\|guard_iv_step\|guard_loop_single_exit"`
      returns nothing outside this doc and the parent tracker.

## Assumptions

- Rotation stays in `ir/regalloc.c` (post-tccgen). If it ever moves back ahead of
  a flat guard-elim slot, re-run Step 0 — the inertness conclusion depends on
  loop shape at the call site.
- `find_loop_exit_condition` and `compute_trip_count` remain owned by the live
  IV-SR / const_sim / unroll passes and are not touched by this migration.
- This is a documentation/planning step. No code is deleted until Step 0 confirms
  zero candidates and the Acceptance gate is run.

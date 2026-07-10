# Plan: Replace `decrement_to_zero` with `ssa:decrement_to_zero`

**Status:** proposed pass plan · **Created:** 2026-07-07

Parent tracker: [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md).
Sibling precedents this plan follows directly:
[`plan_legacy_loop_ptr_iv_exit_subst_ssa.md`](plan_legacy_loop_ptr_iv_exit_subst_ssa.md)
and
[`plan_legacy_loop_const_sim_ssa.md`](plan_legacy_loop_const_sim_ssa.md)
— both are the **"legacy pass went inert once rotation left tccgen; the SSA
pass restores it"** shape, which (per the probe below) is exactly this pass's
situation. Also relevant:
[`plan_legacy_loop_unroll_ssa.md`](plan_legacy_loop_unroll_ssa.md)
(the immediately-upstream flat loop pass — `decrement_to_zero` runs on the loops
`ssa:loop_const_sim`/`ssa:loop_unroll` did **not** eliminate) and
[`plan_legacy_loop_postinc_fusion_ssa.md`](plan_legacy_loop_postinc_fusion_ssa.md)
(the "retire, don't replace" template — the fallback disposition if the Step-0
measurement shows the restored candidate set is empty in practice).

## Read first: the pass is inert today, and it is restorable

`decrement_to_zero` rewrites a count-**up** counted loop into a count-**down-to-zero**
loop so the backend can fuse the latch `SUB #1` + back-edge `CMP #0` into a single
flag-setting `SUBS` (the `ir/codegen.c:2689-2721` peephole, EQ/NE only). That win
is real, but the pass **does not fire at all in the current tree**, for the same
reason `ptr_iv_exit_subst` and `loop_const_sim` were inert at retirement:

- The legacy call site (`tccgen.c:30385`) runs the transform **before** loop
  rotation now happens. Rotation migrated out of tccgen to regalloc time
  (`ssa:loop_rotate`), so at the tccgen `decrement_to_zero` site every loop is
  still **top-tested / un-rotated**, and the transform needs the **rotated**
  shape (a separate pre-test guard *plus* a bottom-tested back-edge `CMP`/`JUMPIF`).
- **Confirmed empirically** (this build, `-dump-ir-passes=ZZ2_dtz`): a canonical
  `for (i=0;i<7;i++)` and a `do { } while (i<50)` both reach the pass with the IV
  unchanged — `V1 <-- #0`, `CMP V1,#7`, `V1 <-- V1 ADD #1` — i.e. still counting
  up. Zero changes.

But the candidate shape **does** re-appear later in the regalloc pipeline, after
`ssa:loop_rotate` rotates the loop and after `ssa:loop_const_sim`/`ssa:loop_unroll`
decline it (side-effecting body). Confirmed with
`for (int i=0;i<100;i++) sink=5;` (`sink` volatile, `i` a pure counter): at
`-dump-ir-passes=ssa:loop_unroll` the IR is the **exact** legacy candidate —

```
0000: V0 <-- #0        ; init i=0
0001: CMP V0,#100      ; pre-test guard
0002: JMP to 8 if ">=S"; guard exit
0003: GlobalSym***DEREF*** <-- #5 [STORE]   ; body — i unused
0004: V0 <-- V0 ADD #1 ; increment
0005: CMP V0,#100      ; back-edge test
0006: JMP to 3 if "<S" ; back-edge (bottom-tested)
0007: NOP
```

So the disposition is **restore, not retire** (the `postinc_fusion` route): add
`ssa:decrement_to_zero` in the regalloc pre-SSA loop block, positioned **after
`ssa:loop_unroll`**, reusing the legacy detection + rewrite driven from a
CFG/dominator front-end — exactly the `ptr_iv_exit_subst` / `loop_const_sim`
pattern. The legacy pass being inert means coexistence is trivially non-divergent
(it changes nothing today), and the SSA pass *recovers* the lost `SUBS` fusion.

### Caveat that shapes acceptance: the candidate set is narrow

The candidate is the intersection of four conditions, and they pull against each
other:

1. **Register-only / plain-store body** — `ssa:loop_rotate` *declines* bodies
   containing a call, an indexed memory op (`p[i]`), or an indirect lvalue
   (rotation plan, lines 177-179). So the loop must not index by `i` or call.
2. **Pure counter** — the IV must be unused in the body except increment + the
   two `CMP`s (legacy "no other uses" scan).
3. **Constant limit > 0** — legacy requires an immediate bound.
4. **Survives `ssa:loop_const_sim` / `ssa:loop_unroll`** — a register-only counted
   loop with a computable trip is collapsed to a closed form by those passes
   before `decrement_to_zero` sees it; only a loop with a **non-collapsible side
   effect** (a plain store to a fixed location, a spin loop) survives.

Conditions 1+2 (no `i`-indexing, `i` unused) exclude the most common counted
loops (array fills, `use(i)` calls); condition 4 excludes the pure accumulators.
What remains is loops like `for (i=0;i<N;i++) *fixed = v;` — real but uncommon.
**Because the win per fire is only one instruction/iteration and the candidate
set is narrow, this plan makes a codesize/fuzz measurement part of the acceptance
gate** (see Acceptance): if the restored pass proves to change nothing measurable
on the corpus, fall back to the `postinc_fusion` retirement disposition (keep the
sound backend peephole, delete the dead producer) rather than carrying a new pass
that never fires. The recommendation is to **restore** (parity + the confirmed
`purestore`-class win), but the measurement decides.

## Intent

- **Preserve:** the count-up→count-down-to-zero rewrite and its exact legality
  guards (init `=0`, step `+1`, constant limit `>0`, IV a pure counter, a
  *separate* pre-test guard distinct from the back-edge test, back-edge condition
  `<S`). Rewriting to init `=limit`, step `-1`, back-edge `CMP #0` with condition
  `!=` (never `>S` — the `SUBS`/`CMP #0` peephole is Z-flag-only, EQ/NE), and
  NOPing the always-taken pre-test guard. The downstream `SUBS` fusion's matching
  logic is unchanged; its forward scan was extended in Step 0 to reach the branch
  past the out-of-SSA phi copy (see Step 0 results).
- **Restore (currently lost):** the transform actually running. Today it is
  inert; the SSA pass placed after rotation makes it fire again on the surviving
  bottom-tested candidates.
- **Not preserved (deliberately):** the flat `tcc_ir_detect_loops` candidate
  source with its overlap-merge and function-wide range scans. As with the
  siblings, candidates become **dominance-verified outermost natural loops** from
  a CFG front-end; the per-loop detection + rewrite engine is retained verbatim.
- **Non-goals (v1):**
  - No SSA-native (phi-aware) version inside the `tcc_ir_ssa_opt_run` fixed point;
    v1 is the same flat-IR mutation on CFG-verified candidates (same deferral as
    rotation/const_sim/unroll).
  - **No broadening to do-while / bottom-tested-without-a-guard shapes.** Legacy
    declines those (`test_decrement_to_zero_no_separate_pretest_guard_bails`,
    bugs.md #12). A natively bottom-tested `do { } while (i<N)` is arguably the
    *better* count-down target (no guard to NOP), but legacy never handled it and
    v1 preserves that decline. Broadening is recorded under "Deferred" — it is
    where the *additional* value is, but it is a behavior change, not a migration.
  - No widening of the `init=0 / step=+1 / const limit` requirements, and no new
    condition codes beyond the legacy `<S`→`!=` rewrite.

## Current Legacy Shape

- **Call site:** single site in `tccgen.c` (`≈30382-30389`), **ungated** — called
  unconditionally, not behind any `opt_*` flag. Wrapped by
  `dbg_scan_overlap(ir,"Q4-before-decrement_to_zero")` /
  `"Q4b-after-decrement_to_zero"` and a `dump_ir_after_pass(..., "ZZ2_dtz")` under
  `CONFIG_TCC_DEBUG`. Positioned **after** `loop_bound_remat`
  (`tccgen.c:30380`) and **before** `redundant_init_elim` (`tccgen.c:30394`).
  The comment claims "Must run late, after IV-SR has eliminated body uses of loop
  counters" — that ordering is real but the pass is inert here regardless (see
  above). Ungated in source, but effectively `optimize>=1`-only: it needs the
  rotated shape, which only `ssa:loop_rotate` (`optimize>=1`) produces.
- **Gate / flag:** none of its own. No `-f` flag, **no `opt_pipeline.c` table
  entry**, **not in `scripts/bisect_opt.py`**, **no `PASS_COVERAGE.md` row**. The
  only isolation available today is editing the call site. The replacement fixes
  this (`ssa:decrement_to_zero` dump name + `TCC_DISABLE_PASS`).
- **Observability:** `ZZ2_dtz` dump under `CONFIG_TCC_DEBUG` only; not registered
  with `tcc_ir_opt_pass_disabled`.
- **Driver:** `tcc_ir_opt_decrement_to_zero` (`ir/opt_loop.c:581`, prototype
  `ir/opt.h:609`). Self-detects loops via `tcc_ir_detect_loops` /
  `tcc_ir_free_loops`. Per detected loop:
  1. **IV def** — scan back from `loop->end_idx` for the latch `V = V + 1` where
     `V` is a `VAR`, `dest==src1`, `src2==#1` (skipping NOP/JUMP/CMP/JUMPIF).
  2. **Back-edge test** — within the last ~6 insns, a `CMP V,#limit` immediately
     (over NOPs) followed by `JUMPIF` with condition `<S` (`0x9c`, `TOK_LT`) and
     `limit>0`.
  3. **IV init** — near `loop->preheader_idx`, an `ASSIGN V,#0`.
  4. **Pre-test guard** — a *separate* `CMP V,#limit` + `JUMPIF` in the header
     window, **explicitly skipping any index that coincides with the back-edge
     `CMP`/`JUMPIF`** (bugs.md #12: NOPing a coincident guard would delete the
     loop's only back-edge test and silently degenerate it to one iteration —
     locked by `test_decrement_to_zero_no_separate_pretest_guard_bails`).
  5. **No-other-use scan** — the IV, and an optional copy-through temp
     (`T=V` before `V=T+step`), must have no uses across the IV's live range
     (init → post-loop redefinition) other than init/increment/the two `CMP`s.
  6. **Require guard found** — bails if no separate pre-test guard (changing init
     to `#limit` without a patched/removed guard would skip the loop).
  7. **Apply** (in place, no IR growth): init `#0`→`#limit`; increment
     `ADD`→`SUB #1`; back-edge `CMP #limit`→`CMP #0`; condition `<S`→`!=`
     (`0x95`, `TOK_NE`); NOP the pre-test guard `CMP`+`JUMPIF`.
- **Downstream beneficiary (extended in Step 0):** the `SUBS`/`CMP #0` fusion
  peephole (`ir/codegen.c`) that fuses an `ADD`/`SUB` immediately followed by a
  `CMP dest,#0` of the same vreg into one flag-setting op, **EQ/NE only**
  (Z-flag). It requires the `SUB`/`CMP #0` adjacent and, up to the branch, only
  flag-neutral instructions. The original was `out of scope for change`, but Step
  0 found its forward scan (skipping only NOPs) declined every `decrement_to_zero`
  latch because `ra_resolve_phis` puts the loop-carried phi copy between the `CMP`
  and the `JUMPIF` (see Step 0 results). The scan now also steps over an
  **identity move** — a phi copy that coalesced to a physical self-move, which the
  assign/load codegen already elides to no code, so flags survive. The matching
  logic and the EQ/NE-only Z-flag constraint are unchanged; a non-coalesced copy
  still declines (safe).
- **Existing unit tests** (`tests/unit/arm/armv8m/test_opt_loop.c`, call the
  driver directly, bypassing any gate):
  - `test_decrement_to_zero_basic_countup_rewritten` (`:491`) — a body that
    **reads** the IV: asserts **0 changes** (pure-counter requirement).
  - `test_decrement_to_zero_pure_counter_rewritten` (`:522`) — the happy path:
    asserts all five rewrites (init `#7`, `SUB #1`, back-edge `CMP #0`, `NE`,
    guard NOPed).
  - `test_decrement_to_zero_no_separate_pretest_guard_bails` (`:559`) — the
    bugs.md #12 regression lock: no separate guard → 0 changes, loop intact.
  - `test_decrement_to_zero_no_loops_returns_zero` (`:597`).
  - `UT_COVERS("loop_decrement_to_zero")` (`:1381`).
- **IR regression pins:** none dedicated. There is **no** `tests/ir_tests/`
  entry that end-to-end exercises the count-down rewrite (grep found none). This
  is a coverage gap the migration must close (see Migration Steps).
- **Fuzz history tied to this pass:** none in the memory index (the pass has been
  inert since rotation migrated, so it has generated no recent divergences). The
  only recorded correctness event is the pre-existing **bugs.md #12**
  guard-coincidence hazard, already fixed and unit-locked.

## Why an SSA replacement restores it (and why placement is after `ssa:loop_unroll`)

`ssa:loop_rotate` converts safe top-tested loops to bottom-tested **for
register-only bodies** (it declines call/indexed-mem/indirect-lval bodies). That
rotation produces precisely the "separate pre-test guard + bottom-tested
back-edge `CMP`/`JUMPIF`" shape the legacy detector wants (confirmed on
`for (i=0;i<30;i++) acc ^= i*7+3;` — the IR at `ssa:loop_rotate` shows the
guard `CMP V1,#30`+`>=S` at the top and the back-edge `CMP V1,#30`+`<S` at the
bottom). `ssa:loop_const_sim` and `ssa:loop_unroll` then eliminate the
*collapsible* register-only loops. Whatever survives with a bottom-tested counted
shape and a pure-counter IV is the `decrement_to_zero` candidate.

Therefore the pass must run **after `ssa:loop_unroll`** (so it only touches
survivors, matching legacy's "runs after unroll" tccgen order) and **before the
CFG/SSA build** (`ir/regalloc.c:4593`), so the NOPed guard's control-flow change
is reflected when the CFG is built and the downstream SSA `branch`/`dce`/
`dead_loop` passes clean it up. This is the identical placement contract used by
`ssa:loop_const_sim` and `ssa:loop_unroll`.

## SSA Replacement Design

- **Required CFG/SSA facts:** dominance-verified natural loops. Reuse the
  `ssa_opt_loop.c` front-end already used by `ssa_opt_loop_const_sim` /
  `ssa_opt_loop_unroll`: build the CFG, compute dominators, find headers
  (`succ h` with `dominates(h,b)`), collect members
  (`lcs_collect_header_members`), and restrict to **outermost** headers (decline
  a header nested in another's member set). This replaces the legacy flat
  `tcc_ir_detect_loops` overlap-merge and the function-wide external-entry scan
  with the same soundness guarantees the siblings established.
- **Candidate loop pattern:** unchanged from legacy (init `=0`, step `+1`, const
  limit `>0`, pure-counter IV, separate pre-test guard, back-edge `<S`).
- **Legality guards / side-effect rules:** unchanged from legacy, verbatim —
  including the bugs.md #12 guard-coincidence skip, the copy-through-temp
  allowance, and the live-range-bounded no-other-use scan.
- **Mutation strategy:** unchanged in-place rewrite (no IR growth; five field
  edits + two NOPs per loop). Idempotent — its output has no count-up IV with a
  separate pre-test guard, so a second pass finds nothing.
- **Structure (mirror `loop_const_sim`):** retain the ~300-line legacy engine as
  a region helper — `dtz_try_region(ir, start, end, header_idx, preheader_idx)`
  returning 1 on fire — carved out of `tcc_ir_opt_decrement_to_zero` in
  `ir/opt_loop.c` (or moved to `ir/opt_loop_utils.c` as a shared mutator, matching
  where `try_rotate_loop` / `try_eliminate_loop` live). Add the driver
  `ssa_opt_decrement_to_zero(TCCIRState*)` in `ir/opt/ssa_opt_loop.c` that walks
  outermost headers, builds a synthetic contiguous single-entry `IRLoop` from the
  member span (the `unroll_try_candidate` pattern: `eff_start`/`eff_end` from
  member blocks, `preheader` = first non-jump before header), and calls the
  region helper. Bounded fixed point like the siblings
  (`SSA_LOOP_..._MAX_PASSES`), though one pass suffices (idempotent, disjoint
  spans).
- **Pass ordering:** in `tcc_ir_ssa_regalloc`, immediately after the
  `ssa:loop_unroll` block (`ir/regalloc.c:≈4590`) and before `tcc_ir_cfg_build`
  (`:4593`).
- **Gate:** `tcc_state && tcc_state->optimize >= 1 &&
  !tcc_ir_opt_pass_disabled("ssa:decrement_to_zero")` — matches `ssa:loop_rotate`
  (the shape provider). The legacy pass was ungated but effectively `>=1`-only.
  Whether to further restrict to `-O2` is a Step-0 codesize/fuzz call; default to
  `>=1` for parity with the rotation it consumes and to give `-O1` the `SUBS`
  win. Follow with `tcc_ir_dump_after_pass(ir, "ssa:decrement_to_zero")`.
- **Pass name:** `ssa:decrement_to_zero` for `-dump-ir-passes` and
  `TCC_DISABLE_PASS`. Add to `scripts/bisect_opt.py` knobs and a
  `PASS_COVERAGE.md` row.

## Interaction with other passes

- **`ssa:loop_rotate` (upstream, provider):** produces the bottom-tested + guard
  shape. `decrement_to_zero` is a strict consumer; if rotation declines a loop
  (call/indexed/indirect body), there is no candidate — this is the dominant
  reason the candidate set is narrow, and it is *correct* (those bodies would not
  benefit from `SUBS` fusion in the same way).
- **`ssa:loop_const_sim` / `ssa:loop_unroll` (upstream, siblings):** eliminate the
  collapsible register-only counted loops first; `decrement_to_zero` sees only
  survivors. Running after them is required (a loop these would collapse must not
  be rewritten instead).
- **IV strength reduction (still legacy in tccgen, upstream):** runs before all of
  regalloc, so the "after IV-SR" ordering the legacy comment asserts is preserved
  automatically. IV-SR strips body uses of the counter; what it leaves is exactly
  the pure-counter IV `decrement_to_zero` needs.
- **`redundant_init_elim` (tccgen, downstream today):** the legacy comment says it
  "must run after decrement-to-zero (which NOPs pre-test guards)". This
  dependency is **already unfulfilled today** — `decrement_to_zero` is inert at
  the tccgen site, so `redundant_init_elim` already sees un-NOPed guards. Moving
  `decrement_to_zero` to regalloc therefore **regresses nothing** here; the guard
  NOP now happens in regalloc, and the SSA `branch`/`dce`/`dead_loop` passes
  (which run after `decrement_to_zero`) do the equivalent cleanup. Verify with the
  Step-0 baseline that no `redundant_init_elim`-dependent codegen changes.
- **The `SUBS`/`CMP #0` codegen peephole (downstream):** the sole consumer of the
  rewrite's output. Adjacency in the bottom-tested latch makes it fire; Step 0
  extended its forward scan to reach the branch past the out-of-SSA identity-move
  phi copy (otherwise it declined every candidate).

## Step 0 results (2026-07-07) — win was blocked, then unblocked

Legacy pass confirmed inert (fires 0× in the current tree). The SSA pass fires
on the `purestore` shape (`for(i=0;i<100;i++) sink=5;`) and produces the exact
count-down IR (`V0 <-- #100`, `SUB #1`, `CMP V0,#0`, `JUMPIF !=`, guard NOPed).

**But the intended `SUBS` win did not materialise.** Measured `-O1` output was
**byte-identical** enabled vs disabled — the latch stayed `subs; cmp #0; bne`
(3 insns), not the fused `subs; bne` (2). Root cause: `decrement_to_zero`'s IV is
always the loop-carried counter, so after the SSA round-trip `ra_resolve_phis`
re-inserts the phi copy `T3 <-- T6` **between the latch `CMP` and `JUMPIF`**. The
`SUBS`/`CMP #0` codegen peephole scans forward from the `SUB` for the branch
skipping only NOPs, hits that `ASSIGN`, and declines — so the `cmp` is never
dropped. (The legacy pass could fuse only because it ran pre-SSA, before phi
resolution; it has been inert since rotation left tccgen regardless.)

**Fix (approved deviation from "peephole out of scope"):** the phi copy here
coalesces to a physical **self-move** (`R0 <-- R0`) that the assign/load codegen
already elides to no code, so it is flag-neutral. The peephole's forward scan now
steps over such an identity move (`ir_codegen_is_identity_move`, the same idiom
the STRD-fusion scan already uses), and the latch fuses to `subs; bne` — **−1
instruction/iteration**. If the copy does *not* coalesce (distinct regs) the scan
does not skip it and the fusion simply declines (safe). The transform is
therefore **restored** with a realised per-iteration win.

## Migration Steps

- [x] **Step 0 — establish inertness + measure the win.** Done — see "Step 0
  results" above. Legacy inert; SSA pass fires on `purestore`; the SUBS win was
  blocked by the out-of-SSA phi copy and unblocked via the identity-move scan.
- [x] **Add regression coverage first.** Added `tests/ir_tests/349_decrement_to_zero.c`
  (registered in `test_qemu.py`): pure-counter purestore at three constant limits,
  an IV-read decline, nested loops, and a runtime-limit decline; verified correct
  at `-O0/-O1/-O2/-Os` under QEMU.
- [x] **Carve out the engine.** `dtz_try_region(ir, start, end, header_idx,
  preheader_idx)` extracted verbatim into `ir/opt_loop_utils.c` (declared in
  `ir/opt_loop_utils.h`); the four unit tests now drive the SSA driver.
- [x] **Add the SSA driver.** `ssa_opt_decrement_to_zero` in
  `ir/opt/ssa_opt_loop.c` (outermost-natural-loop front-end, no has_memory
  decline — plain-store bodies are the candidate); declared in `ir/opt/ssa_opt.h`.
- [x] **Wire the call site.** Gated call (`optimize>=1`,
  `!tcc_ir_opt_pass_disabled("ssa:decrement_to_zero")`) + dump added in
  `tcc_ir_ssa_regalloc` after `ssa:loop_unroll`.  `bisect_opt.py` auto-discovers
  the dump label (introspective, no manual entry); `PASS_COVERAGE.md` row added.
- [x] **Unblock the fusion.** Extended the codegen forward scan to step over an
  identity-move phi copy (see Step 0 results) so the count-down latch fuses.
- [x] **Remove the legacy call site + driver.** `tccgen.c` block (call +
  `dbg_scan_overlap` + `ZZ2_dtz`), the `tcc_ir_opt_decrement_to_zero` shell
  (`ir/opt_loop.c`), its `ir/opt.h` decl, and the stale `redundant_init_elim`
  ordering comment removed.
- [x] **Run the full gate** (Acceptance) — `make cross`, unit tests, `make
  test-ir` green.

## Test disposition

- **`test_decrement_to_zero_*` (4 tests, `test_opt_loop.c`):** keep, retarget at
  the retained region helper / driver. They are the pure-counter, happy-path, and
  bugs.md #12 guard-coincidence locks — all still meaningful for the engine.
- **New `tests/ir_tests/` count-down pin:** added as `349_decrement_to_zero.c`,
  proving correctness across `-O0..-Os`; the `-O1+` count-down `subs; bne` latch
  was disasm-verified during Step 0.
- **Codegen `SUBS`/`CMP #0` peephole:** its *matching* logic is unchanged; its
  forward scan for the branch was extended to step over a flag-neutral identity
  move (Step 0 fix). Existing peephole tests still hold.
- **`PASS_COVERAGE.md`:** add an `ssa:decrement_to_zero` row.

## Deferred: broaden to natively bottom-tested loops (out of scope)

Recorded because it is where the *larger* value is, so the narrow v1 candidate
set is understood as a deliberate parity choice, not the ceiling. A
`do { body; i++; } while (i < N)` (and any loop that reaches codegen bottom-tested
without a separate guard) is a **better** count-down target than the rotated
for-loop: no guard to NOP, and the latch `dec`+`CMP #0` are already adjacent for
the `SUBS` peephole. Legacy declines these (it *requires* a separate pre-test
guard, `test_decrement_to_zero_no_separate_pretest_guard_bails`). A v2 that
handles the guard-less bottom-tested shape — proving the trip is `>0` (or, for a
`do-while`, that it always executes once) so init `=limit` is safe without a guard
to remove — would capture the common case and is the right place to reconsider
whether this optimization earns its keep. It is a **behavior change with its own
soundness proof and tests**, not part of this migration.

## Comment policy

Per [[comments-max-one-liner]] and the sibling plans: any legacy comment in
*touched* code is deleted or compressed to a single line; no comment blocks are
added. The bugs.md #12 rationale and the "must run after IV-SR /
`redundant_init_elim`" narratives live here and in the unit-test headers, not in
new source.

## Acceptance

- [x] Step-0 recorded: legacy pass fires **0×**; SSA pass fires on `purestore`;
  the SUBS win was blocked by the out-of-SSA phi copy and unblocked via the
  identity-move scan (`subs; bne`, −1 insn/iter). See "Step 0 results."
- [x] New `tests/ir_tests/349_decrement_to_zero.c` passes at `-O0/-O1/-O2/-Os`
  (count-down `subs; bne` latch disasm-verified at `-O1+`); the four
  `test_decrement_to_zero_*` unit tests pass against the SSA driver + engine.
- [x] `TCC_DISABLE_PASS=ssa:decrement_to_zero` cleanly reverts to the count-up
  form; `-dump-ir-passes=ssa:decrement_to_zero` emits.
- [x] `make cross -j$(nproc)`.
- [x] `make test-ir` (IR suite) green; unit suite green (`run_unit_tests`, 0
  failed).  (`make test` blocked by a pre-existing unrelated sccp UT fail.)
- [x] `python3 scripts/diff_olevels.py --seeds 0-3000 --require-qemu` — **zero new
  divergences** (`checked=3001 divergences=0` at -O0/-O1/-O2); the codegen
  identity-move scan change is non-divergent corpus-wide.  (0-3000 run; extend to
  0-5000 for a fuller sweep if desired.)
- [x] Legacy `tccgen.c` call site + inert driver shell removed; parent tracker
  `decrement_to_zero` checkbox ticked (**"replaced by `ssa:decrement_to_zero`"**),
  this file linked under "Detailed Pass Plans."

## Assumptions

- Loop rotation runs at regalloc time (`ssa:loop_rotate`, `optimize>=1`) and
  produces the separate-guard + bottom-tested shape for register-only bodies;
  `decrement_to_zero` is a strict consumer of that shape.
- The `SUBS`/`CMP #0` codegen peephole (`ir/codegen.c`) is sound and is the sole
  beneficiary of the rewrite; it fires on the bottom-tested latch the rewrite
  creates, scanning to the branch over the flag-neutral out-of-SSA identity-move
  phi copy (Step 0 extension).
- IV strength reduction stays in tccgen (upstream of regalloc) for this
  migration; its "counter is a pure counter after IV-SR" guarantee holds when
  `decrement_to_zero` runs at regalloc time.
- The candidate set is narrow but non-empty (`purestore`-class loops); the
  restore-vs-retire decision is gated on the Step-0 measurement, with **restore**
  recommended for behavior parity and the confirmed win.

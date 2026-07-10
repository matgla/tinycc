# Plan: Migrate `loop_bound_remat` off the legacy pre-SSA tail

**Status:** **RETIRED 2026-07-07** (v1a observability proved the legacy pass
inert at its tccgen site; driver + call site + tests removed, `216` pin kept, the
post-SSA `ra:bound_remat` revival design below preserved) · **Created:** 2026-07-06

> **Retirement note.** The plan below was written for a two-step migration
> (v1 in-place observability → Deferred-v2 post-SSA relocation).  In practice v1a
> observability made the pass measurable and immediately proved it **inert**
> (byte-identical object output across the ir_tests corpus, 1127 fuzzer programs,
> and a 2988-object gcc c-torture ×-O1/-O2 sample — 0 firings), the same result
> that retired `dead_loop_elim` and `loop_guard_elim`.  So the pass was **retired
> outright** rather than relocated: the `tcc_ir_opt_loop_bound_remat` driver
> (`ir/opt_loop.c`), its `ir/opt.h` prototype, the tccgen call site + v1a
> disable/dump wiring, the `triage_olevels.sh` knob, and the four
> `test_loop_bound_remat_*` unit tests are deleted; `216_fuzz_loop_bound_remat_value_load.c`
> is kept as an anti-reintroduction pin.  The "Deferred v2 / `ra:bound_remat`"
> section is retained below as the design of record should a future shape make the
> optimization worth reviving in the post-SSA regalloc region (GCC performs it).
> Everything from "## Why this pass is different" onward describes the *pre-retirement*
> state and rationale; read it as history.

Parent tracker: [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md).
Sibling precedents:
[`plan_legacy_loop_ptr_iv_exit_subst_ssa.md`](plan_legacy_loop_ptr_iv_exit_subst_ssa.md)
(member-span synthetic `IRLoop` with `body_instrs = NULL`, single-entry facts,
process-one-then-rebuild rule — the closest structural cousin, since this pass
also never dereferences `body_instrs`),
[`plan_legacy_loop_unroll_ssa.md`](plan_legacy_loop_unroll_ssa.md)
(the `insert_instr_at` IR-growth precedent — `loop_bound_remat` also grows the
flat IR), and above all
[`plan_legacy_loop_iv_strength_reduction_ssa.md`](plan_legacy_loop_iv_strength_reduction_ssa.md)
(**the direct parent** — `loop_bound_remat` is functionally IV-SR's tail, shares
its `opt_iv_strength_red` gate, and consumes the SP-relative end pointer IV-SR
hoists into the preheader; read its "Why this pass is different" and its
Deferred-v2 cluster section, which already names this pass).

## Why this pass is different (read first)

The four landed siblings (`ssa:loop_rotate`, `ssa:first_iter_exit`,
`ssa:ptr_iv_exit_subst`, `ssa:loop_const_sim`) are **correctness / loop-shape**
transforms: they change control flow or fold values, and their output is
*consumed* by SSA construction and the SSA optimizer downstream. They relocated
cleanly to the **pre-SSA** flat region at the top of `ir/regalloc.c` precisely
because SSA/regalloc *wants* to see their result.

`loop_bound_remat` is categorically different. It is a **register-pressure /
live-range** peephole. It does not change what the program computes; it
*lengthens the instruction stream on purpose* — inserting a redundant
`ASSIGN t = Addr[StackLoc[off]]` before each in-loop `CMP` — so that the end
pointer's live range no longer crosses the loop's calls. A shorter live range
lets the linear-scan allocator keep the end pointer in a caller-saved register
(R0-R3) or scratch instead of a callee-saved one (R4-R11), dropping a PUSH/POP
pair. GCC does exactly this (`ADD r3, sp, #off` inside the loop). Its value is
realized **only by the register allocator**, not by any other optimization.

Two consequences drive this whole plan:

1. **Its output must reach interval construction un-merged.** A value-numbering
   or load-CSE pass that re-commons the N identical `Addr[StackLoc[off]]`
   computations back into a single dominating def would *re-create* the long
   live range and undo the pass. So `loop_bound_remat` must run at a point after
   which nothing re-hoists/re-commons its remat sites and before
   `ra_build_call_prefix` / `ra_build_intervals` compute the live ranges the
   allocator sees. This is the opposite constraint from the four siblings (whose
   output is *meant* to be reprocessed).
2. **It is the tail of the IV-SR cluster.** It reads the hoisted end pointer
   IV-SR produces (`ASSIGN end, Addr[StackLoc]` [+ `ADD end, end, #imm`]) and is
   gated on the same `opt_iv_strength_red` flag; it also must run *after*
   `loop_postinc_fusion` (which needs the latch ADD intact). The IV-SR plan
   keeps IV-SR **in place at tccgen** for v1 (downstream-order inversion) and
   explicitly defers the cluster relocation. `loop_bound_remat` sits at the
   *end* of that cluster (only `decrement_to_zero` / `dead_loop_elim` run after
   it, and neither consumes its output), so it is *more* independently
   relocatable than IV-SR — but the correct destination is **not** the pre-SSA
   sibling region; it is the **post-SSA flat peephole region** where the other
   register-pressure peepholes already live (`ra_fold_phi_const_chain`,
   `ra_fold_const_branches`, `const_memcpy_to_dest`, `switch_to_data`).

Like IV-SR — and unlike `ptr_iv_exit_subst` — its recorded fuzz history is a
**soundness guard**, not a false-loop class: fuzz seed 6214 / test 216, the
`is_lval` value-load guard. CFG-verified candidate detection (which fixes the
false-loop / external-entry class) therefore buys little here, while the
mutation machinery it must not disturb (flat-range scan + `insert_instr_at`
growth) is exactly the parent tracker's designated high-risk area. That inverts
the cost/benefit versus the shape passes and argues for the same conservative,
in-place-first migration IV-SR took.

## Intent

Preserve `loop_bound_remat`'s behavior **exactly** while (a) giving it the
observability the tracker wants — it currently has **none** (no dump name, no
`TCC_DISABLE_PASS` name, no `-f` flag of its own; it is bisection-conflated with
IV-SR under the shared `opt_iv_strength_red` gate) — and (b) recording the path
to its correct architectural home (the post-SSA regalloc peephole region) as a
gated, deferred step.

The behavior to preserve — the legacy driver, per detected loop that contains a
call, rematerializes an IV-SR-hoisted SP-relative end pointer inside the loop:

1. **Call gate:** skip any loop with no `FUNCCALLVAL`/`FUNCCALLVOID` in
   `[start_idx, end_idx]` (without a call the end pointer can live in a
   caller-saved register anyway — nothing to gain).
2. **Preheader collection:** starting from `loop->preheader_idx`, expand
   backwards to the basic-block head (stop at `is_jump_target`, at
   `JUMP`/`JUMPIF`/`IJUMP`/`RETURNVALUE`, and at calls), then collect up to
   `REMAT_MAX_CANDIDATES = 8` TEMP vregs defined by `ASSIGN t = STACKOFF`,
   optionally immediately followed by `ADD t, t, #imm`.
3. **Address-of-only guard (the seed-6214 invariant):** accept a candidate only
   when its `STACKOFF` source is `is_lval == 0` (an *address* computation,
   `Addr[StackLoc]`). A value load (`is_lval == 1`, e.g. a named local VAR read)
   is memory whose content need not match a fresh anonymous-slot load;
   rematerializing it as a raw `StackLoc[off]` load reads uninitialized stack
   (test 216).
4. **Use validation:** every use of the candidate vreg anywhere in the function
   must be a `CMP` (no `is_lval`/deref use — redirecting a `*t` operand to a
   remat vreg would silently drop the load and compare the *address*), and every
   use must fall inside `[preheader_start, end_idx + 2]`. Any other use, any
   deref use, or a use outside the range disqualifies the candidate.
5. **Rematerialize:** for each recorded CMP site (processed last-to-first so
   indices stay valid), `insert_instr_at` a fresh
   `ASSIGN remat_t = Addr[StackLoc[stack_off + add_imm]]` (fresh TEMP per site,
   `is_param` carried through), redirect the CMP operand from the candidate vreg
   to `remat_t`, then NOP the original preheader `ASSIGN` (and the optional
   `ADD`).

Behavior intentionally **not** changed in v1 / explicit non-goals:

- **No relocation in v1.** The pass stays at tccgen (the current call site and
  ordering — after `loop_postinc_fusion`, before `decrement_to_zero`). Moving it
  to the post-SSA regalloc home is the *correct* end state but is deferred (see
  "Deferred v2"), gated on Step 0 proving the hoisted-end-pointer pattern
  survives IV-SR→SSA→de-SSA recognizably and that the regalloc win is preserved.
- **No retirement of the legacy driver in this task.** Like IV-SR v1, this is an
  in-place hardening, not a replace-and-delete. The "SSA" driver *is* the legacy
  driver with observability wired in.
- **No SSA-native rematerialization**, no widening of `REMAT_MAX_CANDIDATES`
  (8), the 4-CMP-site cap, the `[end_idx + 2]` window, or the accepted operand
  shapes; no new candidate forms (still only `Addr[StackLoc]` [+ `ADD #imm`]).
- **No touching the mutation path.** The `insert_instr_at` insertion, the CMP
  redirect, and the preheader NOP are frozen. v1 may change only *how the pass
  is named/observed* and — under Step-0 outcome (b) only — *reject* candidates
  via a CFG validation gate; never how a candidate is rewritten.
- **No decoupling of the shared `opt_iv_strength_red` gate.** v1 adds a separate
  `TCC_DISABLE_PASS` name (so bisection can isolate remat from IV-SR) but leaves
  the `-fno-iv-strength-red` `-f` flag gating both, matching the IV-SR plan's
  "gate split happens when the cluster relocates."

## Current Legacy Shape

- **Call site:** single site in the late `tccgen.c` optimization sequence
  (`tccgen.c:30464-30468` at time of writing), gated
  `tcc_state->opt_iv_strength_red`, positioned deliberately **after**
  `tcc_ir_opt_loop_postinc_fusion` (`tccgen.c:30461`, "Must run AFTER
  loop_postinc_fusion to avoid breaking the latch ADD pattern") and **before**
  `tcc_ir_opt_decrement_to_zero` (`tccgen.c:30473`). No post-pass cleanup
  cascade at the site — the value is realized downstream by the register
  allocator, not by a fold.
- **Gate / flag:** `tcc_state->opt_iv_strength_red`, enabled at `-O1+`
  (`libtcc.c:2324`). The pass has **no `-f` flag of its own**; `-fiv-strength-red`
  / `-fno-iv-strength-red` gates it *and* IV-SR together. This shared gate is the
  bisection problem v1 fixes.
- **Observability today: none.** Unlike IV-SR (which at least has a
  `CONFIG_TCC_DEBUG`-only `ZZ_iv_strength_red` dump), `loop_bound_remat` has *no*
  `dump_ir_after_pass`, *no* `tcc_ir_opt_pass_disabled` registration, and *no*
  `-dump-ir-passes` name. It cannot be isolated or dumped at all except by
  `-fno-iv-strength-red` (which also kills IV-SR). Closing this gap is the
  primary concrete deliverable of v1.
- **Driver:** `tcc_ir_opt_loop_bound_remat` (`ir/opt_loop.c:260`, prototype
  `ir/opt.h:804`). Self-detecting: calls `tcc_ir_detect_loops` /
  `tcc_ir_free_loops` itself (does **not** take LICM's loop set — unlike IV-SR's
  `_with_loops` path). Single flat pass over `loops->loops[]`, no re-detection
  loop.
- **`body_instrs` is NOT used (key structural fact).** The driver reads only
  `loop->start_idx`, `loop->end_idx`, `loop->header_idx`, and
  `loop->preheader_idx` — never `loop->body_instrs` / `num_body_instrs`. This
  matches `ptr_iv_exit_subst`'s core (`body_instrs = NULL` synthetic loop) and
  is the opposite of IV-SR (whose `find_derived_ivs`/escape-scan/`APPLY_SHIFT`
  require a populated `body_instrs`). A CFG-detection variant here therefore
  never has to reconstruct `body_instrs` — the expensive, fuzz-fragile half of
  the IV-SR relocation is simply absent.
- **IR growth (the frozen high-risk part).** Rematerialization calls
  `insert_instr_at` once per CMP site, tracking `remat_shift` and refreshing
  `n = ir->next_instruction_index`; CMP sites are processed last-to-first so
  earlier indices stay valid. This is the parent tracker's designated "direct
  instruction insertion" area and is frozen in v1. Note it does **not** patch
  sibling `IRLoop` records the way `try_unroll_loop_ex` does — which is itself a
  latent fragility: after the pass inserts instructions while processing
  `loops->loops[li]`, the cached `start_idx`/`end_idx`/`header_idx`/
  `preheader_idx` of `loops->loops[li+1..]` are stale. Because the use-validation
  scan is conservative (any candidate use outside the — possibly stale — window
  or that is not a clean `CMP` sets `bad_use` and declines), stale indices
  degrade to *missed* rematerializations, not miscompiles; but it is exactly the
  flat-range-staleness class CFG-per-loop-rebuild removes structurally.
- **Shared helpers consumed (all remain shared):** `tcc_ir_detect_loops` /
  `tcc_ir_free_loops` (`ir/licm.c`), `insert_instr_at`
  (`ir/opt_loop_utils.c`), `tcc_ir_vreg_alloc_temp`, and the `irop_*` operand
  accessors. There is **no mutator core to extract** — the whole pass is the
  driver in `ir/opt_loop.c`; a v1 candidate front end wraps it, it is not
  reimplemented.
- **Existing unit tests** (`tests/unit/arm/armv8m/test_opt_loop.c`, three
  driver-level tests, `UT_COVERS("loop_bound_remat")` at `:1380`):
  - `test_loop_bound_remat_no_calls_in_loop_no_change` (`:256`) — the call gate:
    a callless loop returns 0, IR untouched.
  - `test_loop_bound_remat_hoisted_end_ptr_with_call_rematerializes` (`:278`) —
    the happy path: `T0 = Addr[StackLoc[64]]` used only in a header CMP, loop
    body has a call → one change; original def NOP'd; a fresh TEMP ASSIGN reading
    offset 64 inserted before the (shifted) CMP; CMP src2 redirected to it.
  - `test_loop_bound_remat_value_load_not_rematerialized` (`:345`) — the
    seed-6214 guard: `is_lval == 1` source → 0 changes, untouched.
- **Existing IR regression** (in `TEST_FILES`, `tests/ir_tests/test_qemu.py`):
  `216_fuzz_loop_bound_remat_value_load.c` — the seed-6214 reduction; the
  `is_lval` value-load must not be rematerialized (checksum corruption at
  `-O1/-O2` otherwise). This is the acceptance pin and must stay green
  throughout.
- **Known fuzz history tied to this pass:** fuzz seed 6214 / test 216 (the
  `is_lval` value-load guard). That is the *only* recorded divergence for this
  pass, and it is a soundness guard on *which* candidates qualify, not on *which
  loops* are found — reinforcing that the pass's correctness lives in the
  candidate/use validation, not in loop-range discovery. **v1 must not perturb
  the `is_lval`/deref validation.**

## Step 0: Placement / GVN-survival experiment (decision gate)

Run before writing code; record the matrix here. Unlike the shape siblings,
Step 0's decisive question is *not* "does the pattern survive to regalloc time"
for correctness — it is **"where can this register-pressure peephole run such
that (i) its remat sites are not re-commoned before interval construction and
(ii) it still sees a recognizable IV-SR-hoisted end pointer, and does moving it
there preserve the regalloc win?"** Probes on the motivating shape (a counted
array loop whose body calls a helper, so IV-SR hoists an SP-relative end pointer
that is live across the call — the seed-6214 skeleton without the value-load
bug, plus a `short[]` variant and a two-end-pointer struct-field loop),
compiled `-O1`/`-O2`, disassembly compared:

1. **Does the SSA optimizer already undo the *current* placement?** The pass runs
   at tccgen, i.e. **pre-SSA**; SSA is then constructed and `ssa:load_cse` /
   `ssa:cprop` / `ssa:gvn` run (`ir/regalloc.c:4635-4642`). Confirm on the
   motivating shapes that these do **not** re-hoist/re-common the remat
   `Addr[StackLoc[off]]` sites back into the preheader (they should not — there
   is no LICM/PRE in the SSA opt list to hoist out of the loop, and GVN commons
   within available-expression scope, not across the loop back-edge). Dump
   `-dump-ir-passes=ssa:gvn` and check the remat ASSIGNs still sit inside the
   loop and the end pointer still gets a caller-saved register at
   `ra_linear_scan`. **This establishes the invariant any new placement must
   also satisfy**, and confirms today's behavior is real (not accidentally
   already-broken).
2. **Relocatability to the post-SSA peephole home.** Compile out the tccgen
   call and wire the *existing* `tcc_ir_opt_loop_bound_remat` into
   `ir/regalloc.c` in the **post-SSA flat region** — after `const_memcpy_to_dest`
   (`:4783`) / `ra_fold_const_branches`, **before** `ra_build_call_prefix`
   (`:4786`). At that point the IR is de-SSA'd flat form again (its natural
   shape) and no CSE runs afterward. Two things to verify:
   - **Pattern recognizability.** Does the IV-SR-hoisted end pointer still appear
     as `ASSIGN t = Addr[StackLoc[off]]` [+ `ADD`] in a scannable preheader after
     SSA construct/rename/opt and `ra_resolve_phis`? SSA rename renames TEMPs and
     inserts phi-copies at block ends; the "expand preheader backwards" heuristic
     (which stops at jump targets/control flow) may see a different layout.
     Record whether the flat scan still finds the candidate, or whether a CFG
     preheader-block lookup is needed.
   - **Regalloc win preserved.** Compare `ra_linear_scan`'s allocation and the
     final PUSH/POP count vs. baseline (tccgen placement). The whole point is the
     end pointer landing in a caller-saved reg; if the relocated run produces the
     same (or better) allocation with no correctness change, relocation is
     viable. If the post-SSA layout defeats recognition or the win regresses,
     relocation stays deferred.
3. **Independence from the rest of the IV-SR cluster.** Confirm nothing between
   the tccgen call site and the post-SSA home *consumes* remat's output:
   `decrement_to_zero` and `dead_loop_elim` (the only tccgen passes after it) key
   on loop counters / dead loops, not on the end pointer's live range. If
   confirmed, `loop_bound_remat` can relocate **independently of IV-SR** (it is
   the cluster tail), which is the one place it is *less* coupled than IV-SR
   suggests. Record any surprise consumer.
4. **CFG false-loop exposure (decides whether outcome (b) happens).** Enumerate
   the loops `tcc_ir_detect_loops` hands this pass on the corpus and ask: are any
   *false loops* / external-entry shapes a dominance-verified detector would
   reject (seed-278 class)? Expected "few or none" (the fuzz history is the
   `is_lval` guard, not false loops). If zero exposure, the CFG-detection variant
   is not worth the (small, since no `body_instrs`) risk and is dropped.
5. **Observability-only sufficiency.** Confirm that a `TCC_DISABLE_PASS` name +
   `-dump-ir-passes` integration (v1a) delivers the tracker's asks: the disable
   name restores the un-rematerialized loop (end pointer back in a callee-saved
   reg) on the probe, and `-fno-iv-strength-red` still gates both this pass and
   IV-SR as today.

Outcomes:

- **(a)** probe 1 confirms today's pre-SSA placement is not undone, probe 4 shows
  no false-loop exposure → **v1 = observability-only, in place** (register the
  disable/dump name; no detector change; no relocation; no retirement).
  Recommended default, mirroring IV-SR v1a.
- **(b)** probe 4 shows real false-loop / external-entry exposure → **v1 =
  observability + a CFG *validation* gate** (build a throwaway CFG, decline any
  detected loop that is not a dominance-verified single-entry natural loop; free
  the CFG before any mutation; keep `tcc_ir_detect_loops` ranges otherwise; since
  `body_instrs` is unused, nothing is reconstructed). Still in place, still no
  retirement.
- **(c)** probe 2 shows the pattern is recognizable at the post-SSA home *and*
  the regalloc win is preserved, and probe 3 confirms cluster-independence →
  **relocation to the post-SSA regalloc peephole region is viable now** and the
  "Deferred v2" section can be promoted to the primary path (with full
  retirement of the tccgen call site and a gate split). This is the aspirational
  end state; take it only if all three conditions hold cleanly, else default to
  (a)/(b).

Record the shape-by-shape matrix here before implementation starts.

### Step 0 Results

**Decision: outcome (a) — observability-only, in place.** Landed 2026-07-07.

Empirical firing survey (enabled vs. `TCC_DISABLE_PASS=loop:bound_remat`, object
output compared byte-for-byte):

| Corpus | Levels | Pairs | Fired |
|---|---|---|---|
| `tests/ir_tests/*.c` (single-file `-c`) | -O1, -O2 | 54 | **0** |
| Fuzzer programs, profiles int/ptr/signed/longlong/struct_byval/combo_num/agg_deep, seeds 6100-6260 (newlib include path) | -O2 | 1127 | **0** |
| gcc c-torture `execute/*.c` (compile-only) | -O1, -O2 | 2988 | **0** |

Direct IR inspection of the motivating shape (a counted loop over a local
`int a[32]` whose body calls a helper) confirms *why*: at the current tccgen
ordering, IV-SR's `try_eliminate_iv_counter` does **not** produce the
hoisted SP-relative end pointer this pass consumes — it leaves the counter IV
live and lowers the element access to `LOAD_INDEXED` with the counter as index
(`T12 <- Addr[StackLoc[-128]] LOAD_INDEXED V2`, `CMP V2,#32`). With no
`ASSIGN t = Addr[StackLoc]`-used-only-in-CMP candidate in any scanned preheader,
the pass declines every loop. The `-dump-ir-passes=loop:bound_remat` before/after
dump on this shape is byte-identical.

This is the **same inertness pattern that retired `dead_loop_elim` and
`loop_guard_elim`**: rotation / `loop_const_sim` / `loop_unroll` relocated to the
regalloc-time flat region, changing the shapes this pre-SSA tail sees, so the
legacy pass now runs but never fires. The recorded fuzz history (seed 6214 /
test 216) is the *is_lval decline* case — the pass correctly does nothing there —
so it is not evidence of live firing today either.

Consequence for the roadmap: the survey (≈4169 programs, incl. a 2988-object gcc
c-torture ×-O1/-O2 sample — exceeding the 2572-object bar used to retire
`loop_guard_elim`) is decisive evidence that `loop_bound_remat` is inert at its
tccgen site. **Decision (2026-07-07): retire outright**, the
`dead_loop_elim`/`guard_elim` outcome, rather than pursue the Deferred-v2
relocation. Deleting a provably byte-for-byte no-op pass changes no codegen; the
v1a observability wiring is what made the no-op *provable* (via
`TCC_DISABLE_PASS=loop:bound_remat`), so it did its job and was removed together
with the driver. The `ra:bound_remat` post-SSA design is kept below as the
revival path if a future shape makes the optimization worthwhile.

**Retirement executed:** deleted the `tcc_ir_opt_loop_bound_remat` driver +
`REMAT_MAX_CANDIDATES` (`ir/opt_loop.c`), the `ir/opt.h` prototype, the tccgen
call site + the v1a disable/dump wiring, the `triage_olevels.sh` knob, and the
four `test_loop_bound_remat_*` unit tests (+ the now-dead pass-only
`utb_init_temp_intervals`/`VR_TEMP` test helpers and the `opt_utils.h` include).
No `-f` flag/field to remove (the pass shared IV-SR's `opt_iv_strength_red`, which
IV-SR keeps). `216_fuzz_loop_bound_remat_value_load.c` is kept green as an
anti-reintroduction pin (its header updated to note the retirement).

**v1a (the intermediate observability step, since superseded by the retirement
above — the wiring below was added, used to prove inertness, then removed):**

- Call-site disable guard + net-new dump in `tccgen.c` (`loop:bound_remat`;
  `loop:` namespace since it runs on flat pre-SSA IR, not SSA form — reserving
  `ra:bound_remat` for the Deferred-v2 post-SSA home). The frozen mutation body
  in `ir/opt_loop.c` is untouched.
- `ENV:TCC_DISABLE_PASS=loop:bound_remat` added to the `tests/fuzz/triage_olevels.sh`
  curated culprit-knob list (the plan named `scripts/bisect_opt.py`, but that
  script derives knobs from `-f` flags / the pipeline table and has no pass-name
  list; `triage_olevels.sh`'s `KNOBS` array — where `ssa:loop_rotate` already
  lives — is the real realization of "attribute separately during bisection").
  This is net-new triage coverage: `-fiv-strength-red` (the shared gate) is not
  in `KNOBS`, so IV-SR/bound_remat were previously unattributable there.
- Observability unit test `test_loop_bound_remat_disable_knob_wired`
  (`tests/unit/arm/armv8m/test_opt_loop.c`): pins the guard-honoring wiring under
  the CI default (`TCC_DISABLE_PASS` unset → pass fires, change 1, preheader def
  NOP'd). The disabled path is not force-testable in the shared UT binary
  (`tcc_ir_opt_pass_disabled` memoizes the env process-wide; see
  `test_opt_utils.c`), mirroring `test_pass_disabled_unset_env_never_disables`.
- New IR pin declined (outcome (a)): the pass is behavior-preserving and the
  existing `216_fuzz_loop_bound_remat_value_load.c` suffices, matching the IV-SR
  plan's "110/258 may suffice" call. (The plan's suggested number 348 is also now
  taken by `dead_loop_elim`; 349 by `decrement_to_zero`.)

## Replacement Design (in-place v1)

(Assumes Step 0 outcome (a) or (b); under (c) the design is replaced by the
"Deferred v2" relocation variant and re-scoped.)

### Placement: unchanged — tccgen, after `loop_postinc_fusion`

The call site and ordering stay exactly as today (`tccgen.c:30464-30468`), after
`loop_postinc_fusion` and before `decrement_to_zero`. This preserves the "runs
after the latch ADD is fused, before decrement rewrites counters" invariant and
keeps the pass pre-SSA (probe 1's confirmed-safe position). No code moves to
`ir/regalloc.c` in v1.

### Naming, observability, disable knob (the primary v1a deliverable)

- Register the pass with `tcc_ir_opt_pass_disabled(<name>)` at the call site so
  `TCC_DISABLE_PASS=<name>` isolates `loop_bound_remat` **independently of
  IV-SR** (today nothing can). Add a `tcc_ir_dump_after_pass(ir, <name>)` right
  after it so `-dump-ir-passes=<name>` works — the pass has *no* dump today, so
  this is net-new observability, not a `ZZ_`-name replacement.
- **Name decision (maintainer call, flag in Step 0):** the four migrated siblings
  use `ssa:` to mean "runs in the regalloc-time flat region." An in-place tccgen
  pass is neither SSA-form nor regalloc-time, so `ssa:bound_remat` would mislead
  exactly as the IV-SR plan noted for `ssa:iv_strength_reduction`. Recommended v1
  name: **`loop:bound_remat`**, matching the IV-SR plan's proposed `loop:`
  namespace for in-place tccgen loop passes. Reserve **`ra:bound_remat`** (an
  `ra:` namespace, honest for the post-SSA register-pressure region where
  `ra_fold_*` peepholes already live) for the day it relocates (Deferred v2) —
  `ssa:` would be doubly wrong for a pass that runs *after* the SSA optimizer on
  de-SSA'd IR. State the choice in the call-site comment.
- Keep the `-fiv-strength-red` / `opt_iv_strength_red` gate exactly (still gates
  both this pass and IV-SR; unchanged).
- Add `<name>` to `scripts/bisect_opt.py`'s knob list so `loop_bound_remat` and
  IV-SR attribute separately during bisection (today the shared `-f` flag
  conflates them — this is the same bisection-resolution win the IV-SR plan
  cites).

### Comment policy (applies to every code change in this migration)

Per [[comments-max-one-liner]] and the sibling plans: new code (the disable/dump
wiring, any validation gate, unit tests) carries **no comment blocks** — at most
a single-line comment where the code cannot express the constraint. Any legacy
comment in *touched* code is deleted or compressed to one line. The
seed-6214/regalloc-cost narrative lives in this plan and the pinning tests, not
in source. The frozen mutation body in `ir/opt_loop.c` is heavily commented;
leave it untouched by the observability-only v1 so its comments are neither moved
nor preserved into new code.

### Candidate detection

- **Outcome (a): no change.** Keep `tcc_ir_detect_loops` + the manual backward
  preheader expansion + the use-validation scan exactly. The migration is
  naming/observability only. Lowest-risk v1, recommended.
- **Outcome (b): validation gate only.** Before the per-loop work, build a
  throwaway `tcc_ir_cfg_build` + `tcc_ir_cfg_compute_dominators` (confirm it is
  callable at tccgen time — the flat IR fully exists there; if not, fall back to
  (a)) and **decline** any `tcc_ir_detect_loops` loop whose header does not
  dominate its latch or whose header has an out-of-loop predecessor other than
  the preheader (the single-entry fact). Free the CFG before any mutation. Since
  `loop->body_instrs` is unused by this pass, the gate only *rejects* loops — it
  never constructs the range the mutator scans, so the fuzz-fragile
  `body_instrs`-reconstruction risk that dogs IV-SR is absent here. Share
  `fie_collect_members` (`ir/opt/ssa_opt_loop.c`) for the membership walk rather
  than copying it.

### Mutation strategy and staleness — unchanged

The `insert_instr_at` last-to-first rematerialization, the CMP-operand redirect,
and the preheader NOP are preserved verbatim. A validation gate (outcome b) only
prunes the candidate loop list *before* any insertion; once a loop is accepted
nothing in the mutation path changes, and the CFG is freed before the first
mutation. The one existing flat-range-staleness fragility (stale
`loops->loops[li+1..]` indices after an insertion, "Current Legacy Shape") is
**not** fixed in v1 (it degrades to a missed opt, never a miscompile); a
process-one-then-rebuild loop would fix it and is folded into the Deferred-v2
relocation, not bolted onto the frozen in-place body.

### Cleanup cascade — unchanged

None exists and none is added. The value is realized by the register allocator
downstream; there is nothing to fold after a firing.

### Interaction with other passes

- **IV-SR (upstream, the direct parent, still legacy in tccgen):** unchanged
  provider of the hoisted end pointer. This pass reads what IV-SR's
  `try_eliminate_iv_counter` cost heuristic left in the preheader. The IV-SR plan
  notes the shared `opt_iv_strength_red` gate is split "when the cluster
  relocates"; v1 of *both* passes keeps the shared gate, adding only per-pass
  `TCC_DISABLE_PASS` names.
- **`loop_postinc_fusion` (upstream, still legacy):** unchanged; the "remat runs
  after postinc fusion" ordering is preserved by keeping the call site in place.
- **`decrement_to_zero` / `dead_loop_elim` (downstream, still legacy):**
  unchanged; neither consumes remat's output (probe 3), which is why remat is the
  cluster tail and — under outcome (c) — independently relocatable.
- **The migrated regalloc-time siblings (`ssa:loop_rotate` /
  `ssa:first_iter_exit` / `ssa:ptr_iv_exit_subst` / `ssa:loop_const_sim`):** all
  run *after* tccgen, hence after this pass. They reshape / fold / substitute on
  loop *shapes*; remat rewrote an end-pointer *live range*. Confirm on
  `345_ptr_iv_exit_subst.c` (trip >16, survives unrolling) that remat's in-place
  output and the siblings' rewrites do not collide (different vregs, different
  concern).
- **The SSA optimizer (`ssa:load_cse` / `ssa:cprop` / `ssa:gvn`, runs after
  tccgen):** the load-bearing interaction — probe 1. It must **not** re-common
  the remat sites into a single hoisted def. v1 keeps the pass pre-SSA exactly as
  today, so this interaction is unchanged and already validated by the shipping
  behavior; it becomes a *design constraint* only for the Deferred-v2 relocation
  (which sidesteps it by running post-SSA-opt).

## Required Unit Tests

v1 adds no new mutation surface, so the three existing driver tests
(`test_loop_bound_remat_*`) cover the transform and stay green. Add, in
`tests/unit/arm/armv8m/test_opt_loop.c` alongside them:

- **observability (v1a):** a driver test asserting that with the pass disabled
  (via a helper honoring `tcc_ir_opt_pass_disabled(<name>)`, mirroring the
  sibling wiring) the hoisted-end-pointer loop is left un-rematerialized (change
  count 0, preheader ASSIGN intact, CMP unchanged), and with it enabled the
  rematerialization fires (change count 1, preheader def NOP'd, fresh remat
  ASSIGN before the CMP). This pins the disable knob — the concrete v1
  deliverable.
- **validation gate (v1b, only if outcome b):** a synthetic false-loop shape (a
  backward jump that is not a dominance-verified natural loop, seed-278 class)
  and a side-entry-into-a-non-header-member shape are both declined by the gate →
  `tcc_ir_opt_loop_bound_remat` returns 0; a genuine natural loop with a hoisted
  end pointer still rematerializes (the gate must not over-reject).
- **regression pins (no behavior change expected):** re-assert the seed-6214
  `is_lval` decline (`test_loop_bound_remat_value_load_not_rematerialized` stays
  green) and the call gate (`test_loop_bound_remat_no_calls_in_loop_no_change`
  stays green) after the front-end wiring changes.

Refresh the `test_opt_loop` row in `tests/unit/PASS_COVERAGE.md` if the suite
grows.

## IR Regression Test

The existing `216_fuzz_loop_bound_remat_value_load.c` is the acceptance corpus
and must stay byte-behavior-identical across v1 (v1 is behavior-preserving). Add
a new pin only if v1b changes any candidate outcome, or to pin the observability
wiring end-to-end:

`tests/ir_tests/348_loop_bound_remat_ssa.c` + `.expect` (next free number — 346
is reserved by the `loop_unroll` plan and 347 by the `iv_strength_reduction`
plan; register in `TEST_FILES` in `tests/ir_tests/test_qemu.py`):

- a counted array loop whose body calls a helper (so IV-SR hoists an SP-relative
  end pointer live across the call), producing a correct runtime result — the
  positive shape whose codegen the remat improves, to pin the observability
  wiring end-to-end (`TCC_DISABLE_PASS=<name>` must still be correct, only
  larger);
- a two-end-pointer variant (`REMAT_MAX_CANDIDATES` exercise) whose result must
  be unchanged from `-O0`.

Added **first**, green under the legacy pass at `-O0/-O1/-O2/-Os`. If Step 0
lands on outcome (a) (observability only), the new IR test is optional and the
existing 216 pin suffices — record the decision (mirroring the IV-SR plan's
"110/258 may suffice").

## Migration Steps

Unlike the four shape siblings, **v1 does not retire the legacy driver** (see
Intent) — it is an in-place hardening, so the "delete legacy code" step is
absent by design and deferred to v2.

- [ ] Run Step 0; fill in the matrix; pick outcome (a)/(b)/(c). Steps below
  assume (a)/(b); under (c) switch to the Deferred-v2 relocation design and
  re-scope with the gate split.
- [ ] Decide the pass name (`loop:bound_remat` recommended vs. `ssa:bound_remat`);
  record in the call-site comment.
- [ ] **v1a:** register `tcc_ir_opt_pass_disabled(<name>)` at the tccgen call
  site and add `tcc_ir_dump_after_pass(ir, <name>)` after it (net-new dump); add
  `<name>` to `scripts/bisect_opt.py`. Add the observability unit test.
- [ ] **v1b (only if outcome b):** add the CFG *validation* gate in front of the
  per-loop work (decline non-natural / multi-entry candidates; free the CFG
  before mutation; share `fie_collect_members`; no `body_instrs` reconstruction).
  Add the validation unit tests. No change to the insertion/redirect/NOP path.
- [ ] Add `348_loop_bound_remat_ssa.c` (+`.expect`) if v1b changes any outcome or
  to pin observability; register in `TEST_FILES`. Otherwise rely on 216.
- [ ] Confirm observability: `-dump-ir-passes=<name>` shows the rematerialized
  IR; `TCC_DISABLE_PASS=<name>` restores the un-rematerialized loop (end pointer
  back in a callee-saved reg — check the PUSH/POP delta); `-fno-iv-strength-red`
  still gates both this pass and IV-SR (unchanged).
- [ ] Confirm 216 and the three existing driver tests are byte-behavior-identical
  (v1 is behavior-preserving).
- [ ] Local gates: unit tests, `make cross -j$(nproc)`, `make test -j16` (incl.
  GCC torture compile).

## Retirement hazard (deferred — NOT part of v1)

Retirement of the legacy `loop_bound_remat` and its removal from tccgen is a **v2
relocation effort**, not this task. Recorded so the tracker does not tick this
entry "done" after v1:

- **GVN/CSE re-commoning** — the central hazard, unique to this register-pressure
  pass. A relocated pass must run at a point after which no value-numbering /
  load-CSE re-hoists its remat sites (Step-0 probe 1/2). The post-SSA flat region
  (after `ssa:gvn`, before `ra_build_call_prefix`) is the only safe home; the
  pre-SSA sibling region is **not** (GVN runs after it). This is why
  `loop_bound_remat` does *not* relocate alongside the four siblings.
- **Post-SSA pattern recognizability** — after SSA construct/rename/opt and
  `ra_resolve_phis`, the IV-SR-hoisted end pointer may not present as a
  scannable-preheader `ASSIGN t = Addr[StackLoc]`; a CFG preheader-block lookup
  may be needed (Step-0 probe 2). Since `body_instrs` is unused, this is the only
  net-new detection code — cheaper than IV-SR's relocation.
- **Regalloc-win preservation / code size** — the pass's entire value is the
  PUSH/POP saved by shrinking the live range. Gate removal on a disassembly/
  code-size comparison (baseline vs. relocated) over the IR corpus + torture
  suite showing the caller-saved allocation is preserved. Silence is not
  acceptance.
- **Flat-range-staleness fix** — relocation is the natural moment to adopt the
  process-one-then-rebuild rule (the `insert_instr_at` growth invalidates the
  cached loop indices), closing the latent stale-index missed-opt fragility.
- **Shared gate split** — `opt_iv_strength_red` gates both this pass and IV-SR;
  splitting it is part of decoupling the cluster (coordinate with the IV-SR
  plan's Deferred v2).

## Deferred v2: relocate to the post-SSA regalloc peephole region

For completeness — the eventual end state the tracker wants, and the reason this
pass's home differs from every sibling. Once Step 0 confirms recognizability and
win-preservation (or as part of the IV-SR cluster v2), move
`loop_bound_remat` from tccgen into `ir/regalloc.c` in the **post-SSA flat
region** — after `const_memcpy_to_dest` (`:4783`), before `ra_build_call_prefix`
(`:4786`) — as **`ra:bound_remat`**, with:

- CFG-verified single-entry natural-loop candidates (no `body_instrs` to
  reconstruct);
- a CFG preheader-block lookup replacing the manual backward scan;
- process-one-then-rebuild for the `insert_instr_at` growth;
- the shared `opt_iv_strength_red` gate split into its own flag/knob;
- full retirement of the tccgen call site and the `ir/opt_loop.c` driver
  (keeping `insert_instr_at` shared).

This is explicitly **not** attempted in v1; it is named here so the sequencing is
on record and this pass's tracker checkbox is understood to remain open (v1
delivers observability + hardening, not removal). The one thing that makes it
*easier* than IV-SR's relocation — no `body_instrs`, no re-detection loop, a
single self-contained driver — is balanced by the one thing that makes it
*harder* — the GVN-survival constraint forcing a post-SSA (not pre-SSA) home.

## Acceptance

For landing v1 (legacy driver retained, in place):

- [ ] New observability (and, if outcome b, validation) unit tests pass; the
  three existing `test_loop_bound_remat_*` driver tests pass unchanged.
- [ ] `216_fuzz_loop_bound_remat_value_load.c` passes at `-O0/-O1/-O2` (and
  `-Os`), byte-behavior-identical to pre-change.
- [ ] `-dump-ir-passes=<name>` and `TCC_DISABLE_PASS=<name>` work;
  `-fno-iv-strength-red` unchanged (still gates both this pass and IV-SR).
- [ ] `make cross -j$(nproc)`; `make test -j16`; GCC torture suite.
- [ ] **Maintainer-run** (per the 2026-07-06 instruction, after implementation):
  `tests/fuzz/sweep_all_chunks.py 0 1000 --mode triage` → 0 divergent profiles;
  `python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu` → 0
  divergences; a run with `TCC_DISABLE_PASS=<name>` must match today's
  `loop_bound_remat`-off behavior on the corpus.
- [ ] Zero new divergences; any divergence triggers fixes before merge.

The parent tracker's `loop_bound_remat` checkbox stays **open** after v1
(observability + hardening only); it is ticked when the Deferred-v2 post-SSA
relocation lands and the tccgen driver is removed.

## Assumptions

- IV-SR and `loop_postinc_fusion` keep their current tccgen positions and their
  "before `loop_bound_remat`" ordering; v1 preserves this by not moving the pass.
- `tcc_ir_detect_loops` / `tcc_ir_free_loops` / `insert_instr_at` /
  `tcc_ir_vreg_alloc_temp` remain shared utilities and are not modified by v1
  (the mutation path is frozen). The pass never reads `loop->body_instrs`, so no
  reconstruction is required by any variant.
- The SSA optimizer (`ssa:load_cse`/`ssa:cprop`/`ssa:gvn`) does not re-hoist the
  remat sites out of the loop at the current pre-SSA placement (Step-0 probe 1,
  matching shipping behavior); this is a *constraint* for the Deferred-v2
  relocation, not a v1 change.
- `opt_iv_strength_red` (shared with IV-SR) continues to gate this pass; the gate
  split happens in the coordinated cluster/relocation v2.
- `tcc_ir_cfg_build` is callable at tccgen time (needed only for outcome b); if
  Step 0 shows it is not safely callable pre-regalloc, v1 falls back to
  outcome (a) (observability only) with no candidate change.

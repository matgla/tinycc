# Plan: Retire `loop_postinc_fusion` from the legacy pre-SSA tail

**Status:** DONE (Option B landed 2026-07-07) · **Created:** 2026-07-07

> **Outcome (Option B executed).** Deleted both dead producers
> (`tcc_ir_opt_loop_postinc_fusion`, non-loop `tcc_ir_opt_postinc_fusion`/`_ex`),
> the `-fpostinc-fusion` flag + `opt_postinc_fusion` field, the `opt_pipeline.c`
> `postinc` entry, both `tccgen.c` call sites, all prototypes, and the four
> producer unit tests (`test_(loop_)postinc_fusion_*`, `UT_COVERS("postinc")`).
> Kept the `LOAD_POSTINC`/`STORE_POSTINC` opcodes + ARM lowering and their
> codegen/SCCP unit tests. `make cross` clean; UT1 (2856 tests) green; UT4
> compiles clean (one **pre-existing, unrelated** failure:
> `test_tcc_set_options_o1_enables_pass_batch` asserts `opt_mla_fusion==1` at O1,
> but mla is O2-only since the O1/O2 split — fails identically at HEAD); the four
> `bug_postinc_*` IR pins pass at `-O0/-O1/-O2/-Os` (16/16). The vestigial
> INDEXED-DIV NOP-slot synthesis in `ir/opt_loop_utils.c` was left in place
> (comment reworded) to avoid perturbing a live pass's index bookkeeping.
>
> **Full IR suite:** 13495 passed / 246 skipped / 1 xfailed / **1 failed**. The
> lone failure — `test_golden_ir[ssa:dead_loop/simple]` — is a heap-buffer-overflow
> in `sccp_visit_instr` reading the `barrel_shifts` side-table (sized by
> `max_orig_index` in `tcc_ir_barrel_shift_fusion`), reproducible on a plain
> `-O2 -c` compile. It is **unrelated to this change**: `barrel_shift_fusion` is
> byte-identical to HEAD, `ssa_opt_sccp.c` is untouched, and removing the
> flag-gated-off postinc pass changes zero instructions (so `max_orig_index` and
> sccp's iteration are identical). It belongs to the **parallel, uncommitted
> `loop_unroll`/`loop_const_sim` migration** also present in the working tree
> (unrolling grows the instruction stream past `max_orig_index`) — not tracked or
> fixed here.

Parent tracker: [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md).
Sibling precedents (all *live* passes migrated by preserving behavior):
[`plan_legacy_loop_bound_remat_ssa.md`](plan_legacy_loop_bound_remat_ssa.md)
(the "this pass is special" template — but bound_remat is special because it is
**live at -O1+ and must be preserved bit-for-bit**; this pass is special for the
opposite reason), and
[`plan_legacy_loop_iv_strength_reduction_ssa.md`](plan_legacy_loop_iv_strength_reduction_ssa.md)
(the upstream provider of the latch ADD this pass consumes).

## Why this pass is different (read first)

Every sibling migrated so far — `ssa:loop_rotate`, `ssa:first_iter_exit`,
`ssa:ptr_iv_exit_subst`, `ssa:loop_const_sim`, and the in-place-hardened IV-SR /
`loop_bound_remat` — is a **live optimization enabled at -O1 or -O2**. Their
migration problem is "preserve behavior while relocating to SSA."

`loop_postinc_fusion` is not live. **It is disabled by default and has been
deliberately turned off because it is unsound.** Both postinc fusion drivers are
gated on `tcc_state->opt_postinc_fusion`, and that flag is initialized to `0`
with a rationale comment (`libtcc.c:2294-2301`):

> `s->opt_postinc_fusion = 0;` — *fusing LOAD/STORE + ADD into a single
> LOAD_POSTINC/STORE_POSTINC is unsound when the pointer SPILLS — the ARM
> post-indexed writeback (`ldr/str [rN],#imm`) updates rN in place but the IR
> can't model it, so the spilled base never advances (tcc froze in parse_number
> on every integer literal). Without the fusion `*p++` lowers to an explicit
> LOAD + ADD whose result is written back correctly.*

So the pass **never runs** in any default `-O0/-O1/-O2/-Os` build. It is dead
code reachable only by explicitly passing `-fpostinc-fusion`, which re-opens a
known self-hosting-breaking miscompile.

This places the pass squarely in the parent tracker's third disposition —
**"proven unnecessary"** — rather than the first two ("functionally equivalent"
or "deliberately narrower"). The migration is therefore **retirement, not
replacement**: there is no live behavior to port to SSA, and an SSA-form
producer would inherit the *identical* soundness hole (see "Why an SSA
replacement inherits the same bug"). Disabling the pass causes **zero
divergence by construction** — it is already disabled.

## Disposition recommendation (up front)

**Retire the legacy `loop_postinc_fusion` producer; do not build an SSA
replacement.** Keep the `LOAD_POSTINC`/`STORE_POSTINC` opcodes and their sound,
unit-tested backend lowering intact — they are the target ISA capability, not
the bug, and a future *correct* postinc pass (deferred, out of scope) would
reuse them. The bug lives entirely in the **producer** (the flat fuse pass that
mints post-indexed ops the register allocator can later spill), never in the
opcode or its codegen.

Recommended concrete outcome: **Option B** below — delete the dead loop producer
(and, coordinated, the dead non-loop sibling that shares its flag), leaving the
opcode + lowering + their unit tests in place. This ticks the tracker entry
honestly ("proven unnecessary, removed") without pretending an SSA port
happened.

## Intent

- **Preserve (runtime):** nothing observable. The pass is off; every
  `-O0/-O1/-O2/-Os` output is already the un-fused LOAD + ADD form. Retirement
  must be a **no-op on all shipping optimization levels** (the acceptance bar is
  "identical codegen with the pass gone," which is trivially met because it is
  already gated off).
- **Preserve (capability):** the `LOAD_POSTINC` / `STORE_POSTINC` opcodes
  (`tccir.h:79-80`), their operand config (`ir/core.c:2051-2052`), their codegen
  dispatch (`ir/codegen.c:3651/3657`) and ARM emitters
  (`tcc_gen_machine_load_postinc_mop` / `tcc_gen_machine_store_postinc_mop`,
  `arm-thumb-gen.c:8370/8433`), and the codegen unit tests that exercise them.
  These are sound and independently tested; they are **not** in scope for
  deletion.
- **Not preserved (deliberately):** the unsound producer transform must never
  ship enabled. Retirement makes "off" permanent (removes the code path)
  instead of merely defaulting it off (where a stray `-fpostinc-fusion` still
  reaches the miscompile).
- **Non-goal:** reviving post-increment as a *correct* optimization. That is a
  net-new backend/regalloc feature (model the writeback so a spill stores the
  incremented base, or pin the base non-spillable across the postinc→next-use
  window). It is recorded in "Deferred: a correct revival" and is **not** this
  task.

## Current Legacy Shape

- **Two producers, one shared flag.** `opt_postinc_fusion` gates *both*:
  - `tcc_ir_opt_loop_postinc_fusion` (`ir/opt_fusion.c:872`, proto
    `ir/opt.h:545`) — the loop-aware pass this tracker entry names. Direct call
    at `tccgen.c:30385-30386` in the late loop-opt sequence, positioned **after**
    IV-SR (which mints the latch `ptr = ptr + #imm`) and **before**
    `loop_bound_remat` (whose comment, `tccgen.c` remat block, records "Must run
    AFTER loop_postinc_fusion to avoid breaking the latch ADD pattern"). It has
    **no `opt_pipeline.c` table entry** — only the hand-written call site.
  - `tcc_ir_opt_postinc_fusion` (non-loop, `ir/opt_fusion.c:244`, proto
    `ir/opt.h:542`), plus its context wrapper `tcc_ir_opt_postinc_fusion_ex`
    (`ir/opt_fusion.c:2985`, proto `ir/opt.h:672`). Direct call at
    `tccgen.c:29692-29693`, **and** a declarative pipeline entry
    (`ir/opt_pipeline.c:396`: `PASS_GATED("postinc",
    tcc_ir_opt_postinc_fusion_ex, ...)`). This is the "adjacent non-loop fusion"
    the parent tracker flags as audit-but-don't-remove-without-a-plan; it shares
    the flag and the exact soundness fate, so its retirement must be coordinated
    (see Migration Steps).
- **Gate / flag:** `tcc_state->opt_postinc_fusion` (field `tcc.h:1041`,
  `-fpostinc-fusion`), registered in the `-f` option table at `libtcc.c:1739`
  and force-initialized to `0` at `libtcc.c:2294`. There is **no `-O` level that
  turns it on** — unlike every sibling, whose gate is enabled at `-O1+`.
- **Observability:** none of the tracker-preferred kind (no `ssa:`/`loop:` dump
  name, no `TCC_DISABLE_PASS` registration). Because the pass is off by default,
  the usual "restore the un-optimized loop via the disable knob" check is
  vacuous — the disable state *is* the default state.
- **Driver internals (loop pass).** Self-detecting via `tcc_ir_detect_loops` /
  `tcc_ir_free_loops`; scans `loop->body_instrs` heavily (unlike
  `loop_bound_remat`). Per loop: finds the latch self-update ADD (TEMP,
  `dest==src1`, imm in `[1,255]`), requires **exactly one** deref of the pointer
  in the body, a dominance check (deref executes every iteration), a
  no-other-use safety scan, then either converts a standalone LOAD/STORE
  in-place to `LOAD_POSTINC`/`STORE_POSTINC` (+ a writeback `ASSIGN ptr, ptr` in
  an adjacent NOP slot, + NOPs the latch ADD), or — for an embedded deref —
  extracts a `LOAD_POSTINC` into two preceding NOP slots. It falls back to a
  plain LOAD + kept latch ADD when NOP slots are unavailable. The writeback
  `ASSIGN` is the pass's *own* attempt to model the ARM side effect; it is
  insufficient once the base spills (see below).
- **The opcode + lowering (sound, keep).** `LOAD_POSTINC`/`STORE_POSTINC`
  (`tccir.h:79-80`, config `ir/core.c:2051-2052`) lower through
  `ir/codegen.c:3651/3657` to `tcc_gen_machine_load_postinc_mop` /
  `tcc_gen_machine_store_postinc_mop` (`arm-thumb-gen.c:8370/8433`). Exercised by
  `tests/unit/arm/armv8m/test_codegen_mem.c:441` and
  `test_gen_dispatch_smoke.c:382/395`, and by an SCCP recovery test
  (`test_ssa_opt_sccp.c:735`). These pass regardless of the producer's fate.
- **Existing unit tests for the producer** (`tests/unit/arm/armv8m/test_opt_fusion.c`,
  call the drivers directly, **bypassing the flag**):
  - `test_loop_postinc_fusion_standalone_load_fuses` (`:914`) — happy path: latch
    ADD + standalone LOAD + adjacent NOP → `LOAD_POSTINC` + writeback `ASSIGN` +
    NOP'd latch.
  - `test_loop_postinc_fusion_multi_deref_kept` (`:943`) — the "exactly one
    deref" guard: two derefs → 0 changes, untouched.
  - non-loop coverage via `UT_COVERS("postinc")` (`:1394`) with two driver tests
    (`:795`, `:817`).
  These are **IR-level transform assertions**, not end-to-end correctness — they
  prove the fuse *fires* on a shape, which is exactly the transform being
  retired. They must be deleted or repurposed with the producer (see Test
  disposition).
- **IR regression pins (the load-bearing tests).** Four `TEST_FILES` entries in
  `tests/ir_tests/test_qemu.py`, all run at `-O0/-O1/-O2/-Os` (`OPT_LEVELS`),
  each expecting exit 0 — and each passes **because the fusion is off**:
  - `bug_postinc_spilled_ptr.c` — the canonical soundness pin. Its header
    documents: *"Fixed by disabling opt_postinc_fusion... Without the fix this
    test never returns (infinite loop) at -O1."* Mirrors `parse_number` almost
    verbatim (spilled loop-carried pointer + 64-bit accumulator).
  - `bug_postinc_store.c` (`:973`) — the STORE-instead-of-LOAD miscompile
    (`str.w r1,[r4],#1` where a `ldrb` load was needed), corrupting input in
    self-hosted `parse_number`.
  - `bug_postinc_struct.c`, `bug_struct_field_postinc.c` — struct-member
    post-increment shapes.
  These are the acceptance corpus: they must stay **green and unchanged**, and
  crucially must *remain protected* after retirement — i.e. retirement must
  ensure the miscompile is unreachable, not merely default-off.
- **Fuzz / self-host history:** this pass's recorded failure is not a
  differential-fuzz seed but a **self-hosting freeze** — enabling it hung `tcc`
  in `parse_number` on every integer literal (the `bug_postinc_spilled_ptr`
  story). That is a *soundness* failure of the transform itself, not a
  candidate-selection edge case, which is why the fix was to disable rather than
  guard.

## Why an SSA replacement inherits the same bug

The tracker's default instinct — "port the legacy loop pass to an SSA/CFG
pass" — does not apply here, and it is worth stating precisely why, so this is
not re-litigated later.

The bug is **not** in how candidates are *found* (loop detection, latch
matching, deref counting). Those are the parts SSA/CFG facts would improve. The
bug is in what the transform *produces*: a `LOAD_POSTINC`/`STORE_POSTINC` whose
base register is written back **only in the physical register**, by the ARM
instruction's side effect, with no IR-modeled def that the register allocator
honors when it decides to spill that base. SSA form changes none of that:

- SSA renaming gives the base a fresh name per def, but the *hardware* writeback
  still mutates one physical register in place; the allocator can still choose
  to spill the base between the postinc and its next use, and the spilled home
  never receives the incremented value.
- The legacy pass's adjacent `ASSIGN ptr, ptr` is exactly an attempt to
  manufacture that missing def. It is insufficient because it only pins the
  value at the instruction boundary; any spill of the base across the loop
  back-edge (the `parse_number` shape) still drops the increment. An SSA phi for
  the base would model the *value* flow but not force the allocator to
  materialize the post-increment into the spill slot.

So an `ssa:postinc_fusion` would reproduce `bug_postinc_spilled_ptr`'s infinite
loop unless it *also* shipped the missing piece: a register-allocation
contract that either (a) forbids spilling the post-incremented base across the
window where the writeback is live, or (b) lowers a spilled post-indexed base to
an explicit `ADD` + store on the spill path. That contract is the **real**
feature, and it is a backend/regalloc change, not a loop-pass migration.
Building it is the "Deferred: a correct revival" path; nothing short of it makes
the transform sound, and there is no partial SSA port worth landing in between.

## Disposition options (decision gate)

Analogous to the siblings' "Step 0," but the decision is *which retirement*, not
*which placement*. No probe run is required to establish the default is safe
(the pass is already off); the choice is scope.

- **Option A — leave dead, add observability only.** Keep both producers, keep
  the flag defaulted off, but register a `TCC_DISABLE_PASS` / dump name and add
  the loop pass to `scripts/bisect_opt.py` so the tracker's observability ask is
  met. *Rejected as the primary path:* it preserves an unsound, unreachable code
  path and a footgun flag (`-fpostinc-fusion` still miscompiles), buying only
  cosmetic observability for a pass no build exercises. It does not honestly
  close the tracker entry.
- **Option B — retire the producer, keep the opcode (recommended).** Delete
  `tcc_ir_opt_loop_postinc_fusion` and its `tccgen.c` call site; coordinate the
  deletion of the flag-sharing non-loop `tcc_ir_opt_postinc_fusion` /
  `_ex` / its `tccgen.c` call site / its `opt_pipeline.c:396` entry; remove the
  `opt_postinc_fusion` field and the `-fpostinc-fusion` option-table entry.
  **Keep** the `LOAD_POSTINC`/`STORE_POSTINC` opcodes, config, codegen, ARM
  emitters, and their codegen/SCCP unit tests. Convert the four `bug_postinc_*`
  IR pins from "passes because default-off" to "passes because the fuse no
  longer exists" (they stay green; they now also protect against re-introduction
  rather than re-enablement). This is the cleanest "proven unnecessary" outcome
  and makes the miscompile *unreachable*, not merely off.
- **Option C — retire the loop pass only, defer the non-loop sibling.** Delete
  only `tcc_ir_opt_loop_postinc_fusion` + its call site, leave the shared flag
  and the non-loop pass for a separate "adjacent fusion" cleanup. *Viable
  fallback* if touching the shared flag / pipeline entry is deemed out of scope
  for this tracker entry, but it leaves the flag half-orphaned (still gating a
  dead non-loop pass) and is a messier end state than B.

**Recommendation: Option B**, with **Option C as the fallback** if the parent
tracker wants the non-loop `postinc` fusion handled under its "adjacent non-loop
cleanup" bucket instead of here. Confirm the flag-ownership scope with the
maintainer before deleting the shared flag (that is the one cross-entry
decision).

## Migration Steps

Retirement, not replace-and-delete. Do the removal in one reviewable step per
Option B (fall back to C if the flag scope is contested).

- [ ] **Confirm the dead-code fact.** `grep` shows `opt_postinc_fusion` is only
  ever set to `0` (`libtcc.c:2294`) and read at the two call sites + the
  pipeline entry; no `-O` level and no test sets it to `1`. Record the grep in
  the PR description.
- [ ] **Establish the no-op baseline.** Build current tree; capture
  `-dump-ir`/disassembly for the four `bug_postinc_*` shapes plus a couple of
  `*p++` array-copy loops at `-O0/-O1/-O2/-Os`. This is the "identical after
  removal" reference (it will match trivially — the pass is off).
- [ ] **Delete the loop producer.** Remove `tcc_ir_opt_loop_postinc_fusion`
  (`ir/opt_fusion.c:872`), its prototype (`ir/opt.h:545`), and its `tccgen.c`
  call site (`≈30385-30386`, with the surrounding comment). Verify the
  `loop_bound_remat` "Must run AFTER loop_postinc_fusion" comment/ordering is
  updated (the ordering constraint is moot once the pass is gone; keep
  `loop_bound_remat` where it is and drop the stale reference).
- [ ] **Coordinate the non-loop sibling (Option B).** Delete
  `tcc_ir_opt_postinc_fusion` (`ir/opt_fusion.c:244`),
  `tcc_ir_opt_postinc_fusion_ex` (`:2985`), their prototypes (`ir/opt.h:542,672`),
  the `tccgen.c` call site (`29692-29693`), and the `ir/opt_pipeline.c:396`
  `PASS_GATED("postinc", ...)` entry. Remove the `opt_postinc_fusion` field
  (`tcc.h:1041`) and its option-table row (`libtcc.c:1739`) and the
  `libtcc.c:2294` initializer. *(Option C: skip this bullet; leave the flag and
  non-loop pass, and only stop referencing them from the loop path.)*
- [ ] **Keep the opcode + lowering untouched.** No change to `tccir.h:79-80`,
  `ir/core.c:2051-2052`, `ir/codegen.c:3651/3657`, `arm-thumb-gen.c:8370/8433`.
- [ ] **Reconcile tests** (see Test disposition).
- [ ] **Build + gate.** `make cross -j$(nproc)`; `make test -j16` (incl. GCC
  torture compile); run the unit suite. The `bug_postinc_*` pins and the
  codegen/SCCP opcode tests must stay green.
- [ ] **Tracker bookkeeping.** Tick `loop_postinc_fusion` in the parent tracker's
  "Per Legacy Pass" list as **"proven unnecessary — retired (disabled + unsound;
  no SSA replacement)"**, and add this file to its "Detailed Pass Plans" list.

## Test disposition

- **`bug_postinc_spilled_ptr.c`, `bug_postinc_store.c`, `bug_postinc_struct.c`,
  `bug_struct_field_postinc.c`** — **keep, stay green, unchanged.** They remain
  correctness pins at `-O0/-O1/-O2/-Os`. Their protective meaning upgrades from
  "the fusion is defaulted off" to "the fusion producer does not exist"; update
  each header comment's "Fixed by disabling opt_postinc_fusion" line to "Fixed by
  removing the postinc fusion producer" so the comment does not dangle after the
  flag is gone. (Under Option C, leave the comments as-is.)
- **`test_opt_fusion.c` producer tests** (`test_loop_postinc_fusion_*` `:914/:943`,
  the non-loop driver tests `:795/:817`, `UT_COVERS("postinc")` `:1394`) —
  **delete with the producers** (Option B) or leave the non-loop ones and delete
  only the loop ones (Option C). These assert the retired transform fires; they
  have no meaning once the code is gone. Refresh `UT_COVERS`/`PASS_COVERAGE.md`
  accordingly and remove the `postinc` row from the coverage matrix.
- **Opcode / codegen tests** (`test_codegen_mem.c:441`,
  `test_gen_dispatch_smoke.c:382/395`, `test_ssa_opt_sccp.c:735`) — **keep
  unchanged.** They validate the surviving `LOAD_POSTINC`/`STORE_POSTINC`
  lowering directly and do not depend on any producer pass.
- **No new IR regression is required.** The existing four pins already cover the
  motivating shapes; retirement adds no new behavior to pin. (A future *correct*
  revival would add its own tests.)

## Comment policy

Per [[comments-max-one-liner]] and the sibling plans: any legacy comment in
*touched* code is deleted or compressed to a single line; no comment blocks are
added. The `libtcc.c:2294` block rationale and the `bug_postinc_*` header
narratives move their "why" into this plan and the pin headers, not into new
source. When the flag and its initializer are deleted, the multi-line rationale
comment goes with them (its content is preserved here).

## Deferred: a correct revival (out of scope)

Recorded so the sequencing is on record and this entry is not confused with a
"we lost an optimization" regression. Post-increment addressing (`ldr [rN],#imm`)
is a real ARM code-size/perf win GCC takes; reviving it *soundly* is a
backend/regalloc feature, independent of this tracker:

- **Model the writeback so spills are correct.** Either mark the post-incremented
  base non-spillable across the postinc→next-use live window (a regalloc
  constraint the linear-scan allocator honors), or teach the spill path to
  lower a spilled post-indexed base to an explicit `ADD` + store so the home
  slot advances. The current adjacent-`ASSIGN` trick is not sufficient.
- **Own the candidate detection in SSA/CFG.** Once the writeback is modeled, the
  finder half *can* be the clean SSA/CFG pass the tracker prefers (latch IV +
  single dominating deref), reusing the surviving `LOAD_POSTINC`/`STORE_POSTINC`
  opcodes and lowering.
- **Re-earn the four `bug_postinc_*` pins.** Any revival must pass all four
  (especially `bug_postinc_spilled_ptr` at `-O1`) *with the transform enabled* —
  that is the gate the original pass failed.

This is explicitly **not** attempted here; retirement (Option B) is the complete
scope of this task.

## Acceptance

For landing the retirement (Option B; Option C drops the flag/non-loop bullets):

- [ ] `grep` confirms `opt_postinc_fusion` had no enabling site (recorded in PR).
- [ ] Codegen for the four `bug_postinc_*` shapes and sample `*p++` loops is
  **byte-identical** before and after removal at `-O0/-O1/-O2/-Os` (trivially,
  since the pass was already off).
- [ ] `bug_postinc_spilled_ptr.c`, `bug_postinc_store.c`, `bug_postinc_struct.c`,
  `bug_struct_field_postinc.c` pass at `-O0/-O1/-O2/-Os` (exit 0, no hang).
- [ ] Surviving opcode/codegen unit tests (`test_codegen_mem`,
  `test_gen_dispatch_smoke`, `test_ssa_opt_sccp`) pass; retired producer unit
  tests removed and coverage matrix refreshed.
- [ ] `make cross -j$(nproc)`; `make test -j16`; GCC torture compile suite.
- [ ] **Maintainer-run** (per the 2026-07-06 instruction): a
  `python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu` and a
  `tests/fuzz/sweep_all_chunks.py 0 1000 --mode triage` — expected **0
  divergences and 0 delta** vs. baseline, because retirement removes only an
  already-disabled path.
- [ ] Parent tracker `loop_postinc_fusion` checkbox ticked as **"proven
  unnecessary — retired"**, with this file linked under "Detailed Pass Plans."

## Assumptions

- `opt_postinc_fusion` is disabled in all shipping configurations and no test or
  `-O` level enables it; removal is a no-op on default codegen (verified by grep
  + the byte-identical baseline).
- The `LOAD_POSTINC`/`STORE_POSTINC` opcodes and their ARM lowering are sound and
  are retained; only the *producer* passes are unsound and removed.
- The shared `opt_postinc_fusion` flag also gates the non-loop
  `tcc_ir_opt_postinc_fusion`; Option B treats both together (the recommended,
  cleaner end state), Option C defers the non-loop pass to the tracker's
  "adjacent non-loop cleanup" bucket. The flag-ownership scope is the one
  decision to confirm with the maintainer before deleting the shared flag.
- A correct post-increment optimization is desirable long-term but is a
  backend/regalloc feature, not part of this loop-pass retirement.

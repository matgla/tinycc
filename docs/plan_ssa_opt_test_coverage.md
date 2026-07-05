# Plan: Unit-Test Coverage for the `ssa_opt` Family

**Status:** proposed · **Owner:** unit-test track · **Created:** 2026-07-04

This plan brings the SSA optimizer family (`ir/opt/ssa_opt*.c`) — currently the
single largest coverage hole in the tree — under host-native unit test. Read
[`docs/writing_unit_tests.md`](writing_unit_tests.md) first; the two workflow
rules there (no production-source edits, all bugs to `docs/bugs.md`) apply
verbatim and are load-bearing here because parallel fuzz-fix tasks edit these
exact files.

---

## 1. Current state (ground truth from `coverage.txt`)

Every target-independent SSA pass is at **0% line coverage**:

| File | Lines | Cov | Role |
|------|------:|----:|------|
| `ir/opt/ssa_opt.c` | 782 | **0%** | driver + use-def machinery (`vinfo`, `scan`, `replace_all_uses`, resolvers) |
| `ir/opt/ssa_opt_phi.c` | 91 | **0%** | trivial-phi elimination |
| `ir/opt/ssa_opt_strength.c` | 142 | **0%** | strength reduction |
| `ir/opt/ssa_opt_narrow.c` | 204 | **0%** | width narrowing |
| `ir/opt/ssa_opt_reassoc.c` | 308 | **0%** | reassociation |
| `ir/opt/ssa_opt_cmp_eq.c` | 333 | **0%** | CMP equality-fact propagation |
| `ir/opt/ssa_opt_fold.c` | 486 | **0%** | algebraic/const fold |
| `ir/opt/ssa_opt_gvn.c` | 503 | **0%** | global value numbering (⚠ *linked* but still 0% — see below) |
| `ir/opt/ssa_opt_branch.c` | 585 | **0%** | branch simplification |
| `ir/opt/ssa_opt_dead_loop.c` | 887 | **0%** | dead-loop elimination |
| `ir/opt/ssa_opt_dce.c` | 1222 | **0%** | dead-code elimination |
| `ir/opt/ssa_opt_load_cse.c` | 1380 | **0%** | load CSE + store→load forwarding |
| `ir/opt/ssa_opt_cprop.c` | 1686 | **0%** | copy/const propagation |
| `ir/opt/ssa_opt_sccp.c` | 1891 | **0%** | sparse conditional constant prop |
| **total (ir/opt/ssa_opt*)** | **~10,700** | **0%** | |

For contrast, the neighbours that *are* covered:
`arch/arm/ssa_opt_arm.c` **84%** (via `test_ssa_opt_arm.c`), `ir/ssa.c` **94%**
(SSA construction, via `test_ir_ssa.c` + metamorphic).

### Why it's 0%

1. The Makefile lists all `ir/opt/ssa_opt*.c` in **`UT_COVERAGE_ONLY_SRCS`** —
   compiled with `--coverage` for the report, **but never linked**.
2. `tests/unit/arm/armv8m/ra_link_stubs.c` provides **no-op stub definitions**
   for every entry point (`ssa_opt_dce`, `ssa_opt_cprop`, … `ssa_opt_dead_loop`)
   and the whole driver (`tcc_ir_ssa_opt_init/rebuild/free/run/run_target`), so
   the main and backend binaries link the *stubs*, not the real code.
3. `ssa_opt_gvn.c` is in `UT_MODULE_SRCS` (linked for real) yet still reads 0%:
   nothing calls it, because the *driver* that would (`tcc_ir_ssa_opt_run`) is
   itself stubbed in `ra_link_stubs.c`, and `--gc-sections` strips the unreached
   real `ssa_opt_gvn`.
4. `test_metamorphic_ssa.c` is an honest **SKIP**: the SSA substrate
   (`IRSSAState` + `IRCFG` + dominators + def-use `vinfo`) that the passes
   consume is not stood up in the isolated harness. Its header documents the
   exact enabling path — this plan executes it.

**Net:** the SSA optimizer's actual logic has *zero* test coverage today. Given
the density of known fuzz miscompiles in this family (see §7), that is the
highest-value gap in the project.

---

## 2. The API we're testing (`ir/opt/ssa_opt.h`)

Every pass has the same clean shape — it takes a context and returns a change
count, mutating `ctx->ir` in place:

```c
typedef struct IRSSAOptCtx {
  struct TCCIRState *ir;   // the IR under optimization
  IRSSAState        *ssa;  // phi nodes (block_phis[]), renamed defs
  IRCFG             *cfg;  // blocks, edges, dominators
  IRSSAVregInfo     *vinfo;// per-TEMP def/use chains
  int vinfo_cap, changes, no_stack_fwd;
} IRSSAOptCtx;

int ssa_opt_dce(IRSSAOptCtx *ctx);        // + cprop/fold/gvn/sccp/load_cse/…
```

Driver (`ssa_opt.c`): `tcc_ir_ssa_opt_init(ctx, ir, ssa, cfg)` →
`tcc_ir_ssa_opt_rebuild(ctx)` (builds `vinfo`) → per-pass calls, or
`tcc_ir_ssa_opt_run(ctx)` which runs the whole fixed-order pipeline:

```
var_const_fold → sccp → cprop → var_to_param_forward → fold → load_cse →
branch → cmp_eq_prop → reassoc → strength → narrow → gvn → phi_simplify →
dead_loop → dce
```

This uniform `(ctx) → int` surface is exactly what makes host unit testing
tractable: build a small `ctx`, call one pass, assert on the mutated IR.

Two testable substructures beyond the passes themselves, both in `ssa_opt.c`:
- **Use-def machinery**: `ssa_opt_vinfo`, `ssa_opt_scan_instr_uses`,
  `ssa_opt_replace_all_uses`, `ssa_opt_nop_instr`, `ssa_opt_add/remove_use_*`.
- **Address resolvers** (underpin load_cse/cprop/sccp aliasing, and several
  known bugs): `ssa_opt_resolve_lea_stackloc[_ex]`,
  `ssa_opt_resolve_temp_to_base_off`, `ssa_opt_indirect_stack_offset[_ex]`.

---

## 3. Architecture decision: a new isolated binary (`build_ssaopt`, UT11)

We cannot link the real `ssa_opt*.c` into the main or backend binary —
`ra_link_stubs.c` (linked there) already defines every one of their symbols, so
it's a guaranteed multiple-definition. This is the identical situation that gave
`arm-thumb-gen.c` its own `build_backend` and `tccgen.c` its own `build_tccgen`.

**Recommendation: add an 11th binary `build_ssaopt` (target `run-ssaopt`).**

Model it on the existing `UT2_*`/`UT3_*` blocks:

```makefile
# UT11 — SSA optimizer family. Links the REAL ir/opt/ssa_opt*.c on top of the
# shared IR module set, so the passes run against real SSA/CFG/vinfo built by
# ir/ssa.c + ir/cfg.c (already in UT_MODULE_SRCS).
BUILD_DIR11 := build_ssaopt
UT11_MODULE_SRCS := \
    $(UT_MODULE_SRCS) \
    $(TOP)/ir/opt/ssa_opt.c        $(TOP)/ir/opt/ssa_opt_cprop.c \
    $(TOP)/ir/opt/ssa_opt_dce.c    $(TOP)/ir/opt/ssa_opt_fold.c \
    $(TOP)/ir/opt/ssa_opt_sccp.c   $(TOP)/ir/opt/ssa_opt_load_cse.c \
    $(TOP)/ir/opt/ssa_opt_branch.c $(TOP)/ir/opt/ssa_opt_cmp_eq.c \
    $(TOP)/ir/opt/ssa_opt_reassoc.c $(TOP)/ir/opt/ssa_opt_narrow.c \
    $(TOP)/ir/opt/ssa_opt_phi.c    $(TOP)/ir/opt/ssa_opt_strength.c \
    $(TOP)/ir/opt/ssa_opt_dead_loop.c
    # ssa_opt_gvn.c already in UT_MODULE_SRCS

UT11_LOCAL_SRCS := \
    test_main11.c \
    test_ssa_build.c            \  # Phase 0 substrate self-checks
    test_ssa_opt_usedef.c       \  # Phase 1
    test_ssa_opt_phi.c test_ssa_opt_strength.c test_ssa_opt_narrow.c \
    test_ssa_opt_reassoc.c test_ssa_opt_cmp_eq.c              \  # Phase 2
    test_ssa_opt_fold.c test_ssa_opt_gvn.c test_ssa_opt_branch.c \  # Phase 3
    test_ssa_opt_dce.c test_ssa_opt_dead_loop.c              \
    test_ssa_opt_load_cse.c test_ssa_opt_cprop.c test_ssa_opt_sccp.c \  # Phase 4
    test_ssa_metamorphic.c      \  # Phase 5
    stubs.c elfsec_stubs.c tcc_state_stub.c \
    ra_link_stubs.c                # compiled with -DUT_SSA_OPT_REAL (see below)
```

### The `ra_link_stubs.c` conflict — one guard macro

`ra_link_stubs.c` is *shared* (the main/backend/RA binaries need its ssa_opt
no-ops). Don't fork it — guard the ssa_opt block so the new binary compiles it
without those definitions:

```c
/* ra_link_stubs.c */
#ifndef UT_SSA_OPT_REAL          /* build_ssaopt links the real files instead */
void tcc_ir_ssa_opt_init(...) { ... }
int  ssa_opt_dce(IRSSAOptCtx *ctx) { (void)ctx; return 0; }
/* … all ssa_opt_* no-ops + vinfo helpers + driver … */
#endif
```

and in the UT11 object rule only:

```makefile
$(BUILD_DIR11)/%.o: %.c $(UT_ROOT)/ut.h Makefile $(COV_STAMP11)
	$(HOSTCC) $(UT_CFLAGS) -DUT_SSA_OPT_REAL $(UT_DEPFLAGS) -c $< -o $@
```

Note the real `vinfo`/use-def helpers live in `ssa_opt.c`, so the guard must wrap
the *entire* ssa block in `ra_link_stubs.c` (passes **and** `vinfo`/`scan`/
`replace_all_uses` **and** init/rebuild/free/run), not just the no-op passes.

**Editing `ra_link_stubs.c` and the `Makefile` is allowed** — they're harness
files, not product source (Rule 1). The `ir/opt/ssa_opt*.c` files themselves must
never be touched by this work.

### Watch-outs when the binary first links

- `USING_GLOBALS`: every `ssa_opt_*.c` does `#define USING_GLOBALS`, so state-var
  access is `tcc_state->…`. A real `tcc_state` (`tcc_state_stub.c`) must be
  present and non-NULL before any pass runs; clear state-vars before nulling it
  at teardown (see the `USING_GLOBALS` gotcha in the test-writing guide).
- `dbg_scan_imm_dest` / `dbg_scan_overlap`: `ssa_opt.c`'s driver calls
  `dbg_scan_imm_dest`. `ir/opt_pipeline.c` provides the real ones — link it
  **or** stub them, never both (multiple definition).
- `tcc_ir_ssa_opt_register_target`: backends register ARM generators via this;
  in this binary either leave the target table empty or drive it explicitly.

### Coverage wiring

Add a `coverage-ssaopt` target (mirror `coverage-backend`) and add
`$(CURDIR)/$(BUILD_DIR11)` to the merged `coverage` target's `gcovr` invocation.
Because gcovr sums hit-counts across build trees, the ssa_opt files can *stay* in
`UT_COVERAGE_ONLY_SRCS` for the main binary's bookkeeping while the real coverage
now flows from `build_ssaopt`.

---

## 4. Fixture strategy — two layers, staged

The hard part is not calling a pass; it's producing a *valid* `ctx` (ssa/cfg/
vinfo/phis). Two complementary approaches, introduced in order:

### Layer A — hand-built `ctx` (precise, start here)

Build a minimal `TCCIRState` + `IRSSAState` + `IRCFG` + `vinfo` directly, setting
only the fields a pass reads. This is exactly what `test_ssa_opt_arm.c` already
does (it hand-builds `vinfo` and calls an ARM `ssa_gen_*` directly), and what
`test_ir_ssa.c` does for construction. Best for leaf passes and for pinning a
specific rewrite. Example (worked, for `ssa_opt_phi_simplify`, whose full logic
is in `ssa_opt_phi.c`):

```c
UT_TEST(test_phi_simplify_collapses_trivial_phi)
{
  /* CFG: 1 block; SSA: phi T5 = [T3, T3, T3]; a use of T5. */
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/6);
  ssa_add_phi(&c, /*block=*/0, /*dest=*/T(5), (int[]){T(3),T(3),T(3)}, 3);
  int use_i = ssa_add_instr(&c, TCCIR_OP_ASSIGN, /*dest=*/T(7), /*src1=*/T(5));
  ssa_ctx_rebuild(&c);                       /* builds real vinfo */

  int changed = ssa_opt_phi_simplify(c.ctx);

  UT_ASSERT(changed >= 1);
  UT_ASSERT(ssa_block_phi_count(&c, 0) == 0);          /* phi removed */
  UT_ASSERT_EQ(ssa_instr_src1(&c, use_i), T(3));        /* T5 use → T3 */
  ssa_ctx_free(&c);
  return 0;
}
```

The `ssa_ctx_*` helpers are the deliverable of Phase 0 (`ssa_build.h`). Also add
the negative case from the pass's own comment: a phi whose dest is kept alive by
a barrel-shift `src2` use must **not** be dropped (fuzz seed 19826) — a
ready-made regression lock straight out of the source comment.

### Layer B — real construction + metamorphic (broad, high bug-yield)

Generate straight-line/loop IR with the existing `ir_gen.h`, run the **real**
`ir/ssa.c` construction + `ir/cfg.c` + dominators + `tcc_ir_ssa_opt_rebuild` to
get a genuine `ctx`, then run a pass (or the whole pipeline) and **evaluate the
result with `ir_eval.h`'s reference interpreter**, asserting semantics are
preserved. This is the SKIP that `test_metamorphic_ssa.c` documents; the
generator, interpreter, and delta-reducer are pipeline-agnostic and reused
unchanged — only the pass-driver differs.

Layer B is where the differential bug-hunting happens. **Prerequisite (non-
negotiable):** cross-validate SSA *construction* on hand-written cases first
(Phase 0), so a metamorphic failure is attributable to a pass, not to an
untested construction substrate — otherwise it manufactures false positives (the
exact reason the suite skips today).

---

## 5. Phasing

Each phase adds one suite (a `test_ssa_opt_<x>.c`) to `build_ssaopt`, annotated
`UT_COVERS("ssa:<pass>")`. Ordered by dependency and by size/independence.

### Phase 0 — Substrate bring-up (`test_ssa_build.c`, `ssa_build.h`)
Stand up the binary and the fixture layer.
- Add UT11 to the Makefile; apply the `ra_link_stubs.c` guard; get a clean link.
- Write `ssa_build.h`: `ssa_ctx_new`, `ssa_add_instr`, `ssa_add_phi`,
  `ssa_add_block`/edges, `ssa_ctx_rebuild` (calls the real init+rebuild),
  accessors (`ssa_instr_src1`, `ssa_block_phi_count`, …), `ssa_ctx_free`.
- **Self-checks**: 6–8 tests asserting `ssa_ctx_rebuild` produces the correct
  `vinfo` (def_instr, def_count, use lists) and phi wiring on known IR — this is
  the trust anchor for Layer B.
- Flip `test_metamorphic_ssa`'s skip note to point at the now-live path.
**Exit:** binary links & runs; construction self-checks green.

### Phase 1 — Use-def + resolvers (`test_ssa_opt_usedef.c`)  `UT_COVERS("ssa:usedef")`
Cover `ssa_opt.c`'s foundational surface directly: `vinfo`,
`scan_instr_uses` (incl. MLA accumulator + memory-write STORE dest — the class
behind several fuzz bugs), `replace_all_uses`, `nop_instr`, and all four
resolvers with their documented contracts (`resolve_lea_stackloc_ex`'s
`-1` real-slot vs `>=0` `&VAR` distinction; `indirect_stack_offset` scale-0 index
rule). High leverage: unblocks every later suite.

### Phase 2 — Leaf passes  (`phi`, `strength`, `narrow`, `reassoc`, `cmp_eq`)
Small, self-contained, Layer-A. Target **80%+** each. `phi_simplify` worked
example above; `narrow`/`strength` are near-pure; `cmp_eq` needs a 2-block
dominator fixture (push a fact on the CMP+JEQ edge, assert the dominated compare
folds).

### Phase 3 — Mid passes  (`fold`, `gvn`, `branch`)
Layer-A tables of rewrites + a few Layer-B checks. For `gvn`, include the
64-bit-truncation regression (longlong 686: GVN must not coalesce 64-bit results
into a high-word-dropping copy). Target **75%+**.

### Phase 4 — Large, high-bug-density passes  (`dce`, `dead_loop`, `load_cse`, `cprop`, `sccp`)
The payload. Layer-A for specific rewrites + Layer-B metamorphic for breadth.
Seed regression locks directly from the known fixed bugs (§7). These files are
60–70% realistic (many paths are loop-shape- or profile-gated); prioritize the
aliasing/forwarding logic where the miscompiles cluster:
- `load_cse`: store→load forwarding across intervening indexed loads/stores
  (ptr 80958, ptr 72674, agg_deep 86393, agg_deep 36641).
- `cprop`: copy-forwarding an address-taken local across an aliasing pointer
  store (combo 74935 / agg_deep 69382); `var_const_fold` deleting a def with an
  intervening use (signed 2016).
- `sccp`: entry-block exemption skipping conditional `*p` stores via VAR-held
  pointers (ptr 58108).

### Phase 5 — Driver + metamorphic  (`test_ssa_metamorphic.c`, driver tests)
- Wire `ir_gen.h`/`ir_eval.h` over the real pipeline (Layer B). Run a
  semantics-preservation sweep (per-pass **and** full `tcc_ir_ssa_opt_run`);
  reduce any mismatch with the existing delta-reducer; **report divergences in
  `docs/bugs.md`, do not fix** (Rule 2 — parallel fuzz-fix tasks own these
  files).
- Test the driver itself: fixed pass order, convergence/idempotence
  (`run` twice → 0 changes the second time), `run_target` path.

### Phase 6 — Golden-IR (complementary, existing infra)
Add `-dump-ir-passes=ssa:<pass>` golden cases per pass via the existing
`test_golden_ir.py` machinery (it already emits `=== AFTER ssa:<pass> ===`).
End-to-end lock that also exercises the front half; cheap once a pass is
understood from its unit tests.

---

## 6. Success metrics

- **Coverage:** `ir/opt/ssa_opt*` family from **0% → ≥65% aggregate**; leaf/mid
  passes ≥80%, large passes ≥60%. Tracked via `make -C tests/unit/arm/armv8m
  coverage-ssaopt` and rolled into the merged `ut-coverage` report.
- **Ledger:** every pass carries a `UT_COVERS("ssa:<pass>")` marker; register
  the family in `tests/unit/PASS_COVERAGE.md`; `make check-pass-coverage` (if the
  ledger script is present) shows no SSA gap.
- **Metamorphic:** `test_metamorphic_ssa` flips from SKIP to a real
  semantics-preservation sweep (0 unreported mismatches).
- **Wiring:** `make ut` includes `run-ssaopt` (add UT11 to the aggregate `run`
  list) and stays green on a clean rebuild.

---

## 7. Bug-hunting payload — known ssa_opt defects become regression locks

This family is where the differential fuzzer has found the most miscompiles.
Every *already-fixed* one has a minimal repro and an expected-correct behavior in
`docs/bugs.md` / the session memory — turn each into a Layer-A regression lock in
the matching suite (assert the **fixed/correct** behavior; if you find the tree
has regressed, that's a live bug → `docs/bugs.md`):

| Pass suite | Seed(s) | Defect (now fixed — lock the correct behavior) |
|-----------|---------|-----------------------------------------------|
| `cprop` | combo 74935 / agg_deep 69382 | copy-prop forwarded a copy of an address-taken local across an aliasing `*p` store (`src_addrtaken` guard) |
| `cprop` (`var_const_fold`) | signed 2016 | deleted a VAR const def with an intervening use |
| `load_cse` | ptr 80958 | NOP'd a store kept live by an intervening runtime `LOAD_INDEXED` |
| `load_cse` (`sl_forward`) | ptr 72674 / struct_byval 60351 | re-marked a multiply-defined `?:` merge temp valid after forwarding |
| `load_cse` (`ptr_load_cse`) | agg_deep 86393 | CSE'd a `*pa` deref across an `is_lval` write to an addr-taken local |
| `sccp` | ptr 58108 | entry-block exemption skipped conditional `*p` stores via VAR-held pointers |
| `gvn` | longlong 686 | coalesced 64-bit results into a high-word-dropping ASSIGN copy |
| `reassoc` | ptr 85636 | `add_reassoc` folded across a gap STORE to an addr-taken base |
| `phi` | seed 19826 | dropped a phi whose dest was kept live by a barrel-shift `src2` use |

New divergences the metamorphic layer surfaces follow the same rule: pin +
report, never fix in this task.

---

## 8. Risks & mitigations

| Risk | Mitigation |
|------|-----------|
| SSA construction substrate is itself bug-prone → false metamorphic positives | Phase 0 construction self-checks are a hard gate before Layer B is trusted (this is precisely why the suite skips today) |
| Multiple-definition on link | Single `UT_SSA_OPT_REAL` guard in `ra_link_stubs.c`; watch `dbg_scan_*` (link `opt_pipeline.c` xor stub) |
| `tcc_state` NULL segfault under `USING_GLOBALS` | Real `tcc_state_stub.c`; correct set/clear ordering at teardown |
| Editing `tcc.h` forces a full product rebuild → surfaces *other tasks'* uncommitted `ir/*.c` crashes | Don't touch `tcc.h`; this work needs only harness files + the Makefile |
| Racing a parallel fuzz-fix that edits the same `ssa_opt*.c` | Never rebuild the compiler while a sweep/reducer runs; rebuild clean and re-confirm before triaging any failure (stale-binary rule) |
| "Fixing" a pass to make a test pass | Forbidden (Rule 2). Pin current behavior + `docs/bugs.md` |

---

## 9. Immediate next steps

1. Land Phase 0: add the `build_ssaopt` (UT11) Makefile block + `coverage-ssaopt`
   target, apply the `ra_link_stubs.c` guard, write `ssa_build.h` +
   `test_ssa_build.c` construction self-checks. Confirm a clean link and green
   self-checks.
2. Phase 1 (`test_ssa_opt_usedef.c`) — use-def + resolvers.
3. Phase 2 leaf passes, starting with `ssa_opt_phi_simplify` (worked example
   above) as the end-to-end shakedown of the fixture layer.
4. Register `run-ssaopt` in the aggregate `run`/`make ut` and in
   `PASS_COVERAGE.md`.

Each phase is an independent, mergeable unit; the family reaches meaningful
coverage incrementally, and Layer B turns the suite into a standing differential
bug-hunter once Phase 5 lands.

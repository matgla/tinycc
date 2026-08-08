# Optimizer .text Consolidation

**Status**: Planned
**Created**: 2026-08-07
**Goal**: Shrink the optimizer's `.text` footprint (~45-60 KB expected) and remove
structural duplication — dead driver code, `_ex` adapter thunks, fat DSL codegen,
the cloned SSA driver — without changing pass order or emitted code.
**Supersedes**: `ssa_opt_loop_consolidation.md` (2026-07-09), which is stale: it
predates the `source/opt/` reorg and proposes the legacy retirement that
`plan_legacy_flat_ir_ssa_retire.md` has since completed.

---

## Measurements (2026-08-07, native-stage ARM objects → bin/armv8m-tcc.elf)

Device `.text` = 1,437,000 B. The optimizer (`source/opt/`, 209 TUs) is
**~738 KB ≈ 49%** of it:

| Subsystem | .text | files |
|---|---|---|
| opt/flat (legacy, pre-SSA linear IR) | 470,678 | 123 |
| opt/ssa | 225,365 | 57 |
| opt/util + analysis + engine + ra + pipeline | 42,130 | 29 |
| (for scale: backend 215 KB, tccgen.o 219 KB, ir core 137 KB) | | |

Flat: memory 164 KB, scalar 122 KB, loop 79 KB, dce 47 KB, fusion 34 KB, cfg 14 KB,
ipa 12 KB. SSA: scalar 104 KB, cfg 38 KB, memory 30 KB, dce 20 KB, loop 19 KB.
Top objects: `ssa/scalar/fold.o` 32.5 KB, `flat/memory/sl_forward.o` 30.4 KB,
`flat/loop/const_sim.o` 18.5 KB, `ssa/scalar/sccp.o` 17.8 KB.

Key facts driving the plan:

- **The legacy tier is not retirable.** `plan_legacy_flat_ir_ssa_retire.md` is
  COMPLETE (2026-07-19): every in-scope flat pass reached a terminal state, and
  `docs/optimizations/duplication_analysis.md` verdicts the remaining duplicate
  pairs "Not removable" (flat shrinks IR before SSA construction). What remains
  duplicated is *walks and boilerplate*, not semantics.
- **The DSL is size-fat**: 74 `opt_dsl_dispatch_*` rules = 67,720 B, avg 915 B/rule
  (9.4% of opt .text). `PATTERN` (`framework/opt_dsl.h:30-44`) eagerly binds
  dest/src1/src2 via three out-of-line by-value `IROperand` calls per dispatch;
  `REWRITE` builds an `IROptRewriteSpec` compound literal applied through four
  runtime tag tests. Three trivial strength rules (~34 source lines) compile to
  2,420 B.
- **~100 `_ex` adapter thunks = 15.8 KB** (avg 158 B, thumb prologue-dominated),
  existing only because the table fn-ptr takes `IROptCtx*` while most pass bodies
  take `TCCIRState*`.
- **Dead driver code**: `engine/pass_registry.c` (207 lines, 3 TODO-stub passes,
  test-only — production fp-cache goes via `fp_cache_shim.c`), the never-executed
  7-entry `fusion_passes` group (`function_pipeline.c:84` calls the same gens
  directly), test-only `pipeline_o0/o1/os` tables, ignored `IROptPass.invalidates`
  metadata. Already `--gc-sections`-stripped from `.text`; the payoff is the
  divergence hazard and ~300-500 B of `.data` (pass tables carry relocations).
- **Duplicate SSA driver**: `ir/regalloc.c:5225-5283` `RUN_SSA` clone (23 passes,
  `!had_promotable` path) vs `source/opt/ssa/engine/driver.c:63` (27 invocations).
  Near- but NOT identical: the clone uses `ssa_opt_var_forward`, omits
  `tmp_block_const`/`cmp_eq_prop`/`dead_loop`/late_tail, runs `guard_collapse`
  bare inside the loop, ends with `run_target`.
- 156 of 180 pass TUs hand-roll their walks (~65% skeleton lines in small flat
  passes): 40 inline forward NOP-skips + 17 backward (no util exists), 17 private
  re-implementations of def/use/ever-written/flag-consumer scans. Same-name
  duplicate statics across opt TUs: only ~4.3 KB.

## Explicitly dropped

- **Merging the 20 `run_gens` call sites into fewer walks.** Zero `.text` (tables
  and gen bodies remain) and the highest risk here: `run_gens` is first-match-wins
  per instruction, so two back-to-back tables are NOT equivalent to one merged
  table — table A's full walk can enable an earlier-index match for table B.
  Every merge is an ordering change → golden-IR churn → full validation, for a
  compile-time-only payoff. Revisit only if on-device compile time becomes the goal.
- **C6: migrating hand-rolled peepholes into the slimmed DSL** — follow-up work
  with its own validation cost; only sensible after the DSL is thin.

---

## Landing order: A → B → C1..C5 → D1+D3 → D4 → D2

No retained chunk changes pass order or walk structure, so **every commit demands a
byte-identical `make test-golden-ir` run** — any diff is a bug in the change.

### A — delete dead driver code (hygiene, ~350 LOC)

- Delete `source/opt/engine/pass_registry.c` + its types/prototypes in
  `source/opt/include/tccopt.h` (`TCCOptPass`, `TCCOptRegistry`, `tcc_optimize_ir`,
  `tcc_opt_register_pass`, `tcc_opt_get_passes`, `tcc_opt_run_pass`,
  `tcc_opt_get_level`, stub prototypes). Keep `fp_mat_cache.c` and `TCCOptStats`.
- `pipeline_table.c`: delete `fusion_passes`, `pipeline_o0/o1/os`,
  `tcc_ir_opt_get_pipeline`, `tcc_ir_opt_run_default`, `IROptLevel`. Export named
  groups (`extern const IRPassGroup propagation_group, memory_group,
  late_cleanup_group;` — precedent: `entry_store_group`, `pipeline_table.c:253`)
  and rewrite the 5 positional call sites: `function_pipeline.c:62,163,209`,
  `ir/regalloc.c:4936,5133`. Keep `tcc_ir_opt_run_pipeline` (test-used, gc'd).
- Tests: `tests/unit/arm/armv8m/Makefile:358`; delete/gut `test_tccopt.c` registry
  cases; rewrite the level-table cases in
  `test_opt_pipeline_orchestration.c:563-647` against named groups. Grep
  `IR_OPT_LEVEL_|run_default|get_pipeline|tcc_opt_` across `tests/` before deleting.

### B — kill ~90 `_ex` thunks (~14-16 KB)

- `source/opt/include/opt_pipeline.h`:

```c
typedef int (*ir_opt_pass_ir_fn)(struct TCCIRState *ir);
typedef struct IROptPass {
  const char *name;
  union { ir_opt_pass_fn run; ir_opt_pass_ir_fn run_ir; } fn;
  uint32_t requires;      /* dead `invalidates` field removed here */
  uint16_t flag_offset;
  uint8_t takes_ir;       /* fits in former padding — 0 rodata cost */
} IROptPass;
```

  (Option "convert pass bodies to `IROptCtx*`" rejected: the `TCCIRState*`
  functions are called directly all over the cascades, `function_pipeline.c`, and
  `regalloc.c`; converting would force ctx creation at dozens of direct call sites.)
- `pipeline_run.c` both dispatch points (trigger path :176-178, main loop :212):
  `pass->takes_ir ? pass->fn.run_ir(ctx->ir) : pass->fn.run(ctx)`. Keep the
  :63-75 invalidation comment block.
- Table entries with trivial thunks switch to the bare function. Delete a thunk
  only after a per-symbol grep shows table-only references (e.g.
  `tcc_ir_opt_sl_forward_ex` is also the memory-group trigger at
  `pipeline_table.c:159` — convert that to a direct call inside the trigger
  cascade first). KEEP the 6 gen-table adapters in `gen_adapters.c` (they bind
  table+count and are called directly from `function_pipeline.c:90-121`,
  `pipeline_table.c:54`, `regalloc.c`) and any `_ex` that consumes ctx for real.
- Update tests calling deleted `_ex` symbols (`test_opt_dce.c`,
  `test_opt_fusion.c`, `test_opt_pipeline_orchestration.c`).
- `TCC_DISABLE_PASS` is unaffected (name check at `pipeline_run.c:198` precedes
  dispatch).

### C — DSL v2: slim rule codegen (~25-40 KB, the big lever)

New macros beside the old in `framework/opt_dsl.h`/`opt_dsl_helpers.h`; old macros
deleted only in C5. `GUARD`/`when`/`and`/`and_not`/`mk_imm` and the rule-source
altitude survive unchanged.

```c
/* MATCH opens the body: binds ir/q + guard flag only. */
#define MATCH() \
  TCCIRState *ir = ctx->ir; \
  IRQuadCompact *q = &ir->compact_instructions[i]; \
  int _opt_dsl_guard_ok = 1; (void)_opt_dsl_guard_ok

/* Per-operand binders; constraint fused into the bind. */
#define BIND(name)          IROperand name = tcc_ir_op_get_##name(ir, q)
#define BIND_IMM(name)      BIND(name); if (!irop_is_immediate(name)) return 0
#define BIND_VREG(name)     BIND(name); if (irop_get_vreg(name) < 0) return 0
#define BIND_STACKOFF(name) BIND(name); if (irop_get_tag(name) != IROP_TAG_STACKOFF) return 0

/* Direct-emission rewrite: args are comma-joined setter expressions. */
#define new_op(x)   ((void)(q->op = (x)))
#define set_dest(v) tcc_ir_set_dest(ir, i, (v))
#define set_src1(v) tcc_ir_set_src1(ir, i, (v))
#define set_src2(v) tcc_ir_set_src2(ir, i, (v))
#define REWRITE2(...) do { __VA_ARGS__; return 1; } while (0)
```

Example — `sr_udiv` (`ssa/scalar/strength.c:46-55`):

```c
OPT_GEN_SSA(sr_udiv, TCCIR_OP_UDIV) {
  int shift = 0;
  MATCH();
  BIND_IMM(src2);
  GUARD(when(is_power_of_2((uint32_t)src2.u.imm32, &shift)));
  REWRITE2(new_op(TCCIR_OP_SHR), set_src2(mk_imm(shift)));
}
```

Steps:

1. **C1 — prototype on `strength.c`** (3 rules, object 2,420 B). Rebuild that one
   object with device flags, measure with `arm-none-eabi-size`.
   **Go/no-go gate: expect ≤ ~1,000 B (~60% reduction).**
2. **C2 — `ssa/scalar/fold.c`** (25 rules; 28.6 KB of its 32.5 KB object is rule
   dispatches). Measure again.
3. **C3 — remaining SSA DSL files**; convert `framework/example_strength.c` last
   (it is compile-check-only, not linked — it documents the new style).
4. **C4 — flat DSL files** (`flat/scalar/bool.c`, `known_bits.c`,
   `var_tmp_fwd.c`, `flat/fusion/disp.c`, `pair_reorder.c`, `indexed_chain.c`, …
   — the 19 files matching `OPT_GEN_SSA(|OPT_GEN_FLAT(`).
5. **C5 — delete** `PATTERN`/`REWRITE`/`OPT_DSL_APPLY_REWRITE`/
   `OPT_DSL_CHECK_CONSTRAINT` + dead spec structs in `opt_dsl_types.h`; update
   `docs/optimizations/opt_dsl_framework.md` and `framework/README.md`.

**Per-rule audit trap (the one real semantic risk)**: old `OPT_DSL_APPLY_REWRITE`
silently skips spec operands whose *runtime* tag is `IROP_TAG_NONE`. Classify each
REWRITE operand: statically-always-valid → plain setter; dynamically-optional →
guarded `set_*_if(v)` variant. All three strength rules are guard-validated; expect
the optional case to be rare, but check every rule. Scope/`#undef` the short setter
names inside the DSL header (as `when/and` already are).

### D1 — unify the SSA drivers (~1.5-2 KB, one schedule to edit)

Table-driven runner in `ssa/engine/driver.c`:

```c
typedef struct SSAPassEnt { const char *name; int (*fn)(IRSSAOptCtx *);
                            uint16_t flag_offset; uint8_t min_opt; } SSAPassEnt;
static int ssa_run_list(IRSSAOptCtx *ctx, const SSAPassEnt *t, int n);
```

`ssa_run_list` performs pass_disabled + `TCC_PASS_TIMED` + dbg + dump per entry —
**including `tcc_ir_dump_after_pass` for gate-skipped passes, exactly as
`SSA_RUN` does today** (golden pass-name snapshots depend on it).
`tcc_ir_ssa_opt_run` keeps its loop/tail structure over `ssa_main_passes[]`; a new
`tcc_ir_ssa_opt_run_nonpromotable(ctx)` embodies the `regalloc.c` clone verbatim
over `ssa_nonpromotable_passes[]` (guard_collapse stays a bare in-loop call,
bool_norm+dce tail, `run_target` at the end). `ir/regalloc.c:5225-5283` collapses
to one call; the `RUN_SSA` macro dies. **Do NOT converge the two orders** — that
is a semantic change.

### D3 — header hygiene (with D1, tiny)

Rename the SSA `static inline is_power_of_2(uint32_t, int*)`
(`ssa/include/opt/ssa/ssa_opt_helpers.h:88`) → `is_pow2_shift` (3 consumers:
`strength.c`, `narrow.c`, `example_strength.c`; the flat `int is_power_of_2(int64_t)`
in `opt_utils.h:53` keeps the name — more callers). Then delete `narrow.c:20-28`'s
hand-copied prototypes and include `opt_utils.h`. If C1 lands first, fold the
rename into the C sweep of the same files.

### D4 — framework header-inline bodies to a shared TU (~2 KB)

New `source/opt/framework/opt_dsl_runtime.c` (wire into the build beside the other
`source/opt` objects): out-of-line `opt_ssa_domwalk` (3 header copies:
`gvn.c`/`cmp_eq.c`/`vrp.c`), `opt_dsl_pair_match`/`opt_dsl_pair_retire` (x3), phi
helpers (x2). Keep hot one-liners (`opt_dsl_drop_use`/`add_use`) inline.

### D2 — shared scan utilities (timeboxed, exact-duplicates only)

Add to `opt_utils.h` + `util/vreg_query.c` (home of the once-used
`tcc_ir_vreg_has_single_def`, which gains callers instead of dying):

- `IR_FOR_EACH_INSTR_REV(ir, i)` backward NOP-skip macro (17 hand-rolled sites;
  no util exists today);
- `tcc_ir_vreg_use_count`, `tcc_ir_vreg_ever_written`, promoted from the verified
  clones (`flat/scalar/cmp_expr_fold.c:19 ir_opt_vreg_use_count`,
  `ssa/scalar/setif_or_taut.c so_vreg_ever_written`,
  `ssa/scalar/cmp_offset_fold.c:68 co_vreg_ever_written`).

**Rule: replace a private static only after diffing it against the canonical body —
these scans differ subtly (lval handling, call-clobber treatment, NOP handling);
a "close enough" merge is a miscompile.** Non-exact duplicates stay put with a
comment naming the delta. Stop when the exact-duplicate well runs dry (~1-3 KB).

---

## Commit boundaries and validation

| # | Commit | Golden-IR | Validation |
|---|--------|-----------|------------|
| 1 | A: dead driver code + named groups + test updates | byte-identical | host `make test`; smoke only if golden diffs (it must not) |
| 2 | B: IROptPass union+takes_ir, drop `invalidates`, delete thunks | byte-identical | full ladder + size; `rm -rf armv8m-source` |
| 3 | C1: v2 macros + strength.c + measurement | byte-identical | host + golden + object-size go/no-go |
| 4 | C2: fold.c | byte-identical | full ladder + size |
| 5 | C3+C4 (splittable per directory) | byte-identical | host+golden per sub-commit; full ladder at end |
| 6 | C5: delete old DSL machinery + docs | n/a | host `make test` |
| 7 | D1+D3: SSA driver tables + is_pow2_shift + narrow.c fix | byte-identical | full ladder |
| 8 | D4: framework TU | byte-identical | host + golden + smoke |
| 9 | D2: scan utils (timeboxed) | byte-identical | full ladder |

Non-negotiables:

- Host `make test` must use the TEST configure, not the yasos-sysroot one (two
  incompatible configures exist); green = 13,927/0.
- Full QEMU smoke: `./scripts/run_qemu_smoke.sh --rebuild-rootfs`; canonical
  13,364 passed / 216 skipped / 0 failed. Device ir_tests where the ladder says so.
- Size: `arm-none-eabi-size -A bin/armv8m-tcc.elf`, identical build command for
  baseline and candidate; record per-commit deltas. The `yasos_smoke.yml` size-gate
  budget literal stays (total delta is well under the 100 KB ratchet threshold).
- Any header/struct edit under `source/opt` or `ir/` ⇒ `rm -rf armv8m-source`
  before the device rebuild (stale headers produce phantom failures).
- Every restructured runner keeps the `tcc_ir_opt_pass_disabled` name check and
  `tcc_ir_dump_after_pass` emission; `CASCADE_END`/generation-bump semantics are
  untouched by every chunk — which is exactly why byte-identical golden runs can
  be demanded throughout.

## Related documents

- `docs/plan_legacy_flat_ir_ssa_retire.md` — completed retirement (why flat stays)
- `docs/optimizations/duplication_analysis.md` — per-pair "not removable" verdicts
- `docs/optimizations/opt_dsl_framework.md` — DSL docs (update in C5)
- `docs/plans/ssa_opt_loop_consolidation.md` — superseded by this plan

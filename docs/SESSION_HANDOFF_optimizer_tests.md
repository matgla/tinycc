# Session handoff — tinycc optimizer-pass unit tests

Paste the "Kickoff prompt" below into a fresh session. Everything it references is in the repo.

---

## Context / goal
Part of the tinycc flash-size-reduction effort: before merging the legacy linear-IR optimizer
into the SSA optimizer, every optimization pass needs isolated host unit tests so the merge can't
silently regress codegen. Plan: `libs/tinycc/docs/plan_optimizer_test_coverage.md`. Live tracker:
`libs/tinycc/tests/unit/PASS_COVERAGE.md`. Harness how-to memory: `yasos-tcc-optpass-unit-test-harness`.

**Working rules (hard):** write tests in parallel to bug-fixing; **do NOT modify production code**
(anything under `ir/`, or root `tcc*.c`/`*.h`). If a test surfaces a bug, record it under *Findings*
in PASS_COVERAGE.md with a minimal repro — do not fix it.

## State at handoff
- Harness foundation done: `tests/unit/arm/armv8m/ir_build.h` (hand-built IR), `UT_COVERS()` in `ut.h`,
  Makefile uses `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` to pull `irop_config`
  out of heavy `ir/core.c` without dragging frontend symbols.
- Shared base modules already wired into the Makefile: `ir/opt.c`, `ir/opt_alias.c`, `ir/cfg.c`,
  plus a `get_tok_str` stub in `stubs.c`.
- `make ut` is GREEN at **524 tests**. Only `opt_neg_chain` (4) among the new suites is registered.
- **10 more suites (65 tests) are written and each was self-verified GREEN in isolation, but are NOT
  yet registered** in `test_main.c` / the Makefile, so they don't run in `make ut`/`make test` yet.
- `make test` runs `ut` as a prerequisite (Makefile:667) and a unit-test failure fails `make test`.

Test files on disk (all under `tests/unit/arm/armv8m/`): test_opt_neg_chain.c *(registered)*,
test_opt_knownbits.c, test_opt_copyprop.c, test_opt_cmp_fuse.c, test_opt_cmpfold.c,
test_opt_constprop.c, test_opt_constfold.c, test_opt_licm.c, test_opt_jump_thread.c,
test_opt_setif_or_taut.c, test_opt_dead_lea_store.c.

## IMMEDIATE TASK — register the 10 verified suites (mechanical)
1. **stubs.c**: add `void *elfsym(void *s){ (void)s; return 0; }` (needed by `ir/opt_dce.c` for the cmpfold suite; benign for hand-built IR).
2. **Makefile `UT_MODULE_SRCS`** — add (de-duplicated):
   `ir/opt_knownbits.c ir/opt_copyprop.c ir/opt_cmp_fuse.c ir/opt_constprop.c ir/opt_du.c ir/opt_constfold.c ir/licm.c ir/opt_jump_thread.c ir/opt_setif_or_taut.c ir/opt_dead_lea_store.c ir/opt_dce.c`
3. **Makefile `UT_LOCAL_SRCS`** — add the 10 `test_opt_*.c` files listed above.
4. **test_main.c** — `UT_DECLARE_SUITE` + `UT_RUN_SUITE` for: opt_knownbits, opt_copyprop, opt_cmp_fuse,
   opt_cmpfold, opt_constprop, opt_constfold, opt_licm, opt_jump_thread, opt_setif_or_taut, opt_dead_lea_store.
5. `make ut` → expect **~589 tests, 0 failed**.

### Gotchas for the combined link (the real test is the union build)
- Each suite was verified individually against the same base, so the union should resolve — but the
  combined `make ut` is the proof. If it fails, it's almost certainly a **duplicate symbol** or an
  **undefined ref** not seen per-suite.
- `test_opt_constfold.c` defines 4 frontend link-stubs in-file (`global_stack`, `sym_push2`,
  `external_global_sym`, `tok_alloc_const`). If another newly-linked module also defines one, you'll get
  a duplicate-symbol error — move those stubs to `stubs.c` (shared) to resolve.
- The session-scratch self-verify tool (`verify_suite.sh` + `utbase/`) lived in `/tmp/.../scratchpad/`
  and is **gone in a new session**. Don't look for it — just use `make ut` to verify the integrated build.
  (If you want per-suite isolation again, the recipe is in PASS_COVERAGE.md and the harness memory.)

## Findings so far
- **No production bugs found.** All suites are *characterization* tests (expectations derived from the
  code) — a good regression baseline for the merge, but weak at *finding* bugs. The smoke/QEMU suite
  remains the stronger oracle.
- One harness limitation: name-gated constfold passes (`self_copy_elim`, `float_narrowing`) can't reach
  their positive fold because the `get_tok_str` stub returns a constant — they're covered as guard-only.

## After integration — phases (see PASS_COVERAGE.md for the full list)
- **Highest bug-finding value:** legacy-vs-SSA **equivalence harness** (run both pass paths on the same IR,
  assert identical results) — Phase F. Also spec-oracle asserts (compute expected fold in the test) and
  edge/fuzz inputs (overflow, width boundaries, merge points, back-edges).
- Phase C: SSA passes (`ssa_opt_*`) via golden-IR snapshots.
- Phase D: codegen size levers via objdump pattern/count (R9 spills, b.w→b.n, cbz).
- Phase E: `check_pass_coverage.py` ledger gating `UT_COVERS` vs registered passes in CI.
- Phase F: remaining uncovered entries in multi-pass files (copyprop/constprop/constfold ~15 more);
  deferred: `opt_switch_data` (needs section/codegen state).

---

## Kickoff prompt (paste into the new session)
> Continue the tinycc optimizer-pass unit-test work. Read `libs/tinycc/tests/unit/PASS_COVERAGE.md`
> and `libs/tinycc/docs/SESSION_HANDOFF_optimizer_tests.md` first. Do NOT modify production code; if a
> test reveals a bug, record it under *Findings* in PASS_COVERAGE.md, don't fix it.
> Immediate task: register the 10 already-written, isolation-verified suites into the build — add the
> `elfsym` stub to stubs.c, add the pass sources to `UT_MODULE_SRCS`, add the test files to
> `UT_LOCAL_SRCS`, and declare+run the 10 suites in test_main.c. Then run `make ut` and confirm it goes
> from 524 to ~589 tests, 0 failed (watch for duplicate-symbol / undefined-ref issues in the combined
> link; if the 4 frontend stubs in test_opt_constfold.c collide, move them to stubs.c). Update
> PASS_COVERAGE.md to mark the suites integrated. After that, propose the legacy-vs-SSA equivalence
> harness (Phase F) as the next bug-finding step.

# Optimizer + codegen test-coverage tracker

Progress tracker for the per-pass / codegen unit-test effort described in
`docs/plan_optimizer_test_coverage.md`. This file is the source of truth for
"what is covered" until `check_pass_coverage.py` (Phase E) automates the diff.

**Working rules** (per user, 2026-06-26):
- Write tests in parallel to the bug-fixing work. **Do NOT change production code.**
- If a test surfaces a bug, do **not** fix it here — record it under *Findings* below
  (with a minimal repro) and leave the test as a documented expectation.
- Each suite annotates the pass it covers with `UT_COVERS("<pass>")` so the future
  ledger script can enumerate coverage automatically.

Legend: `[x]` done · `[~]` written+verified-in-isolation, NOT yet registered in build · `[ ]` todo · `[!]` blocked / bug found

---

## STATUS SNAPSHOT (2026-06-26)

- **11 suites, 69 tests** written this session. **All 11 suites are now integrated** and run in `make ut` (**589 tests, 0 failed**).
- Harness foundation (Phase A) complete. Shared base modules already wired into the Makefile: `ir/opt.c`, `ir/opt_alias.c`, `ir/cfg.c` + `get_tok_str`, `elfsym`, and frontend symbol-table stubs in `stubs.c`.
- **No production bugs found** by any suite — all asserts pin current (correct) behavior. These are regression/characterization guards for the upcoming legacy→SSA optimizer merge.

The session-scratch self-verify harness (`verify_suite.sh` + `utbase/`) was temporary and is no longer present; the integrated `make ut` build is the source of truth.

---

## Phase A — Harness foundation
- [x] `ir_build.h` — hand-built IR builder (`utb_new/emit/temp/imm/...`) for isolated pass tests
- [x] `ut.h` — `UT_COVERS("<pass>")` annotation macro
- [x] Makefile — link opt passes via `--gc-sections` (isolates `irop_config` from heavy `core.c`)
- [x] Shared base modules wired: `ir/opt.c` (pass timing + `tcc_ir_find_defining_instruction`), `ir/opt_alias.c` (`ir_opt_store_btype_size_bytes`), `ir/cfg.c` (dominators), `get_tok_str` stub
- [ ] `UT_ASSERT_STREQ` in `ut.h` (only needed once golden/snapshot asserts land)

### How the isolated harness links (for future suites)
A pass `int tcc_ir_opt_<name>(TCCIRState*)` links against: its own TU + the prebuilt base
(`core.c` for `irop_config`, `opt_utils.c`, `opt.c`, `opt_alias.c`, `cfg.c`, `tccir_operand.c`,
`pool/type/vreg`, stubs). `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` GC core.c's
unreferenced frontend functions, so per-pass module deps stay tiny. Build/run: `make ut`.

---

## Phase B — Tier-1 legacy passes
| Suite | File | Pass(es) covered | Tests | Extra module srcs to add | Status |
|---|---|---|---|---|---|
| opt_neg_chain | test_opt_neg_chain.c | neg_chain_cse | 4 | ir/opt_neg_chain.c | **[x] integrated** |
| opt_knownbits | test_opt_knownbits.c | known_bits | 7 | ir/opt_knownbits.c | **[x] integrated** |
| opt_copyprop | test_opt_copyprop.c | copy_prop | 9 | ir/opt_copyprop.c | **[x] integrated** |
| opt_cmp_fuse | test_opt_cmp_fuse.c | cmp_field_fuse | 5 | ir/opt_cmp_fuse.c | **[x] integrated** |
| opt_cmpfold | test_opt_cmpfold.c | cmp_expr_fold, cmp_const_offset_fold, cmp_field_fuse | 5 | ir/opt_constprop.c, ir/opt_du.c, ir/opt_cmp_fuse.c, ir/opt_dce.c + `elfsym` stub | **[x] integrated** |
| opt_constprop | test_opt_constprop.c | const_var_prop, const_prop | 11 | ir/opt_constprop.c, ir/opt_du.c | **[x] integrated** |
| opt_constfold | test_opt_constfold.c | self_copy_elim, float_narrowing | 6 | ir/opt_constfold.c (frontend stubs moved to `stubs.c`) | **[x] integrated** (see Finding #1) |
| opt_licm | test_opt_licm.c | licm | 4 | ir/licm.c | **[x] integrated** (real hoist positive) |
| opt_jump_thread | test_opt_jump_thread.c | jump_threading, eliminate_fallthrough | 8 | ir/opt_jump_thread.c | **[x] integrated** |
| opt_setif_or_taut | test_opt_setif_or_taut.c | setif_or_tautology | 5 | ir/opt_setif_or_taut.c | **[x] integrated** |
| opt_dead_lea_store | test_opt_dead_lea_store.c | dead_lea_store_elim | 5 | ir/opt_dead_lea_store.c | **[x] integrated** |

### >>> Integration complete <<<
All 10 verified suites are registered in `stubs.c`, the Makefile (`UT_MODULE_SRCS` / `UT_LOCAL_SRCS`), and `test_main.c`. `make ut` reports **589 tests, 0 failed**.

Notes from the combined link:
- The 4 frontend link stubs originally inside `test_opt_constfold.c` (`global_stack`, `sym_push2`, `external_global_sym`, `tok_alloc_const`) were moved to `stubs.c` so the union link has a single definition.
- The `elfsym` stub was added to `stubs.c` for `ir/opt_dce.c` (needed by the cmpfold suite).

---

## Phase B2 — Corner-case unit tests — NEXT (user priority, before bug-hunting tracks)
See `docs/plan_corner_case_tests.md`. Drive each of the 11 passes into its edge cases (boundaries,
overflow/UB-shaped inputs, widths/signedness, degenerate IR, control-flow/lval/memory corners,
idempotence, ASAN robustness). Arithmetic ★ cases assert independently-computed values, so they can
*find* bugs, not just characterize.
- [ ] §B harness extensions first (serial): `utb_emit4`, symref/stackoff/flag helpers, fixpoint helper, **settable `get_tok_str` table** (resolves Finding #1)
- [ ] Per-pass corner-case fan-out (parallel, one agent per `test_opt_*.c`)
- [ ] ASAN run of the unit binary over the new tests

## Phase C — Tier-2 SSA passes (golden-IR snapshots, host, no QEMU) — TODO
- [ ] `ssa_opt_branch` (`instr_to_block` bounds), `ssa_opt_fold`, `ssa_opt_sccp`, `ssa_opt_cprop`, `ssa_opt_gvn`, `ssa_opt_load_cse`, `ssa_opt_narrow`

## Phase D — Tier-3 codegen size levers (objdump pattern/count, host) — TODO
- [ ] R9 GOT-base spill elimination; `b.w`→`b.n` narrowing; cbz/cbnz fusion; struct by-value 9-byte packed operand; wide-string-literal merge

## Phase BH — Real bug hunting (independent oracles) — PLANNED, do before Phase F
See `docs/plan_bug_hunting.md`. Characterization tests (Phase B) don't *find* bugs; these do, via
independent oracles. Recommended order:
- [ ] Track 1 — ASAN/UBSan corpus sweep (memory-safety class; proven, ~1d)
- [ ] Track 2 — O-level self-consistency differential (miscompile class; ~2-3d)
- [ ] Track 4 — IR metamorphic / semantics-preservation fuzzer (per-pass, host, localizing; flagship — also the substrate for Phase F)
- [ ] Track 3 — differential vs arm-none-eabi-gcc (wrong-at-all-levels class; ~2-3d)

## Phase E — Ledger + CI gate — TODO
- [ ] `check_pass_coverage.py` — enumerate `PASS`/`PASS_GATED` in `ir/opt_pipeline.c` vs `UT_COVERS` markers; wire into CI (soft-fail first)

## Phase F — Remaining registered passes + legacy↔SSA equivalence harness — TODO
Many multi-pass files only partially covered. Still uncovered entries include:
- opt_copyprop.c: `cse_global_load`, `globalsym_cse`, `cse_param_add`, `local_load_cse`, `local_alu_cse`, `bool_cse`
- opt_constprop.c: `global_init_prop`, `symref_const_prop`, `complex_const_param_fold`, `value_tracking`
- opt_constfold.c: `const_string_calls`, `const_call_replace`, `switch_call_replace`, `param_addrof_const_fold`, `local_addrof_const_fold` (these pull frontend symbols — need stubs/harness work)
- opt_jump_thread.c covered; remaining small passes: opt_loop_dead, opt_reroll, opt_bitfield, opt_const_aggregate, opt_dead_vla, opt_switch_data (deferred), opt_gens_*, opt_hash, opt_xform, opt_engine, opt_setif_or_taut covered, ...
- [ ] Equivalence harness (old legacy path vs new SSA path on a `.c` corpus)

---

## Findings (DO NOT fix here — hand off to bug-fix work)

1. **Harness limitation (not a pass bug): name-gated passes can't exercise their positive fold.**
   `self_copy_elim` and `float_narrowing` (opt_constfold.c) decide solely on the callee name from
   `get_tok_str(callee->v)` (e.g. needs "memcpy" / "__aeabi_f2d"). The base `get_tok_str` stub returns
   a constant `"?"`, so the fold is structurally unreachable — these suites only assert the pass correctly
   *declines* (non-vacuous guard tests). To test the real fold, give the harness a settable token-name
   table (a `get_tok_str` that reads a test-populated map) or real token state. Same blocker for the
   `*addrof_const_fold` / `*_string_calls` / `*_call_replace` constfold passes (Phase F).

2. **No production bugs found.** All 9 agent suites + cmpfold assert current behavior and pass, including
   the Tier-1 historical bug-class guards (copyprop `is_lval`/DEREF preservation; cmp_fuse `is_lval` base
   guard; knownbits narrow-load mask/sign-extend; licm hoist-only-to-dominating-preheader; jump-thread
   backward-edge guard). These now act as regression guards for the legacy→SSA optimizer merge.

## Deferred
- **opt_switch_data** (`switch_to_data`, `switch_collapse`): needs ELF/section + frontend state
  (`get_sym_ref`, `greloc`, `int_type`, `section_add`) — not a clean IR-only unit test. Revisit with a
  section/codegen stub layer or move to an integration-level test.

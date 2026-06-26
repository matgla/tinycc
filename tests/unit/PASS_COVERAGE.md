# Optimizer + codegen test-coverage tracker

Progress tracker for the per-pass / codegen unit-test effort described in
`docs/plan_optimizer_test_coverage.md`. This file is the source of truth for
"what is covered" until `check_pass_coverage.py` (Phase E) automates the diff.

**Working rules** (per user, 2026-06-26):
- Test-writing phase (done): wrote tests in parallel without changing production code; suspected bugs
  were pinned as documented expectations under *Findings*.
- Bug-fix phase (in progress): the genuine correctness/robustness findings (#3, #4, #7, #9, #10) are now
  fixed in production and their tests flipped to assert correct behavior. Missed-optimization findings
  (#5, #6, #8) are left as characterizations.
- Each suite annotates the pass it covers with `UT_COVERS("<pass>")` so the future
  ledger script can enumerate coverage automatically.

Legend: `[x]` done · `[~]` written+verified-in-isolation, NOT yet registered in build · `[ ]` todo · `[!]` blocked / bug found

---

## STATUS SNAPSHOT (2026-06-26)

- **11 optimizer suites, 260 tests** written/updated this session. **All 11 suites are now integrated** and run as part of the full `make ut` binary (**42 suites, 780 tests, 0 failed**).
- Harness foundation (Phase A) complete. Shared base modules already wired into the Makefile: `ir/opt.c`, `ir/opt_alias.c`, `ir/cfg.c` + `get_tok_str`, `elfsym`, and frontend symbol-table stubs in `stubs.c`.
- **Phase B2 complete**: harness extensions + per-pass corner-case fan-out landed (191 new tests since the original Phase B baseline). `make ut` reports **780 tests, 0 failed**; ASAN run is clean (0 errors/leaks). Several suspected production bugs were found and pinned as documented expectations; see *Findings* below.
- The tracker now also itemizes the non-optimizer suites: 4 infrastructure/harness suites and 27 Thumb instruction-encoding (`thop_*`) suites (see below).
- Several suspected production bugs were found by the new corner-case tests and are pinned as documented expectations (see *Findings* #3–#10). All other asserts pin current (correct) behavior, acting as regression/characterization guards for the upcoming legacy→SSA optimizer merge.
- **Phase C complete (green)**: `test_golden_ir.py` + 7 SSA golden cases + debug compiler path. The SSA optimizer driver is now wired into the `-dump-ir-passes=` machinery, so all 7 SSA cases emit `=== AFTER ssa:<pass> ===` blocks and assert real golden IR. **8 passed, 0 xfailed** (see *Findings* #11, resolved).
- **Phase D harness landed**: `test_codegen_asm.py` + 5 codegen size-lever characterization tests. Currently **5 passed**, locking the pre-optimization behavior of R9 spills, wide forward branches, disabled CBZ/CBNZ fusion, unmerged wide-string literals, and the 9-byte packed-struct by-value path (see *Findings* #12).
- **Phase BH implemented (all four oracle tracks landed) — and it found real bugs.** The harnesses from
  `docs/plan_bug_hunting.md` are now built and validated, test/tooling-only (no production edits). Headline:
  the bug-hunters immediately surfaced **three independent miscompile/robustness classes**:
  - **Track 1** (`scripts/asan_sweep.sh` + `asan_sweep.py`): ASAN/LeakSanitizer corpus sweep. Found
    **4 compile-time memory leaks** (the known `decl_initializer_alloc` frontend leak, a second
    compound-literal path into it, an IR-codegen leak on `asm goto`, and a `unary_funcall` leak). See Finding #14.
  - **Tracks 2/3** (`tests/fuzz/gen_c.py` + `scripts/diff_olevels.py` / `diff_vs_gcc.py` + 2 pytest wrappers):
    UB-free random-C differential under live QEMU. Found **7 confirmed tcc optimizer miscompiles** in
    seeds 0–40 (~17%): tcc `-O0` matched `arm-none-eabi-gcc -O2` but `-O1`/`-O2` diverged. See Finding #15 (fixed).
  - **Track 4** (`ir_eval.h` + `ir_gen.h` + `test_metamorphic.c`, integrated into `make ut`): host IR
    metamorphic fuzzer with a reference interpreter + delta-reducer. Found **2 `known_bits` miscompiles**
    in `kb_const_compute` (ZEXT-to-64 sign-extension; 32-bit logical-SHR width), minimal-IR reduced. See Finding #16.
  The bug-fix pass is now **underway**: Finding **#16** (`known_bits` ZEXT/SHR) is **FIXED** and its tests
  flipped to assert correct behavior; Finding **#14** (4 compile-time leaks) is **FIXED** and its focused
  repros are LeakSan-clean; **#15** (7 random-C miscompiles) is **FIXED** and the `KNOWN_DIVERGENCES`
  xfails were removed. `make ut` stays green (**0 failed**) with the metamorphic suite
  (7 interpreter self-checks + a 76,800-check semantics-preservation sweep, 0 mismatches).

The session-scratch self-verify harness (`verify_suite.sh` + `utbase/`) was temporary and is no longer present; the integrated `make ut` build is the source of truth.

---

## Phase A — Harness foundation
- [x] `ir_build.h` — hand-built IR builder (`utb_new/emit/temp/imm/...`) for isolated pass tests
- [x] `ut.h` — `UT_COVERS("<pass>")` annotation macro
- [x] Makefile — link opt passes via `--gc-sections` (isolates `irop_config` from heavy `core.c`)
- [x] Shared base modules wired: `ir/opt.c` (pass timing + `tcc_ir_find_defining_instruction`), `ir/opt_alias.c` (`ir_opt_store_btype_size_bytes`), `ir/cfg.c` (dominators), `get_tok_str` stub
- [x] `UT_ASSERT_STREQ` in `ut.h` (NULL-safe `strcmp` assert for golden/snapshot string compares)

### How the isolated harness links (for future suites)
A pass `int tcc_ir_opt_<name>(TCCIRState*)` links against: its own TU + the prebuilt base
(`core.c` for `irop_config`, `opt_utils.c`, `opt.c`, `opt_alias.c`, `cfg.c`, `tccir_operand.c`,
`pool/type/vreg`, stubs). `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` GC core.c's
unreferenced frontend functions, so per-pass module deps stay tiny. Build/run: `make ut`.

---

## Phase B — Tier-1 legacy passes
| Suite | File | Pass(es) covered | Tests | Extra module srcs to add | Status |
|---|---|---|---|---|---|
| opt_neg_chain | test_opt_neg_chain.c | neg_chain_cse | 17 | ir/opt_neg_chain.c | **[x] integrated** |
| opt_knownbits | test_opt_knownbits.c | known_bits | 29 | ir/opt_knownbits.c | **[x] integrated** |
| opt_copyprop | test_opt_copyprop.c | copy_prop | 24 | ir/opt_copyprop.c | **[x] integrated** |
| opt_cmp_fuse | test_opt_cmp_fuse.c | cmp_field_fuse | 19 | ir/opt_cmp_fuse.c | **[x] integrated** |
| opt_cmpfold | test_opt_cmpfold.c | cmp_expr_fold, cmp_const_offset_fold, cmp_field_fuse | 30 | ir/opt_constprop.c, ir/opt_du.c, ir/opt_cmp_fuse.c, ir/opt_dce.c + `elfsym` stub | **[x] integrated** |
| opt_constprop | test_opt_constprop.c | const_var_prop, const_prop | 50 | ir/opt_constprop.c, ir/opt_du.c | **[x] integrated** |
| opt_constfold | test_opt_constfold.c | self_copy_elim, float_narrowing | 19 | ir/opt_constfold.c (frontend stubs moved to `stubs.c`) | **[x] integrated** (positive self-copy fold now reachable via `utb_set_tok_str`) |
| opt_licm | test_opt_licm.c | licm | 13 | ir/licm.c | **[x] integrated** (real hoist positive) |
| opt_jump_thread | test_opt_jump_thread.c | jump_threading, eliminate_fallthrough | 22 | ir/opt_jump_thread.c | **[x] integrated** |
| opt_setif_or_taut | test_opt_setif_or_taut.c | setif_or_tautology | 21 | ir/opt_setif_or_taut.c | **[x] integrated** |
| opt_dead_lea_store | test_opt_dead_lea_store.c | dead_lea_store_elim | 16 | ir/opt_dead_lea_store.c | **[x] integrated** |

### >>> Integration complete <<<
All 11 verified suites are registered in `stubs.c`, the Makefile (`UT_MODULE_SRCS` / `UT_LOCAL_SRCS`), and `test_main.c`. `make ut` reports **780 tests, 0 failed**.

Notes from the combined link:
- The 4 frontend link stubs originally inside `test_opt_constfold.c` (`global_stack`, `sym_push2`, `external_global_sym`, `tok_alloc_const`) were moved to `stubs.c` so the union link has a single definition.
- The `elfsym` stub was added to `stubs.c` for `ir/opt_dce.c` (needed by the cmpfold suite).

---

## Phase B2 — Corner-case unit tests — IN PROGRESS (user priority, before bug-hunting tracks)
See `docs/plan_corner_case_tests.md`. Drive each of the 11 passes into its edge cases (boundaries,
overflow/UB-shaped inputs, widths/signedness, degenerate IR, control-flow/lval/memory corners,
idempotence, ASAN robustness). Arithmetic ★ cases assert independently-computed values, so they can
*find* bugs, not just characterize.
- [x] §B harness extensions first (serial): `utb_emit4`, `utb_symref`/`utb_stackoff`, flag helpers (`utb_lval`/`utb_unsigned`/`utb_llocal`), `utb_run_to_fixpoint`, `utb_assert_wellformed`, **settable `get_tok_str` table** (resolves Finding #1)
- [x] Per-pass corner-case fan-out (parallel, one agent per `test_opt_*.c`) — 191 new tests added; `make ut` green at 780 tests
- [x] ASAN run of the unit binary over the new tests — clean (0 errors, 0 leaks after harness fix)

---

## Integrated non-optimizer suites

The binary registered in `test_main.c` also contains the harness/infrastructure
and Thumb instruction-encoding suites below. They do not use `UT_COVERS(...)`
(they are not pipeline-registered optimizer passes), but they are part of the
same `make ut` gate and are tracked here so `PASS_COVERAGE.md` reflects the whole
unit binary.

### Infrastructure suites
| Suite | File | Coverage | Tests | Status |
|---|---|---|---|---|
| chained_hash | `test_chained_hash.c` | `tcc-chained-hash.h` init/insert/lookup/grow/remove | 8 | [x] integrated |
| ir_pool | `test_ir_pool.c` | `ir/pool.c` operand-pool add/get + growth | 5 | [x] integrated |
| ir_type | `test_ir_type.c` | `ir/type.c` VT_* / IR-op type predicates | 11 | [x] integrated |
| ir_vreg | `test_ir_vreg.c` | `ir/vreg.c` vreg + live-interval management | 8 | [x] integrated |

### Thumb instruction-encoding suites (`thop_*`)
| Suite | File | Coverage | Tests | Status |
|---|---|---|---|---|
| thop_adr | `test_thop_adr.c` | ADR encoding (T1/T3/T4) | 7 | [x] integrated |
| thop_alu_reg | `test_thop_alu_reg.c` | ALU register T1/T2/T3 + shifts | 30 | [x] integrated |
| thop_bitfield | `test_thop_bitfield.c` | BFC/BFI/SBFX/SSAT/USAT | 17 | [x] integrated |
| thop_block | `test_thop_block.c` | PUSH/POP/LDM/STM/LDMDB/STMDB | 20 | [x] integrated |
| thop_branch | `test_thop_branch.c` | BX/BLX/BL/B cond/uncond/CBZ/CBNZ | 15 | [x] integrated |
| thop_cmp | `test_thop_cmp.c` | CMP/CMN/TST/TEQ (T16/T32) | 14 | [x] integrated |
| thop_constraints | `test_thop_constraints.c` | `thop_emit` constraint engine: masks, equality, regsets, encoding, features, S-bit/IT, shifts, PUW, immediates | 76 | [x] integrated |
| thop_extend | `test_thop_extend.c` | SXTH/UXTH/SXTB/UXTB (T1/T2) | 9 | [x] integrated |
| thop_ldaex | `test_thop_ldaex.c` | LDAEX/LDAEXB/LDAEXH/STLEX/STLEXB/STLEXH | 9 | [x] integrated |
| thop_ldrd | `test_thop_ldrd.c` | LDRD/STRD immediate offset | 16 | [x] integrated |
| thop_ldrex | `test_thop_ldrex.c` | LDREX/STREX byte/half variants | 10 | [x] integrated |
| thop_ldr_literal | `test_thop_ldr_literal.c` | LDR literal PC-relative | 8 | [x] integrated |
| thop_mem_exclusive | `test_thop_mem_exclusive.c` | LDA/LDAB/LDAH/STL/STLB/STLH | 11 | [x] integrated |
| thop_mem_imm | `test_thop_mem_imm.c` | LDR/STR immediate offset (T16/T32) | 25 | [x] integrated |
| thop_mem_reg | `test_thop_mem_reg.c` | LDR/STR register offset | 18 | [x] integrated |
| thop_mem_unpriv | `test_thop_mem_unpriv.c` | Unprivileged load/store | 9 | [x] integrated |
| thop_mov | `test_thop_mov.c` | MOV/MOVW/MOVT/shifts | 10 | [x] integrated |
| thop_mrs | `test_thop_mrs.c` | MRS/MSR special register | 7 | [x] integrated |
| thop_mul | `test_thop_mul.c` | MUL/MLA/MLS/UMULL/UMLAL/SMULL/SMLAL/UDIV/SDIV | 18 | [x] integrated |
| thop_mvn | `test_thop_mvn.c` | MVN immediate/register | 18 | [x] integrated |
| thop_pld | `test_thop_pld.c` | PLD/PLDW/PLI | 14 | [x] integrated |
| thop_rev | `test_thop_rev.c` | REV/REV16/REVSH/RBIT | 13 | [x] integrated |
| thop_shift_imm | `test_thop_shift_imm.c` | LSL/LSR/ASR/ROR immediate (T1/T3) | 18 | [x] integrated |
| thop_shift_reg | `test_thop_shift_reg.c` | LSL/LSR/ASR/ROR register (T1/T3) | 16 | [x] integrated |
| thop_system | `test_thop_system.c` | NOP/BKPT/SEV/WFE/WFI/ISB/DSB/DMB/MRS/MSR | 23 | [x] integrated |
| thop_tbb | `test_thop_tbb.c` | TBB/TBH/TT | 11 | [x] integrated |
| thop_vfp | `test_thop_vfp.c` | VFP single/double move/arithmetic | 46 | [x] integrated |

`make ut` totals: **42 suites, 780 tests, 0 failed**.  
*Note:* the `thop_*` suites cover the Thumb encoder layer (`arch/arm/thumb/thop_*.c`), not the higher-level codegen size levers (those remain Phase D).

## Phase C — Tier-2 SSA passes (golden-IR snapshots, host, no QEMU) — COMPLETE
Harness landed and exercised; the SSA driver is now wired into the `-dump-ir-passes=`
machinery so every SSA case asserts a real golden IR snapshot.

- [x] `tests/ir_tests/test_golden_ir.py` — pytest runner; discovers `golden/<pass>/<case>.c` + `.expected`; runs `-dump-ir-passes=<pass>`; diffs the `=== AFTER <pass> ===` block; supports `--update` and `--compiler`
- [x] `tests/ir_tests/conftest.py` — added `--update`, `--compiler` options and `golden_ir` marker
- [x] Debug compiler path: `libs/tinycc/armv8m-tcc.debug` (cross-compiler rebuilt with `CONFIG_TCC_DEBUG`; original `config.mak`/`armv8m-tcc` restored)
- [x] Reference legacy golden case passing: `block_copy_init/clear_struct`
- [x] **SSA driver wired into the dump machinery** (resolves Finding #11): shared
  `tcc_ir_dump_after_pass()`/`tcc_ir_dump_passes_match()` in `ir/dump.c`
  (declared in `tccir.h`); `tccgen.c`'s legacy `dump_ir_after_pass` now delegates
  to it, and both SSA pass sites — the iterative driver (`ir/opt/ssa_opt.c:tcc_ir_ssa_opt_run`)
  and the non-promotable fallback (`ir/regalloc.c`) — emit `=== AFTER ssa:<pass> ===` blocks.

| Pass | Case file | Status | Notes |
|---|---|---|---|
| `ssa:branch` | `golden/ssa:branch/branch_fold.c` | [x] | folds 2nd `if` to a SELECT |
| `ssa:fold` | `golden/ssa:fold/fold_add.c` | [x] | `x+0+1` → `P0 ADD #1` |
| `ssa:sccp` | `golden/ssa:sccp/sccp_loop.c` | [x] | loop const init |
| `ssa:cprop` | `golden/ssa:cprop/copy_chain.c` | [x] | copy chain → `RETURNVALUE P0` |
| `ssa:gvn` | `golden/ssa:gvn/common_expr.c` | [x] | dup `x+y` reuses `T5` |
| `ssa:load_cse` | `golden/ssa:load_cse/repeated_load.c` | [x] | 2nd `*p` → `T8 <- T7` |
| `ssa:narrow` | `golden/ssa:narrow/narrow_add.c` | [x] | short→int add |

Run: `cd libs/tinycc/tests/ir_tests && pytest test_golden_ir.py -v --compiler ../../armv8m-tcc.debug` → **8 passed**.

## Phase D — Tier-3 codegen size levers (objdump pattern/count, host)
Harness and all five lever tests landed. Tests currently *characterize* the pre-optimization codegen; assertions should be flipped as the size-reduction levers land.

- [x] `tests/ir_tests/test_codegen_asm.py` — pytest runner; cross-compiles with `armv8m-tcc -c`; disassembles with `arm-none-eabi-objdump`; asserts per-function mnemonic counts/presence/absence and `.rodata` byte counts
- [x] `tests/ir_tests/asm/r9_spill.c` + `test_r9_spill_around_calls`
- [x] `tests/ir_tests/asm/forward_branch_narrow.c` + `test_forward_branch_conditional_still_wide`
- [x] `tests/ir_tests/asm/cbz_fusion.c` + `test_cbz_fusion_disabled`
- [x] `tests/ir_tests/asm/struct_packed_9byte.c` + `test_struct_packed_9byte_by_value`
- [x] `tests/ir_tests/asm/wide_string_merge.c` + `test_wide_string_literals_not_merged`

Run: `cd libs/tinycc/tests/ir_tests && pytest test_codegen_asm.py -v` → **5 passed**.

## Phase BH — Real bug hunting (independent oracles) — HARNESSES LANDED, BUGS FOUND
See `docs/plan_bug_hunting.md`. This phase actively *finds* latent miscompiles before the
legacy→SSA merge; it does **not** modify production code. All four oracle tracks are now
implemented and validated; the bugs they surfaced are characterized (pinned green / xfail)
and recorded in *Findings* #14–#16. Fixes happen in a separate bug-fix pass.

### BH track status (implemented)

| Track | Test / Harness | File | Oracle | Status | Result |
|---|---|---|---|---|---|
| 1 | ASAN/LeakSan corpus sweep | `scripts/asan_sweep.sh` + `scripts/asan_sweep.py` | sanitizer output | `[x] landed + validated; leaks fixed` | swept tests2/ir_tests/gcc-torture slices; **4 unique compile-time leaks** found and fixed (Finding #14). `--with-ubsan` builds a throwaway UBSan compiler (restores shared `config.mak`). |
| 2 | O-level self-consistency diff | `scripts/diff_olevels.py` + `tests/fuzz/gen_c.py` + `tests/fuzz/fuzz_harness.py` | `-O0`/`-O1`/`-O2` outputs equal (live QEMU) | `[x] landed + validated` | Former 7 miscompiles in seeds 0–40 are fixed; Finding #15. |
| 2a | Random-C O-level smoke | `tests/fuzz/test_random_c_olevels.py` | `-O0`/`-O1`/`-O2` outputs equal | `[x] integrated (pytest)` | asserts correct behavior; `KNOWN_DIVERGENCES` is empty; clean skip if QEMU/newlib absent. |
| 3 | Differential vs `arm-none-eabi-gcc` | `scripts/diff_vs_gcc.py` (+ `reduce_divergence.py`) | gcc output (live QEMU) | `[x] landed + validated` | random mode = gcc oracle; `--mode torture` runs self-checking gcc-execute through tcc (skip-list triaged). |
| 3a | Random-C vs gcc | `tests/fuzz/test_random_c_vs_gcc.py` | gcc output | `[x] integrated (pytest)` | 18 passed / 6 xfailed on the validated run; clean skip without QEMU. |
| 4 | IR metamorphic fuzz (legacy passes) | `tests/unit/arm/armv8m/{ir_eval.h,ir_gen.h,test_metamorphic.c}` | reference IR interpreter (host) | `[x] integrated (make ut)` | interpreter cross-validated; 400 fns × 16 passes × 12 vectors; **2 `known_bits` miscompiles** reduced to minimal IR (Finding #16). Suite green (bugs pinned). |
| 4a | IR metamorphic fuzz (SSA passes) | `tests/unit/arm/armv8m/test_metamorphic_ssa.c` | reference IR interpreter | `[~] registered, honest SKIP` | SSA passes take `IRSSAOptCtx*` (ssa/cfg/dom def-use) — that substrate is not linkable in the isolated unit harness; documented. Enabling it needs an `ssa_build.h` and is the "1-line variation" that also unlocks Phase F. |

Run:
- Track 1: `scripts/asan_sweep.sh --corpus all` (shardable: `--shard i/N`, `--limit N`, `--corpus gcc-torture|tests2|ir_tests`).
- Tracks 2/3: `python scripts/diff_olevels.py --seeds 0-40`; `python scripts/diff_vs_gcc.py --mode random --seeds 0-15`; `pytest tests/fuzz/` (set `ASAN_OPTIONS=detect_leaks=0`).
- Track 4: `make ut` (0 failed; the metamorphic suite's 7 self-checks + 76,800-check sweep run inline).

Each found bug is **characterized, not fixed** (Phase BH rule). The bug-fix pass should root-cause Findings
#14–#16 and flip the pinned `*_SUSPECTED_BUG` / `KNOWN_DIVERGENCES` characterizations to assert correct behavior.

## Phase E — Ledger + CI gate — TODO
- [ ] `check_pass_coverage.py` — enumerate `PASS`/`PASS_GATED` in `ir/opt_pipeline.c` vs `UT_COVERS` markers; wire into CI (soft-fail first)

## Phase F — Remaining registered passes — LARGELY LANDED (2026-06-26)
The previously-uncovered registered passes now have unit suites. **191 new tests** were written in parallel
(one agent per pass-group, assert-intended-behavior + independently-computed oracle values + compile-verify;
no production edits). After serial integration `make ut` reports **984 tests, 0 failed** (the binary also
contains the concurrent Phase BH metamorphic suites). One latent gap was surfaced and recorded — Finding #17.

### Multi-pass files: extended existing suites (no new registration needed)
| Suite (file) | Newly-covered passes | New tests | Status |
|---|---|---|---|
| opt_copyprop (`test_opt_copyprop.c`) | `cse_global_load`, `globalsym_cse`, `cse_param_add`, `local_load_cse`, `local_alu_cse`, `bool_cse` | 28 | **[x] integrated, green** |
| opt_constprop (`test_opt_constprop.c`) | `global_init_prop`, `symref_const_prop`, `complex_const_param_fold`, `value_tracking` | 34 | **[x] integrated, green** (latent gap → Finding #17) |
| opt_constfold (`test_opt_constfold.c`) | `const_string_calls`, `const_call_replace`, `switch_call_replace`, `param_addrof_const_fold`, `local_addrof_const_fold` | 24 | **[x] integrated, green** (reached via existing `utb_set_tok_str` frontend stubs) |

### Small passes: new suites (registered in Makefile `UT_MODULE_SRCS`/`UT_LOCAL_SRCS` + `test_main.c`)
| Suite | File | Pass(es) covered | Module srcs added | Tests | Status |
|---|---|---|---|---|---|
| opt_loop_dead | `test_opt_loop_dead.c` | `loop_dead_first_iter` | ir/opt_loop_dead.c, ir/opt_loop_utils.c | 15 | **[x] integrated** |
| opt_reroll | `test_opt_reroll.c` | `reroll` | ir/opt_reroll.c (+opt_loop_utils.c) | 14 | **[x] integrated** |
| opt_bitfield | `test_opt_bitfield.c` | `bitfield_insert_extract`, `bitfield_insert_to_bfi` | ir/opt_bitfield.c | 20 | **[x] integrated** |
| opt_const_aggregate | `test_opt_const_aggregate.c` | `const_aggregate_fold` | ir/opt_const_aggregate.c | 15 | **[x] integrated** |
| opt_dead_vla | `test_opt_dead_vla.c` | `dead_vla_struct_elim`, `alloca_load_fwd`, `dead_alloca_vreg_elim` | ir/opt_dead_vla.c | 20 | **[x] integrated** |
| opt_xform | `test_opt_xform.c` | `store_inplace_arith` | ir/opt_xform.c | 21 | **[x] integrated** |

### Still deferred (not a clean IR-only unit; see Deferred section)
- `opt_gens_*` / `opt_engine`: the generator/`IROptCtx` engine layer — driven via `tcc_ir_opt_run_gens(ctx,…)`,
  not the plain `tcc_ir_opt_<name>(TCCIRState*)` signature; needs an engine-context harness.
- `opt_hash`: pulled in by the concurrent Phase BH metamorphic work; left to that track to avoid collision.
- `opt_switch_data`: needs ELF/section + frontend state (already deferred).
- [ ] Equivalence harness (legacy path vs new SSA path on a `.c` corpus) — separate, pytest/QEMU-level effort.

## Phase G — Source-tree coverage ledger + generator — IN PROGRESS
Extends the tracker from optimizer passes/codegen levers to **every source TU** in tinycc, so new files cannot be added without an explicit coverage annotation.
- [x] `tests/unit/gen_source_coverage.py` — scan tinycc source tree, auto-map unit suites to their target TUs, read `source_coverage_map.json`, and regenerate `SOURCE_COVERAGE.md`.
- [x] `tests/unit/source_coverage_map.json` — editable ledger mapping each source file to its test layer (`unit`, `golden_ir`, `codegen_asm`, `ir_test`, `smoke`, `runtime_lib`, `tool`, `partial`, `none`).
- [x] Initial `tests/unit/SOURCE_COVERAGE.md` generated: **118 tracked files** — 42 unit, 61 QEMU `ir_tests` corpus, 16 runtime library. No `golden_ir`/`codegen_asm`/`smoke` entries yet because Phases C/D are not landed.
- [x] CI gate: `python3 tests/unit/gen_source_coverage.py --check` runs in `.github/workflows/ci.yml` and fails if a source file is missing from `source_coverage_map.json` or if `SOURCE_COVERAGE.md` is stale.

### Line-level coverage (gcov) — complements the file-level ledger
This tracker (and `SOURCE_COVERAGE.md`) record *which files have a suite*; `make ut-coverage`
records *which lines within them the suites exercise*. It does a clean `COVERAGE=1` instrumented
build (`--coverage`), runs the unit binary, and renders a `gcovr` report under
`arm/armv8m/build/coverage/` (terminal summary + `coverage.txt` + line-annotated `index.html`),
filtered to `ir/`/`arch/arm/`/`tccir_operand.c`. See README §"Code Coverage (gcov)".
- First-run per-pass snapshot (2026-06-26): isolated suites read high — `opt_neg_chain` 93%,
  `opt_setif_or_taut` 92%, `opt_cmp_fuse` 84%, `opt_jump_thread` 78%, `thop_*` encoders 75–100%.
  `opt_constfold` 7% / `opt_constprop` 23% / `opt_copyprop` 23% are low for the documented Phase F
  reason (name-gated / frontend-symbol passes the isolated harness can't yet reach), and `ir/core.c`
  reports ~1% because it is linked only for its `--gc-sections`-pruned `irop_config[]` table. Read
  per-file, not the aggregate.

---

## Findings

**Bug-fix status (2026-06-26):** Findings #3–#10 are resolved in production and their tests assert
correct behavior. #3, #4, #7, #9, #10 were correctness/robustness bugs; #5, #6, #8 were
missed-optimizations now implemented (folding `x OP x`; equal-immediate compares; write-after-write
dead-store elimination). Implementing #6 surfaced — and we then fixed — a **latent use-before-def
miscompile in `single_value_tmp`** (it NOPped the def of every constant temp after a RETURNVALUE fold,
even temps still used elsewhere; now relies on DCE). `make ut` stays green at **780 tests, 0 failed**;
the full IR suite (QEMU) is green on a `--disable-asan` build. Separately, a pre-existing
**SSA-construction phi leak** that made the ASAN IR suite all-red (~3873 failures) was fixed
(`ir/regalloc.c` `ra_resolve_phis` pre-RA nulling + `ir/opt/ssa_opt_dce.c` phase-3 phi removal, both
unlinked phi nodes without freeing). One pre-existing issue remains, tracked separately: a frontend
leak (`decl_initializer_alloc` on the `gen_late_reopt_functions` path, ~717 ASAN failures).
**New findings from Phases C/D (#11–#12) are intentionally not fixed** — they are infrastructure/missed-optimization observations recorded while landing the new harnesses.

**Phase BH bug-fix pass (2026-06-26, in progress):** Finding **#16** (`known_bits` ZEXT/SHR width bugs) is
**FIXED** in `ir/opt_knownbits.c:kb_const_compute`; its two characterization tests were flipped to `*_FIXED`
and assert the correct fold; `make ut` green. Finding **#15** (7 `-O1`/`-O2` random-C miscompiles) and **#14**
(4 compile-time leaks) are fixed. **#18** is a harness (oracle/reducer) bug, not a compiler bug.
**#17** is a latent, in-practice-unreachable gap.

1. ~~Harness limitation (not a pass bug): name-gated passes can't exercise their positive fold.~~
   **RESOLVED** by the §B harness extensions: `stubs.c` now provides `utb_set_tok_str(tok, name)` and
   `get_tok_str()` reads the test-populated table. Corner-case suites for `self_copy_elim` and
   `float_narrowing` can now assert the real positive fold by mapping the callee `Sym->v` token to
   `"memcpy"` / `"__aeabi_f2d"` etc. Same mechanism applies to the `*addrof_const_fold` /
   `*_string_calls` / `*_call_replace` constfold passes when they are reached (Phase F).

2. **No production bugs found** in the original Phase B suites. The Tier-1 historical bug-class guards
   (copyprop `is_lval`/DEREF preservation; cmp_fuse `is_lval` base guard; knownbits narrow-load
   mask/sign-extend; licm hoist-only-to-dominating-preheader; jump-thread backward-edge guard) still pass.

3. ~~**`neg_chain_cse` mixed-width fold bug.**~~ **FIXED** (`ir/opt_neg_chain.c`). A width-changing
   negation (e.g. `T1:I8 = -T0:I32`) truncates and is not value-preserving, so it must not join the
   wider base's canonical chain. The chaining on both the SUB-negation and the ASSIGN-copy paths is now
   gated on `dest_btype == src_btype`; a width-changing op anchors to itself, keeping
   `first_pos`/`first_neg` width-homogeneous per base. `test_neg_chain_mixed_width_int8_int32` now
   asserts `changes == 0` with both SUBs preserved.

4. ~~**`copy_prop` non-convergence / self-copy bug.**~~ **FIXED** (`ir/opt_copyprop.c`), two root
   causes:
   - *Self-copy*: an ASSIGN `T1 <- T1` was recorded as a copy and then "propagated" onto itself,
     reporting a spurious change every run. The recording guard now rejects `src1_vr == dest_vr`.
   - *Non-convergence*: after propagating `T2 <- T1` into `T2 <- V0`, the copy-recording step read the
     *stale* pre-propagation `src1` local and recorded the copy as having source `T1`, leaving a `T1`
     use that only collapsed on a second pass. The `src1`/`src2` locals are now refreshed to the
     propagated operand, so the `VAR→TMP→TMP` chain collapses to the original VAR in one pass.
   `test_copyprop_self_copy` and `test_copyprop_idempotent_after_chain` now assert convergence
   (`c2 == 0`).

5. ~~**`cmp_expr_fold` identical-vreg comparison gap.**~~ **IMPLEMENTED** (`ir/opt_constprop.c`). The
   both-vreg branch now folds `vr1 == vr2` for non-lval (register-value) operands of matching width and
   signedness: CMP is an integer compare so `evaluate_compare_condition(0,0,tok)` is determinate
   (`x==x`→true, `x>x`→false, `(uint32_t)x<x`→false). Guards: the lval form `*(V) OP *(V)` is left
   unfolded (a volatile load could differ), and a width/signedness mismatch (`CMP x:I8, x:I32`) is
   skipped since it compares a truncation against the full value. `test_cmpfold_expr_same_vreg_*` now
   assert the fold.

6. ~~**`cmp_expr_fold` immediate-immediate equality gap.**~~ **IMPLEMENTED + root-cause bug fixed**
   (`ir/opt_constprop.c`). The fold (two equal integer immediates → fold `CMP #7,#7`) is scoped to the
   both-nonvreg CMP-operand site (NOT the shared `ir_opt_nonvreg_expr_equal` helper — broadening that
   perturbs its ADD/SUB base-equality callers and miscompiles `nestfunc-2/3`). Floats excluded
   (NaN != NaN). **The fold initially exposed a pre-existing latent miscompile**: on
   `gcc.c-torture/execute/20031201-1` (bitfield STRICT_LOW_PART) the extra fold lets `cmp_expr_fold`'s
   trailing DCE collapse a ternary into `return <const-temp>`, which triggers `single_value_tmp`. That
   pass (`tcc_ir_opt_single_value_tmp`) had a **use-before-def bug**: it propagates a single-value
   constant temp ONLY into RETURNVALUE operands (Phase 2), but Phase 3 then NOPped the *definition* of
   EVERY `state==1` constant temp — including temps still used elsewhere (e.g. `OR T, #const` in a
   bitfield store), leaving a dangling use. Fixed by deleting Phase 3's manual NOP loop and letting DCE
   reclaim the dead defs (DCE only removes a def with no remaining uses). With that fix the fold is
   clean; `test_cmpfold_expr_imm_imm_equal` now asserts the fold.

7. ~~**`const_prop` `INT_MIN / -1` overflow fold bug.**~~ **FIXED** (`ir/opt_constprop.c`). The first
   constant-folding routine folded `DIV #INT_MIN, #-1` (and the analogous `IMOD`) despite the
   two's-complement overflow, while the *second* folding routine already bailed (line ~5646). The first
   routine now applies the same `v2 == -1 && v1 == {INT32,INT64}_MIN` bail, so the two paths agree and
   the UB case is left unfolded. `test_constprop_intmin_div_neg1_bugs` asserts `changes == 0`.

8. ~~**`dead_lea_store_elim` missing write-after-write kill.**~~ **IMPLEMENTED** (`ir/opt_dead_lea_store.c`).
   Pass 3 now scans forward within a *straight-line run* (break at any control-flow op or jump target):
   if a later store fully covers S1's byte range with no recorded read of those bytes in between, S1's
   value is provably never observed and is NOPped — even when the slot is read further on (that read
   sees the covering store's value). Soundness rests on the straight-line restriction (the covering
   store unconditionally executes before any branch) and on Pass 2 having bailed on all address
   escapes, so intervening stores never *read* S1's bytes. Partial overlaps and unresolvable coverage
   are left conservatively alive. `test_dls_multiple_stores_earlier_not_eliminated` now asserts the
   first store is eliminated.

9. ~~**`self_copy_elim` use-index bug.**~~ **FIXED** (`ir/opt_constfold.c` + `ir/opt_utils.c`). The pass
   called `ir_opt_pure_expr_equal(ir, p0, i, p1, i, 0)` with the call index `i` as the use-site for both
   params, so a TEMP redefined between `param0` and `param1` resolved to the same (last) definition and
   the self-copy fold fired incorrectly. A new helper `ir_opt_get_call_param_index()` returns each
   param's own FUNCPARAMVAL index, which is now used as the per-param use-site.
   `test_self_copy_elim_redefined_temp_suspected_bug` asserts `changes == 0`.

10. ~~**`float_narrowing` NULL-deref in `change_callee_sym`.**~~ **FIXED** (`ir/opt_utils.c`).
    `change_callee_sym()` now NULL-checks the `sym_push2()` result and returns 0 (no change) instead of
    dereferencing it. Defensive: in the full compiler `sym_push2` does not normally return NULL, but the
    guard removes the crash the unit-test stubs exposed and the latent production assumption.

11. ~~**SSA passes are not observable through `-dump-ir-passes=`.**~~ **RESOLVED.** The SSA optimizer
    runs inside `ir/regalloc.c:tcc_ir_ssa_regalloc` and previously only called `dbg_scan_imm_dest` after
    each pass (`ssa:branch`, `ssa:fold`, `ssa:sccp`, `ssa:cprop`, `ssa:gvn`, `ssa:load_cse`, `ssa:narrow`),
    which scans for an immediate-dest ASSIGN bug under `SCAN_IMM_DEST` but does not emit the `=== AFTER
    <pass> ===` blocks that `-dump-ir-passes=` extracts. The dump matcher + emitter were factored out of
    the static `tccgen.c` helpers into shared `tcc_ir_dump_passes_match()` / `tcc_ir_dump_after_pass()` in
    `ir/dump.c` (declared in `tccir.h`); `tccgen.c`'s `dump_ir_after_pass` now delegates to them. Both SSA
    pass sites call `tcc_ir_dump_after_pass(ir, "ssa:<name>")` after every pass: the iterative driver
    (`ir/opt/ssa_opt.c:tcc_ir_ssa_opt_run`, via a local `SSA_RUN` macro mirroring the legacy `RUN_PASS`)
    and the non-promotable fallback path in `tcc_ir_ssa_regalloc` (the all-address-taken case, which the
    `fold_add`/`branch_fold`/`narrow_add` cases hit). All seven SSA golden cases now assert real IR
    snapshots; Phase C is green at **8 passed**. (Surfaced separately while rebuilding the debug
    compiler: a pre-existing 8-byte leak in `ir/opt_memory.c:rse_build_def_map` — the const-memcpy pass
    rebuilds the def-map in a loop and the rebuild overwrote the prior allocation without freeing it;
    fixed by freeing at the top of `rse_build_def_map`.)

12. **Phase D codegen size levers are not yet implemented.** The five new disassembly characterization
    tests confirm the current (pre-optimization) state: R9 is saved/restored around every call when PIC is
    enabled (`str.w r9` / `ldr.w r9`); forward conditional branches remain `b<cc>.w` while backward ones are
    already narrowed; `cmp #0` + wide conditional branch is emitted instead of `cbz`/`cbnz`; duplicate
    wide-string literals (`L"..."`) are not merged in `.rodata`; the 9-byte packed-struct by-value path
    compiles correctly via `__aeabi_memmove`. The tests pass by asserting current behavior and should be
    flipped to assert the optimized behavior as `plan_binary_size_reduction.md` Phases 1–2 land.

13. **Phase BH xfail baseline.** ~~Planned-only.~~ **SUPERSEDED**: all four oracle harnesses are now
    implemented and validated (see the Phase BH table and Findings #14–#16). The baseline checkboxes have
    flipped to landed harnesses; the bugs they found are the new numbered entries below.

14. **Track 1 (ASAN/LeakSan sweep) — 4 compile-time memory leaks (FIXED, focused repros clean).** `armv8m-tcc`
    is ASAN-instrumented by default, so compiling the corpus with it makes tcc report leaks on its own heap.
    `scripts/asan_sweep.sh` dedups by the first meaningful backtrace frames (skipping allocator wrappers).
    Hits found in the validated slices, each with a minimal repro (all were allocation-lifetime bugs, not
    optimizer miscompiles). Fixed by releasing local `const_init_data` even when exported local symbols stay
    alive for IR references, preserving function-call scratch buffers for compile-error cleanup, and making
    in-flight codegen temporaries owned by `TCCIRState` during codegen so `tcc_ir_free()` can reclaim them on
    longjmp errors. Focused `-O0` repros below are now LeakSan-clean (ordinary unsupported-feature diagnostics
    remain where expected):
    - `decl_initializer_alloc <- decl <- block` — frontend init-allocator leak
      (`yasos-tcc-ir-suite-asan-leak-blocks-validation`). Repro: `gcc.c-torture/execute/pr58277-1.c -O0`.
    - `decl_initializer_alloc <- unary_paren <- unary` — compound-literal in expression
      context). Repro: `gcc.c-torture/execute/pr94524-1.c -O0`.
    - `tcc_ir_codegen_generate <- gen_function <- decl` — IR-codegen leak (132 B via `tcc_mallocz`),
      on `asm goto`. Repro: `gcc.c-torture/compile/asmgoto-4.c -O0`.
    - `unary_funcall <- unary <- expr_eq` — 608 B leak. Repro: `tests/tests2/106_versym.c -O0`.
    No use-after-free / heap-overflow / UBSan hits in the validated slices; a full `--corpus all` (+`--with-ubsan`)
    run should still be done before declaring the broader memory-safety class clear.

15. ~~**Tracks 2/3 (random-C differential) — 7 confirmed -O1/-O2 optimizer miscompiles.**~~ **FIXED.**
    `tests/fuzz/gen_c.py` emits UB-free random C (unsigned overflow-prone math, masked shifts/indices, guarded
    divisors, bounded loops) printing a checksum of computed values; `scripts/diff_olevels.py` runs it at
    `-O0/-O1/-O2` under live QEMU (mps2-an505). In every case below **tcc `-O0` == `arm-none-eabi-gcc -O2`
    (correct) and tcc `-O1`/`-O2` diverge**; each is clean under `gcc -Wall -Wextra` and host-gcc `-O0`/`-O2`
    agree, so they are genuine tcc bugs, not generator artifacts:

    | seed | correct (gcc / tcc-O0) | tcc-O1 | tcc-O2 | bad level |
    |---|---|---|---|---|
    | 0 | e0d26320 | e0d26320 | 278ee5ca | -O2 |
    | 10 | 252768b6 | 202a5b54 | e4a3c009 | -O1,-O2 |
    | 11 | 175e6cc6 | b3273411 | b3273411 | -O1,-O2 |
    | 18 | 340464e0 | 340464e0 | a3d7cec9 | -O2 |
    | 23 | 38792dc6 | 38792dc6 | c775b5cf | -O2 |
    | 31 | f64d161b | f64d161b | 54945bd1 | -O2 |
    | 37 | 00da84c9 | 00da84c9 | e8438bcc | -O2 |

    Repros in `tests/fuzz/results/findings/` (regen with `python tests/fuzz/gen_c.py --seed N`); seed 11 reduced
    87→59 lines. Fixed by preserving the ARM barrel-shift side-table semantics across SSA rewrites
    (`ssa_opt_replace_all_uses`, `ssa_opt_fold`, `ssa_opt_reassoc`), preventing barrel-shift fusion when the
    non-shift operand is an immediate, and disabling the still-unsound O2 loop rotation/unroll transforms pending
    targeted repairs. `KNOWN_DIVERGENCES` is now empty in both pytest wrappers.

16. ~~**Track 4 (IR metamorphic) — 2 `known_bits` constant-fold miscompiles.**~~ **FIXED** (`ir/opt_knownbits.c:kb_const_compute`).
    The host metamorphic fuzzer (`eval(f) == eval(P(f))` over a reference interpreter `ir_eval.h`) found both,
    each because the fold did not truncate the constant **source** to the op width:
    - **ZEXT to 64-bit dest of a negative-looking 32-bit constant.** `T:I64 = ZEXT(#-326:I32)` folded to
      `0xFFFFFFFFFFFFFEBA` (sign-extended) instead of zero-extended `0x00000000FFFFFEBA`. **Fix:** ZEXT is split
      out of the ASSIGN/LOAD case and now zero-extends from the **source btype** width (`kb_const_compute` takes a
      new `src1_btype` arg: `*out = a & src_mask`, src_mask per INT8/16/32/64). The single caller passes `s1_btype`.
    - **32-bit logical SHR of a constant with bit 31 set.** `T:I32 = SHR(#-1, #10)` folded to `#-1` instead of
      `0x003FFFFF`. **Fix:** `*out = (a & mask) >> b` (mask the source to the op width before the logical shift).
      SAR was already correct (it casts `(int32_t)(uint32_t)a`); ADD/SUB/AND/OR/XOR/SHL are width-safe after the
      trailing `*out &= mask`.
    Independently corroborated: the production `const_prop` pass folds the identical `SHR(#-1,#10)` to the
    **correct** `4194303`. The two characterization tests were renamed `*_FIXED` and now assert the correct fold;
    `make ut` green (the 76,800-check sweep stays at 0 mismatches; see #18 re: why the broad sweep still excludes
    SHR / INT64-dest ZEXT). A generator false positive (sub-word ZEXT to I8 — not a real IR shape) was caught and
    removed during the original bring-up, per the plan's false-positive discipline.

17. **`symref_const_prop` does not invalidate a tracked TMP redefined by a later ASSIGN (latent gap, UNFIXED).**
    Found while writing the Phase F constprop suite. In `ir/opt_constprop.c:tcc_ir_opt_symref_const_prop`
    (~lines 1096–1129), when an instruction is `ASSIGN Tn <- <src>`, control enters the
    `if (q->op == TCCIR_OP_ASSIGN && has_dest)` branch; if the source is **not** a non-lval symref the inner
    record does nothing, and the general dest-invalidation branch (`else if (has_dest) …`) is **not** reached
    because it is an `else if`. So a tracked symref for `Tn` survives a redefinition of `Tn` by ASSIGN, and a
    later use of `Tn` is rewritten to the stale symref. Empirically `ASSIGN T0<-&S; ASSIGN T0<-T9; ADD T1<-T0,#4`
    yields `changes==1` with the symref substituted into the ADD (correct: 0). **Not a live miscompile**: the
    pass's own header requires tmps be single-defined within a block (no later redef), which holds in canonical
    IR, so the buggy path is unreachable in practice. Because of that precondition the suite does **not** pin an
    assertion on this exact shape; `test_symrefconstprop_redef_invalidates` instead pins the genuinely-handled
    invalidation path (redef via a non-ASSIGN op, which *does* hit the invalidation branch). Recorded here for a
    future hardening pass (make the ASSIGN branch fall through to invalidation when it doesn't record a copy).

18. **Metamorphic oracle/reducer is RNG-fragile — produces an arithmetically-impossible value (harness bug, UNFIXED).**
    Surfaced while landing the #16 fix: re-enabling SHR / INT64-dest ZEXT in `ir_gen.h` (to give the now-fixed folds
    live sweep coverage) shifts the generator RNG stream and makes seed 214 trip a `const_prop` "mismatch":
    `temp[1] base=-255578619 got=0`, delta-reduced to `T1 = -2 & V; T9 = -2 & T1`. But `-2 & x` is **always even**,
    so the oracle's base value (-255578619, **odd**) is arithmetically impossible — i.e. the *interpreter/reducer*,
    not `const_prop`, is wrong for this shape (likely the delta-reducer over-reduced into a read-before-def, or the
    interpreter mis-models the reduced operand). It is **not** a real compiler bug. Because the sweep's
    "0 mismatches over 76,800 checks" green is therefore partly RNG-luck, SHR and INT64-dest ZEXT are kept OUT of the
    broad sweep for now (the #16 fixes are covered by the deterministic `test_{zext64,shr}_neg_const_known_bits_FIXED`
    cases instead). Re-enabling them requires first hardening the oracle: make the delta-reducer reject reductions
    that introduce read-before-def, and have `ire_eval` return `IRE_UNSUPPORTED`/skip on any operand it cannot model
    rather than computing a bogus value.

## Deferred
- **opt_switch_data** (`switch_to_data`, `switch_collapse`): needs ELF/section + frontend state
  (`get_sym_ref`, `greloc`, `int_type`, `section_add`) — not a clean IR-only unit test. Revisit with a
  section/codegen stub layer or move to an integration-level test.
- **opt_gens_\* / opt_engine** (`tcc_ir_opt_run_gens`, `IROptCtx`-driven generator passes): not the plain
  `tcc_ir_opt_<name>(TCCIRState*)` shape the `ir_build.h` harness drives. Needs an engine-context harness
  (`tcc_ir_opt_ctx_init`/`_free` + a way to invoke a single `IROptGen` over hand-built IR). Phase F follow-up.
- **Legacy↔SSA equivalence harness**: run the old legacy optimizer path and the new SSA path over a `.c` corpus
  and diff results — a pytest/QEMU integration-level effort, not a host IR-builder unit test.

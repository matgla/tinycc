# Plan: Unit-Test Coverage for `source/backend` — Task Cards for Small-Model Execution

**Status:** proposed · **Owner:** unit-test track · **Created:** 2026-07-19

This plan brings `source/backend/` from **46.4% to ~70% line coverage** with
host-native unit tests, structured as **self-contained task cards** that a
small/cheap model can execute one at a time without global codebase context.

Read [`docs/writing_unit_tests.md](../writing_unit_tests.md) first. Its rules
are load-bearing and apply to every card below:

- **Rule 1:** never edit production source — only files under `tests/unit/`
  (test files, stubs, the Makefile). Characterize code as it is.
- **Rule 2:** every suspected bug → pin the current behavior in the test +
  report in `docs/bugs.md`. Never fix product code to make a test pass.
- **Rule 3:** run unit tests only, always under `timeout 15`. Never
  `make cross` / `make test` while writing tests.

---

## 1. Current state (ground truth)

Regenerated 2026-07-19 via `make -C tests/unit/arm/armv8m coverage`
(merged report of all 12 unit binaries). To regenerate after changes:

```bash
make -C tests/unit/arm/armv8m coverage          # full merged report (slow)
make -C tests/unit/arm/armv8m coverage-backend  # backend binary only (fast)
# text:   tests/unit/arm/armv8m/build/coverage/coverage.txt        (merged)
#         tests/unit/arm/armv8m/build_backend/coverage/coverage.txt (backend-only)
```

Per-file numbers, sorted worst-first (only files <100% shown; every `thop_*`
encoder except `thop_mov` is already at 92–100%):

| File | Lines | Cov | Existing suite(s) |
|------|------:|----:|-------------------|
| `generators/function.c` | 378 | **0%** | none — coverage-only, never linked |
| `generators/regalloc.c` | 511 | **0%** | none — coverage-only, never linked |
| `arch/arm/thumb/arm-thumb-asm.c` | 1739 | **34%** | `test_arm_thumb_asm.c` (188 tests) |
| `arch/arm/thumb/arm-thumb-gen.c` | 6509 | **40%** | `test_gen_*.c` ×13 + `test_codegen_*.c` ×8 + `test_thumb_core.c` (~470 tests) |
| `arch/arm/arm-link.c` | 294 | 75% | `test_arm_link.c` |
| `arch/arm/thumb/thop_mov.c` | 29 | 79% | `test_thop_mov.c` |
| `arch/arm/ssa_opt_arm.c` | 530 | 84% | `test_ssa_opt_arm.c` |
| `arch/arm/thumb/arm-thumb-callsite.c` | 139 | 85% | `test_gen_callsite.c` |
| **aggregate `source/backend/`** | **11413** | **46.4%** | |

### Why the gaps exist (mechanics)

1. **`generators/*.c` at 0%** — both files sit in `UT_COVERAGE_ONLY_SRCS`
   (`tests/unit/arm/armv8m/Makefile:352-353`): compiled for bookkeeping, never
   linked, symbols faked by stubs. They are the recent split of
   `function_pipeline.c` into opt part (`source/opt/`) vs backend part
   (`source/backend/generators/`). No binary links them → no test can reach
   them. Needs a **new isolated binary (UT13)**, same pattern as `build_ssaopt`.
2. **`arm-thumb-gen.c` at 40%** — every `_mop` handler except four
   (`predicated_alu`, `sbfx`, `complex_op`, `complex_div`) is already *named* in
   a suite, so the gap is **untested paths inside handlers** (spill/reload,
   64-bit splits, stack args, literal pool, hard-vs-soft float), not missing
   dispatch coverage. Per-function gap map in §5.
3. **`arm-thumb-asm.c` at 34%** — 188 tests exist but cluster on common forms;
   the big holes are whole opcode-family parsers, above all
   `thumb_single_memory_transfer_opcode` (229 missing lines). Map in §6.
4. The 75–85% files each have **one focused hole** (relocation types, PLT,
   call-layout-from-IR, the `ssa_gen_arm_fuse_*` family). Map in §4.

---

## 2. Execution protocol (how a small model works a card)

Each card below is **one independent, mergeable unit of work**. Cards have no
inter-dependencies except where stated ("requires B0"). Execute one card per
session; do not batch.

### Task-card loop

1. **Read** `docs/writing_unit_tests.md` §0–§8, then the fixture file named in
   the card (copy its setup/helpers verbatim).
2. **Write** the suite `tests/unit/arm/armv8m/test_<name>.c` (or extend the
   existing suite the card names).
3. **Wire** it into the listed binary's `UTn_LOCAL_SRCS` in
   `tests/unit/arm/armv8m/Makefile`.
4. **Build & run** (never without timeout):
   ```bash
   timeout 60 make -C tests/unit/arm/armv8m run-backend   # or: run
   ./build_backend/run_unit_tests_backend <suite_substring>   # iterate on one suite
   timeout 15 ./build_backend/run_unit_tests_backend
   ```
5. **Measure the delta** and check it against the card's exit criterion:
   ```bash
   make -C tests/unit/arm/armv8m coverage-backend   # backend binary
   make -C tests/unit/arm/armv8m coverage           # merged (main-binary suites)
   grep -E "arm-thumb-gen.c|arm-thumb-asm.c" \
     tests/unit/arm/armv8m/build*/coverage/coverage.txt
   ```
6. **Assertions on emitted bytes:** capture expected bytes by *running* the
   call, then cross-check each sequence against the matching `thop_*` encoder
   oracle suite (the discipline `test_gen_arith.c`'s header documents). Never
   hand-invent encodings.
7. **Bug found?** Pin current behavior + `docs/bugs.md` entry (Rule 2). Move on.
8. **Done =** suite green from a clean rebuild (`make -C tests/unit/arm/armv8m
   clean && timeout 120 make -C tests/unit/arm/armv8m run-backend`), no leaks,
   coverage delta ≥ card target.

### Dispatch prompt template (paste to a small model)

> Work the task card **\<ID\>** in `docs/plans/backend_unit_test_coverage.md`.
> Follow `docs/writing_unit_tests.md` rules 1–3 strictly: harness files only,
> pin-don't-fix bugs, `timeout 15` on test binaries. Stop when the card's
> Acceptance line passes; report the coverage delta you measured.

---

## 3. Phase 0 — baseline (done, keep current)

- [x] Merged coverage report exists; numbers copied into §1.
- **B0 (hygiene, any time):** `docs/writing_unit_tests.md` §3 table still says
  "ten binaries" — the tree has 12 (`build_ssaopt`, `build_unique_ptr` landed).
  When UT13 lands (Phase 4), update that table and `tests/unit/README.md`'s
  binary list in the same commit. Also register the new binary in the
  aggregate `all`/`run`/`coverage` targets and top-level `make ut`.

---

## 4. Phase 1 — mop-up cards (small, high certainty, existing suites)

Each extends an existing suite in the named binary. No new fixtures needed.

### B1 — `thop_mov.c` 79% → 100%  (6 missing lines)
- **Targets:** `mov_reg_shift_t1_emit` (5 lines — the shift-form emit path),
  `th_mov_reg` (1).
- **Suite:** extend `test_thop_mov.c` (main binary, `run`).
- **Fixture:** sibling `test_thop_shift_reg.c` — same builder/assert pattern.
- **Acceptance:** `thop_mov.c` at 100% in merged report; suite green.

### B2 — `arm-thumb-callsite.c` 85% → 100%  (20 missing lines)
- **Targets:** `thumb_build_call_layout_from_ir` (17 — an IR-shape branch not
  exercised), `thumb_ensure_call_site_capacity` (2),
  `thumb_get_or_create_call_site` (1).
- **Suite:** extend `test_gen_callsite.c` (backend binary).
- **Acceptance:** 100% in `coverage-backend` report.

### B3 — `arm-link.c` 75% → ≥95%  (71 missing lines)
- **Targets:** `relocate` (34 — uncovered relocation *types*: R_ARM_THM_*
  variants not yet tabled), `relocate_plt` (29 — the PLT path),
  `create_plt_entry` (8).
- **Suite:** extend `test_arm_link.c` (main binary) — it is the guide's worked
  example for table-driven relocation tests; copy its per-test
  `TCCState`/`Section` fixture and error-recording stub.
- **Note:** PLT paths may need stub `tccelf` section helpers; `elfsec_stubs.*`
  already exists — extend it rather than inventing new stubs.
- **Acceptance:** ≥95% in merged report.

### B4 — `ssa_opt_arm.c` 84% → ≥97%  (84 missing lines)
- **Targets:** the `ssa_gen_arm_fuse_*` fusion family —
  `fuse_store_src_through_add_imm` (15), `fuse_shl_add_to_load_indexed` (13),
  `fuse_shl_add_to_store_indexed` (13), `fuse_mla_accum_through_add_imm` (11),
  `fuse_mul_add_to_mla` (8), `arm_extract_add_imm_base` (6),
  `fuse_load_through_add_imm` (5), `fuse_shl_indexed` (5),
  `fuse_store_add_imm_combined` (5), `fuse_store_through_add_imm` (3).
- **Suite:** extend `test_ssa_opt_arm.c` (main binary) — hand-built
  `vinfo` + direct `ssa_gen_*` calls, its established pattern.
- **Acceptance:** ≥97% in merged report.

---

## 5. Phase 2 — `arm-thumb-gen.c` 40% → ~65% (3,870 missing lines, 189 funcs)

All cards go in the **backend binary** (`run-backend` / `build_backend`),
linking the real `arm-thumb-gen.c`. **Master fixture:** `test_gen_arith.c` —
`setup_gen()` (`elfsec_reset` + `cgb_reset` + `arm_target_init(...cortex-m33)`
+ fresh `.text` section), `mop_reg/mop_reg64/mop_imm` builders, direct
`tcc_gen_machine_*_mop` calls, byte assertions via `elfsec_stubs`. Cards B5–B14
are independent; work them in any order. Top gap clusters:

| Cluster | Missing lines | Key functions (missing lines each) |
|---------|--------------:|-------------------------------------|
| Store/load core | ~600 | `store_mop` 216, `load_indexed_mop` 86, `store_indexed_mop` 75, `load_mop` 69, `load_from_base` 52, `decode_str_ldr_imm` 37, `tcc_gen_mach_load_to_reg` 32, postinc 19+17 |
| Arg moves & calls | ~480 | `thumb_emit_arg_move` 157, `place_stack_arguments` 76, `func_call_mop` 59, `place_stack_arg_64bit` 46, `parallel_arg_moves` 46, `fp_mop_load_arg` 45, `place_one_stack_arg` 35, `place_stack_arg_32bit` 34, `build_register_arg_moves` 30, `place_stack_arg_struct` 29 |
| Prolog/epilog/nested | ~250 | `prolog` 156, `gen_nested_func_trampoline` 66, `epilog` 27, `gfunc_sret` 15, `resolve_chain_base` 13 |
| Complex arithmetic | ~265 | `complex_mul_double` 81, `complex_mul` 51, `softcall_name` 50, `complex_div` 37, `complex_op_double` 32, `complex_div_double` 32, `complex_op` 29, `complex_pair_writeback` 20 |
| Constants/literal pool | ~190 | `load_full_const` 89, `th_literal_pool_generate` 84, `th_literal_pool_allocate` 15 |
| Multiply | ~175 | `try_mul_by_const_mop` 109, `emit_mul64_mop` 51, `emit_op_imm_fallback` 14 |
| Data processing/shift64 | ~170 | `emit_shift64_mop` 67, `emit_data_processing_mop32` 65, `mop64` 38, `mach_mod_mop` 17 |
| `mach_*` machinery | ~150 | `mach_ensure_in_reg` 53, `mach_writeback_dest` 37, `get_scratch_reg_with_save` 37, `mach_make_complex_imag` 27, `thumb_decode_dest_reg` 26, `mach_resolve_deref_64` 25, `mach_make_hi_half` 22, `restore_scratch_reg` 20 |
| Branch/opt emission | ~140 | `ot` 53, `branch_opt_analyze` 36, `decbranch` 35, `th_patch_call` 33 |
| Bool/select/setif/lea | ~130 | `bool_mop` 43, `assign_mop` 36, `lea_mop` 35, `select_mop`+`select_emit_inline` 36, `setif_mop` 14, `predicated_alu_mop` 12 |
| Spill/cache | ~70 | `store_spill_slot` 14, `load_spill_slot` 13, `spill_cache_record` 12, `try_strd_imm_spill` 12, `scratch_pushed_dead_reg` 12 |
| Long tail | ~200 | `block_copy_mop` 30, `prefetch_mop` 30, `get_struct_base_addr_mop` 26, `nl_longjmp` 23, `mapcc` 22, `gen_vla_alloc` 19, `addr_of_stack_slot` 19, `sbfx_mop` 19, `pack64` 13, `vla_mop` 14, `arm_deinit` 16, … |

### B5 — `test_gen_mem2.c`: store/load paths deep-dive
- **Targets:** `tcc_gen_machine_store_mop` (the single biggest gap: spill-slot
  stores, 64-bit stores, immediate-offset overflow → scratch-register fallback,
  strd), `load/store_indexed_mop`, `load_mop`, post-inc forms,
  `decode_str_ldr_imm`, `load_from_base`, `tcc_gen_mach_load_to_reg`.
- **Fixture:** `test_gen_mem.c` already covers the shallow paths — extend with
  operand shapes it lacks: unaligned/oversized immediates, r1-pair 64-bit ops,
  stack-slot (`MACH_OP_STACK`?) operands forcing spill code.
- **Acceptance:** these functions ≤30 missing lines combined.

### B6 — `test_gen_call2.c`: stack arguments & 64-bit/struct/FP args
- **Targets:** `thumb_emit_arg_move` + the whole `place_stack_arg*` cluster,
  `thumb_emit_parallel_arg_moves` (cycle-breaking temp moves),
  `fp_mop_load_arg`/`fp_mop_load_double_arg`, `func_call_mop` deep paths,
  `presave_stack_args_from_arg_regs`, `thumb_call_imm_handler`.
- **Fixture:** `test_gen_call.c` + `test_gen_callsite.c`; drive
  `tcc_gen_machine_func_parameter_mop`/`func_call_mop` with >4 args, 64-bit
  args, struct-by-val args, float args under both `ARM_HARD_FLOAT` and soft
  float (`tcc_state->float_abi` is set in setup).
- **Acceptance:** cluster ≤120 missing lines combined.

### B7 — `test_gen_prolog2.c`: prolog/epilog/nested
- **Targets:** `tcc_gen_machine_prolog` (callee-save selection, frame size,
  LR/FP variants), `tcc_gen_machine_epilog`, `gen_nested_func_trampoline`,
  `gfunc_sret`, `resolve_chain_base`.
- **Fixture:** `test_gen_prolog.c`; vary `tcc_state->need_frame_pointer`,
  `func_dynamic_sp`, `registers_for_allocator`, naked/static-chain flags on
  the IR state.
- **Acceptance:** `prolog` ≤40 missing lines; trampoline covered ≥60%.

### B8 — `test_gen_const.c`: constant materialization & literal pool
- **Targets:** `load_full_const` (movw/movt vs literal-pool selection,
  negative values, 16-bit-encodable), `th_literal_pool_generate` (flush at
  pool boundary, alignment, branch-around emission), `th_literal_pool_allocate`.
- **Fixture:** `test_gen_arith.c` + byte-oracle cross-check against
  `test_thop_mov.c`/`test_thop_ldr_literal.c`.
- **Acceptance:** all three ≥85%.

### B9 — `test_gen_mul.c`: multiply synthesis
- **Targets:** `thumb_try_mul_by_const_mop` (shift/add/sub/rsb chains, LSL
  fallback, negative constants, non-synthesizable → `mul`), `emit_mul64_mop`,
  `thumb_emit_op_imm_fallback`.
- **Fixture:** `test_gen_arith.c`; table of constants: powers of 2, 2^n±1,
  ±small composites, 0/1/-1, and a non-decomposable prime; cross-check bytes
  against `test_thop_mul.c`/`test_thop_shift_imm.c`.
- **Acceptance:** `try_mul_by_const_mop` ≥85%, `emit_mul64_mop` ≥70%.

### B10 — `test_gen_dataproc2.c`: shift64 & 32/64-bit data processing
- **Targets:** `thumb_emit_shift64_mop` (variable & constant shifts, both
  directions, edge counts 0/32/≥32), `thumb_emit_data_processing_mop32/64`
  (immediate vs register, wide/narrow selection), `mach_mod_mop`.
- **Acceptance:** cluster ≥80%.

### B11 — `test_gen_mach2.c`: mach_* operand machinery
- **Targets:** `mach_ensure_in_reg` (already-in-reg, imm→mov, spill-slot→load),
  `mach_writeback_dest`, `get_scratch_reg_with_save`/`restore_scratch_reg`
  (push/pop pairing), `thumb_decode_dest_reg`, `mach_resolve_deref_64`,
  `mach_make_hi_half`, `mach_make_complex_imag`.
- **Fixture:** `test_gen_mach_operand.c` (extend — same subject family).
- **Acceptance:** cluster ≥85%.

### B12 — `test_gen_branch_emit.c`: branch emission & `ot()`
- **Targets:** `ot` (wide/narrow selection, pool-flush interaction),
  `branch_opt_analyze`, `decbranch` (cbz/cbnz vs cmp+bne), `th_patch_call`,
  `mapcc` (condition-code mapping table — all 16 entries).
- **Fixture:** `test_gen_branch.c`.
- **Acceptance:** cluster ≥75%.

### B13 — `test_gen_select.c`: bool/select/setif/assign/lea + two orphan handlers
- **Targets:** `bool_mop`, `assign_mop`, `lea_mop`, `select_mop` +
  `select_emit_inline`, `setif_mop`, and the **two never-named handlers**
  `tcc_gen_machine_predicated_alu_mop`, `tcc_gen_machine_sbfx_mop`
  (cross-check bytes vs `test_thop_bitfield.c`).
- **Acceptance:** cluster ≥80%; both orphan handlers ≥70%.

### B14 — `test_gen_complex.c`: complex arithmetic
- **Targets:** all six `thumb_process_complex_*_mop` (fadd/fsub/fmul/fdiv,
  float & double), `complex_pair_writeback`, `tcc_get_abi_softcall_name`
  (table of soft-call names — pure mapping, easiest lines in the file).
- **Note:** `_Complex` torture tests are auto-skipped on this target, but the
  handlers are real reachable code — drive them directly with complex-typed
  `MachineOperand`s; softfloat oracle: `test_gen_softfloat.c`.
- **Acceptance:** family ≥70%; `tcc_get_abi_softcall_name` 100%.

### B15 — `test_gen_spill.c` + long tail (optional, time-boxed)
- **Targets:** spill-slot store/load + spill cache, `block_copy_mop`,
  `prefetch_mop`, `nl_longjmp_mop`, `vla_mop`/`gen_vla_alloc`,
  `get_struct_base_addr_mop`, `pack64_mop`, `tcc_machine_addr_of_stack_slot`,
  `arm_deinit`.
- **Acceptance:** each touched function ≥60%; skip whatever needs full RA
  state — note it for Phase 4 instead.

---

## 6. Phase 3 — `arm-thumb-asm.c` 34% → ~80% (1,145 missing lines, 49 funcs)

Suites live in the **main binary** (`run`). Fixture: the existing
`test_arm_thumb_asm.c` (188 tests) — reuse its assemble-one-line +
bytes/section assertion helpers. Thumb-2 encoding oracles: the `test_thop_*`
suites. Cards independent.

| Cluster | Missing lines | Key functions |
|---------|--------------:|---------------|
| Single memory transfer | 229+37 | `thumb_single_memory_transfer_opcode` 229, `..._literal_opcode` 37 |
| Dispatch/parse layer | ~250 | `asm_opcode` 99, `parse_operand` 70, `asm_gen_code` 42, `asm_parse_optional_shift` 29, `asm_compute_constraints` 20, `process_operands` 12 |
| Branch/shift/block | ~180 | `thumb_branch` 67, `thumb_data_shift_opcode` 57, `thumb_block_memory_transfer_opcode` 29, `thumb_cache_preload_opcode` 46 |
| VFP asm | ~115 | `vfp_arith` 26, `vcvt` 26, `vmov` 23, `vmrs` 17, `vcmp` 12, `vpushvpop` 11 |
| Mid-table parsers | ~190 | `pkhbt` 22, `data_processing` 22, `process_control` 20, `adr` 19, `thumb_tt` 18, `math` 18, `control` 16, `conditional` 15, `barrier` 14, `bitmanipulation` 14, `ssat` 13, `cps` 12 |

### B16 — `test_asm_mem.c`: single memory transfer family
- All `ldr/str{,b,h,sb,sh,d}` × immediate/T2-soff/register/shift forms,
  pre/post-index, writeback, pc-relative literal loads, plus error paths
  (out-of-range offset, bad register lists). **This one card is ~20% of the
  file's gap.**
- **Acceptance:** `thumb_single_memory_transfer_opcode` + literal variant
  ≥85%.

### B17 — `test_asm_parse.c`: operand parser & dispatch
- `parse_operand` shapes (register, immediate, shifted register, register
  list, memory bracket forms, bad-input diagnostics), `asm_parse_optional_shift`,
  `asm_opcode` table routing, `process_operands`, `asm_gen_code` relocation
  emission, `asm_compute_constraints` for inline-asm constraint strings.
- **Acceptance:** cluster ≥80%.

### B18 — `test_asm_branch_block.c`: branch, shift, block transfer, preload
- `thumb_branch` (b/bl/bx/blx, cond codes, wide ranges), `thumb_data_shift_opcode`,
  `thumb_block_memory_transfer_opcode` (ldm/stm variants, `^` and `!`),
  `thumb_cache_preload_opcode` (pld/pldw/pli).
- **Acceptance:** cluster ≥85%.

### B19 — `test_asm_vfp.c`: VFP instruction parsing
- `vfp_arith`/`vcvt`/`vmov`/`vmrs`/`vcmp`/`vpushvpop` across s/d register
  banks and immediate forms.
- **Acceptance:** cluster ≥85%.

### B20 — `test_asm_misc.c`: remaining parser families
- `pkhbt`, `data_processing`, `process_control` (mrs/msr special regs), `adr`,
  `thumb_tt` (TT/TTA instructions), `math`, `control`, `conditional` (it-blocks),
  `synchronization_barrier` (dmb/dsb/isb), `bitmanipulation`, `ssat`, `cps`.
- **Acceptance:** each function ≥80%.

---

## 7. Phase 4 — `generators/` 0% → ~55% (new binary UT13 `build_genfunc`)

The two files export exactly three symbols: `gen_function(Sym*)` (the codegen
driver), `tcc_ir_backend_analyze_leaf_and_tail_calls(TCCIRState*, int)`, and
`tcc_ir_backend_regalloc_pipeline(...)`. Statics need the include-the-`.c`
trick (`writing_unit_tests.md` §7).

### B21 — UT13 bring-up (the only hard card; do it first, alone)
- **Add** `BUILD_DIR13 := build_genfunc` + `UT13_*` block to
  `tests/unit/arm/armv8m/Makefile`, modeled verbatim on the UT11
  (`build_ssaopt`, Makefile:1309+) block: `UT13_MODULE_SRCS := $(UT_MODULE_SRCS)
  + generators/function.c + generators/regalloc.c` — **no**, see below.
- **Include-the-`.c` decision:** to reach statics, the suite
  `#include`s `generators/function.c`/`regalloc.c` directly (like
  `test_tccgen.c`), so `UT13_MODULE_SRCS` must **not** compile them separately.
  `UT13_MODULE_SRCS := $(UT_MODULE_SRCS)` + whatever the trial link demands
  (`ir/codegen.c`? `tccls.c` is already in the main set — check).
- **Stub surface** (new file `genfunc_stubs.c`, UT13-local): `block`,
  `check_vstack`, `compile_nested_functions`, `pop_local_syms`, `gfunc_epilog`,
  `tcc_debug_funcstart`, `put_extern_sym`, `add_array`, `get_tok_str`,
  `sym_push2`, `sym_pop`, `mk_pointer`, `elfsym`, `tcc_bench_log_phase`,
  `dbg_scan_imm_dest`, `dbg_scan_overlap` — trial-link and add exactly what
  `ld` reports; keep each stub minimal (record-and-return).
- **Collision watch:** `ra_link_stubs.c` fakes the `ssa_opt_*` family; the
  generators call the opt pipeline via `tcc_ir_opt_run_function_pipeline` —
  prefer the stub unless a card needs the real pass. Same guard-macro pattern
  as `UT_SSA_OPT_REAL` if a later card needs the real thing.
- **Acceptance:** `timeout 15 make -C tests/unit/arm/armv8m run-genfunc` links
  and runs an empty suite; B0 doc/table updates ride along.
- **Small-model note:** this card is iterative trial-link work — give it to a
  stronger model if available, or expect several build-fix rounds.

### B22 — `test_genfunc_leaf_tail.c`: the pure-analysis win
- **Targets:** `tcc_ir_backend_analyze_leaf_and_tail_calls` +
  (via include) `ir_call_is_tail_positioned`: leaf vs call-containing,
  tail-positioned call (next non-NOP is RETURNVOID / RETURNVALUE of dest),
  non-tail cases, `compute_min_stack_ref`.
- **Fixture:** hand-built `TCCIRState` via `ir_build.h` (the instruction-list
  builder the `test_gen_*` suites use) — flat instruction lists, no CFG needed.
- **Acceptance:** both functions ≥90%. **Highest value-per-line in Phase 4 —
  do this card second.**

### B23 — `test_genfunc_pipeline.c`: regalloc pipeline stages
- **Targets (statics via include):** `setup_register_allocation`,
  `compute_stack_layout`, `run_register_coalescing`, `run_jump_threading_loop`,
  `run_ssa_and_post_ra_passes`, `run_post_alloc_passes`,
  `finalize_nested_functions`, then `tcc_ir_backend_regalloc_pipeline`
  end-to-end on a minimal function IR.
- **Fixture:** `ir_build.h` + `ssa_build.h`; `elfsec_stubs` for sections.
  Assert on IR/stack-layout state after each stage, not on emitted bytes.
- **Acceptance:** `regalloc.c` ≥55%.

### B24 — `test_genfunc_driver.c`: `gen_function` smoke
- **Targets:** `gen_function` on a hand-built `Sym` for a trivial
  `int f(void){return 0;}`-shaped function: prolog+epilog emitted, symbol size
  patched, `tcc_ir_free` path reached. One smoke test, not exhaustive — the
  deep paths belong to B5–B15.
- **Acceptance:** `function.c` ≥40%; whole Phase 4 lands `generators/` at
  ~55% combined.

---

## 8. Success metrics

- **Aggregate `source/backend/`:** 46.4% → **≥70%** line coverage in
  `make -C tests/unit/arm/armv8m coverage`.
- **Per-file:** `arm-thumb-gen.c` ≥65%, `arm-thumb-asm.c` ≥80%,
  `arm-link.c` ≥95%, `thop_mov`/`arm-thumb-callsite`/`ssa_opt_arm` ~100%,
  `generators/*` ≥55%.
- **Wiring:** every new suite in `all`/`run`; `make ut` green from clean;
  `check_coverage_files.py` keeps passing (generators files may stay in
  `UT_COVERAGE_ONLY_SRCS` for the main binary while real coverage flows from
  UT13 — same pattern as `ssa_opt*` in UT11).
- **Order-of-work suggestion:** B1–B4 (1 day-ish) → B16 (biggest single card)
  → B5, B6, B8, B9 → B17–B20 → B7, B10–B14 → B21–B24 → B15.

---

## 9. Risks & mitigations

| Risk | Mitigation |
|------|-----------|
| `build*/` dirs are shared with parallel agents/tasks — a concurrent `make clean` wipes coverage mid-run | Never run coverage concurrently with other work; coverage artifacts are regenerable (`coverage-backend` ≈ 1 min). Cards only *commit* test sources, never build artifacts |
| Byte-level assertions fossilize wrong behavior | Rule 2: cross-check against `thop_*` encoder oracles; pin-and-report in `docs/bugs.md` |
| UT13 trial-link spirals (frontend symbol surface) | Time-box B21; the stub list above is pre-enumerated from the actual `extern` declarations; `genfunc_stubs.c` is UT13-local so it can't collide with other binaries |
| `arm-thumb-gen.c` paths that need full RA/spill state are unreachable from mop-level fixtures | Accept ≤65% file target; push residue to B24's driver-level test or explicitly list as out-of-scope |
| Small model drifts into editing product source | The dispatch template states Rule 1; reviewers check the diff touches only `tests/unit/` (+ docs in B0) |
| Coverage targets gamed by execution without assertions | Every card's Acceptance requires green suite **and** line delta; suites must assert on bytes/state, not just call |

---

## Appendix A — regenerating the gap maps

```bash
make -C tests/unit/arm/armv8m coverage          # merged (main-binary files)
make -C tests/unit/arm/armv8m coverage-backend  # arm-thumb-gen.c lives only here

# missing-lines-per-function for a file (needs ctags):
python3 - <<'EOF'
import re, subprocess
lines = open("tests/unit/arm/armv8m/build/coverage/coverage.txt").read().splitlines()
merged, i = [], 0
while i < len(lines):
    if re.match(r'^\S+\.(c|h)\s*$', lines[i]) and i+1 < len(lines):
        merged.append(lines[i].strip() + " " + lines[i+1].strip()); i += 2
    else:
        merged.append(lines[i]); i += 1
t = "source/backend/arch/arm/thumb/arm-thumb-gen.c"   # <-- change me
miss = next(re.search(r'(\d+)%\s+(.*)$', ln).group(2)
            for ln in merged if ln.startswith(t))
ranges = []
for part in miss.split(","):
    part = part.strip()
    if "-" in part:
        a, b = part.split("-"); ranges.append((int(a), int(b)))
    elif part.isdigit():
        ranges.append((int(part), int(part)))
fm = [(int(c.split()[2]), c.split()[0]) for c in
      subprocess.run(["ctags","-x","--c-kinds=f","--sort=no",t],
                     capture_output=True, text=True).stdout.splitlines()]
from collections import Counter
cnt = Counter()
for a, b in ranges:
    for l in range(a, b+1):
        name = "<file-scope>"
        for fl, fn in fm:
            if fl <= l: name = fn
            else: break
        cnt[name] += 1
for fn, c in cnt.most_common(40): print(f"{c:5d}  {fn}")
EOF
```

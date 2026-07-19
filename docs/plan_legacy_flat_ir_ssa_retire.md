# Plan: Retire flat-IR scalar passes in favor of their SSA analogs

**Status:** in progress · **Created:** 2026-07-07 · **Updated:** 2026-07-19 · **Branch:** `legacyOptRemoval`

Scope: pre-RA flat-IR **scalar** passes that duplicate an SSA pass already running at
`-O1+`. For each pass, reach a terminal state — **deleted** (SSA subsumes) or **kept
flat** (relocated, sharing one core with its SSA analog). **Reference oracle: `-O2` only.**

Out of scope: post-RA passes, ARM machine fusions, frontend memory-init lowering,
VLA/alloca lowering. See § Out of scope.

**Relocation track: COMPLETE** (2026-07-19). `ir/` holds no optimizer code; every flat
pass lives under `source/opt/flat/`. Per-batch history is in git + the memory files.

## Per-pass state machine

1. **Verify live.** Grep call sites. No live call site → delete, stop.
2. **Measure the SSA gap** (§ below). Delta 0 → **Branch B** (delete). Delta >0, residual
   SSA-expressible → **Branch B** (extend SSA, then delete). Delta >0, residual needs
   fresh pre-SSA IR / same-block adjacency → **Branch A** (keep flat).

**Branch A:** A1 UTs · A2 relocate · A3 flat-DSL if it fits · A4 extract shared core with
the SSA analog · A5 byte-identical object-diff at -O2. ★terminal.

**Branch B:** B1 SSA UTs · B2 extend SSA analog to close the gap · B3 remove flat call
site, object-diff neutral at -O2 · B4 delete body+proto+UT once fuzz differential clean. ★terminal.

Gate = `make test` + object-diff delta. Fuzz differential (`diff_olevels`) is **user-run**, never here.

## Measuring the SSA gap

`TCC_DISABLE_PASS=<table-name>` gates every flat pipeline-table pass
([`pipeline_run.c`](../source/opt/engine/pipeline_run.c) `tcc_ir_opt_pass_disabled`), so the
gap is measurable **with no rebuild**: A/B the 4257-test corpus at -O2 and diff per-function
sizes (`regression_disasm.run_csv_mode` + `record._parse_codesize_csv`). ~21s per run.

**Faithfulness check first:** the knob only covers passes reached *through the table*. Grep
for bare `tcc_ir_opt_<name>(ir)` in `function_pipeline.c` / compound passes
(e.g. `esp_cleanup`); if any exist, stub the body instead and use
`compare_worktree --baseline-commit HEAD --opt o2`.

**net-0 on the corpus ≠ subsumed** — `test_codegen_asm.py` covers shapes the corpus does
not. Always `make test` before deleting a net-0 pass.

**Deleting a pass file ≠ deleting its passes.** Grep the file's *other* exported symbols
too: shared helpers hide in pass files (`gsym_cse_insert_before` lived in the otherwise-dead
`memory/load_cse.c` and had 3 live users).

## Measured -O2 gap — every live flat propagation/memory pass (2026-07-19)

Gap = corpus bytes with the pass disabled minus enabled; i.e. what the flat pass still buys
over the SSA analogs. Baseline 20229 funcs / 759497 bytes.

| Pass | Gap | Funcs | State |
|---|---|---|---|
| `self_arith` | +26 | 2 | [A★] pr70222-1 pre-SSA 64-bit-shift cascade |
| `self_copy_elim` | +41 | 5 | [A★] no SSA analog |
| **`string_calls`** | **+12** | **10** | **[B] memchr ported (was +65/13); residual = phase ordering** |
| `add_reassoc` | +59 | 61 | [A★] `ssa:reassoc` deliberately single-use |
| `cmp_expr_fold` | +105 | 4 | [A~] 4 residuals resist; likely terminal A |
| `symref_prop` | +133 | 19 | [A★] enabler for flat `global_init`; SSA runs the inverse |
| `cmp_field_fuse` | +134 | 16 | [A★] no SSA branch-chain fusion |
| `switch_collapse` | +213 | 1 | [A★] arm-equivalence, not constant selector |
| `stack_bool` | +346 | 12 | [A★] raw StackLoc slots — SSA promotes VARs only |
| `bf_insert_extract` | +644 | 225 | [A★] SSA port reverted — geometry destroyed pre-`ssa:narrow` |
| `const_agg_fold` | +740 | 2 | [A★] no SSA analog |
| `var_tmp_fwd` | +2291 | 54 | [A★] |
| `const_prop_tmp` | +2317 | 210 | [A★] shares `_core` with SSA analog |
| `redundant_assign` | +2350 | 54 | [A★] |
| `global_init` | +2518 | 109 | [A★] load-bearing |
| `setif_fuse` | +2745 | 250 | [A★] |
| `value_tracking` | +3008 | 86 | [A★] |
| `const_var_prop` | +3529 | 238 | [A★] |
| `known_bits` | +9967 | 148 | [A★] stateful DSL |

Not table-gated (measured by stubbing): `addrof_var_fwd` **+6** — [B~], last residual
(`pr57321`, `226_fuzz`) needs relaxing the addr-taken forwarding guard (seed-814-class,
fuzz-sensitive). Memory cluster (`sl_forward` +10807, `dse` +1525, `dead_var_store_elim`
+253, `entry_store_prop`) all heavily load-bearing → [A★].

## TODO — open Branch-B candidates

1. **`string_calls`** → `ssa:const_string_fold`. `memchr` handler landed 2026-07-19 (Step 1),
   closing +53: gap **+65/13 → +12/10**, default-pipeline object-diff 0. Residual = three
   `ir/*pure_func_*` at +3 (phase ordering) + seven diffuse +1, against −4 where flat loses.
   Next: Step 2 classification then the Step 3 redirect-table question, both in
   [`plan_ssa_const_string_fold.md`](plan_ssa_const_string_fold.md) § Retiring flat `string_calls`.
2. ~~`symref_prop`~~ — **assessed 2026-07-19 → [A★] terminal.** See § below.
3. ~~`stack_bool`~~ — **assessed 2026-07-19 → [A★] terminal.** See § below.
4. **`addrof_var_fwd`** — [B~] +6, blocked on the fuzz-sensitive guard relaxation.

## Assessed 2026-07-19 — both remaining [B] candidates are Branch A

### `symref_prop` → [A★]

The pass forwards `T = &S+K` into later same-block TMP uses, including deref (`is_lval`)
uses. Its whole value is as a **pre-SSA enabler**: the symref operands it materializes are
what flat `global_init` folds (`is_sym && is_lval` deref of const data → immediate).
Evidence: disabling `global_init` too drops the `symref_prop` gap from +133 to +45, and the
whole `gcc-execute/20031215-1` cluster (+78, `main` 2→41 instr) disappears.

Branch B is blocked twice over: `global_init` (+2518) and `const_agg_fold` (+740) are
themselves terminal-A with no SSA analog, and `ssa:cprop` deliberately runs the **inverse**
rewrite ([`ssa_opt_symref_operand_cse`](../source/opt/ssa/scalar/cprop.c) — `*&sym` → `*Tn`,
reusing a cached address register). Forwarding in SSA would ping-pong with it.

Residual after `global_init`: `strlen-2`/`strlen-3` `test_array_ref` (+30) is *not* a fold
gap — those strlens fold either way; the delta is downstream address-hoist/RA (one
callee-saved base vs. per-use literal loads). Rest is diffuse ±1, −6 where flat loses.

### `stack_bool` → [A★]

Shape (from `-dump-ir-passes=setif_fuse,stack_bool`):

```
StackLoc[-68] <-- #1 [STORE]   ;  JMP merge
StackLoc[-68] <-- #0 [STORE]   ;  merge: TEST_ZERO StackLoc[-68] ; JUMPIF
```

folded to two direct jumps. The slot is always a **frontend-allocated raw `StackLoc`
temp** — a short-circuit bool (`vrp-1`) or an inlined callee's return-value slot
(`test_llong_load_signed`, where `main` goes 8 → 62 instr without the pass). SSA promotion
is VAR-vreg only ([`ir/ssa.c`](../ir/ssa.c) `ssa_var_promotable`, driven by `SSAVarInfo`),
so no phi is ever formed for these slots and every downstream SSA fold is blind to them.

`ssa:or_bool_diamond` is **not** the analog — it folds `acc |= (cond ? 1 : 0)`, a different
consumer. Branch B would need two new SSA capabilities: (1) promotion of raw StackLoc slots
to values/phis, and (2) branch threading on a phi of differing constants (SCCP only
materializes phis whose operands agree). Both are projects in their own right; until they
exist the residual needs pre-SSA IR → Branch A.

A2 done 2026-07-19: extracted out of the shared `branch_fold.c` into
[`source/opt/flat/scalar/stack_bool_diamond.c`](../source/opt/flat/scalar/stack_bool_diamond.c)
(dead `NUM_NONNEG_FUNCS` macro dropped), corpus object-diff 0. `branch_fold.c` now holds
only `float_branch`, which is fuzz-gated for deletion — after that the file goes away.

**Pending user-run fuzz gates before deleting retained bodies:** float_branch · neg_chain_cse ·
single_val_tmp · addrof_var_fwd guard relaxation.

## Out of scope — stay flat by design

No SSA analog; the pre-RA ones are Branch-A relocated under `source/opt/flat/`.

- ARM machine fusions (`fusion_mla`, `deref_indexed`, `disp_fusion`, `chain_fold`,
  `pair_reorder`, `bool_simplify`, `opt_fusion`/`opt_pack64` lowering).
- VLA/alloca lowering and frontend memory-init lowering.
- All post-RA passes (post-RA `dse`, `jump_threading`/`orphan_cmp`, branch-size opt, …).

**A3 assessment:** the kept-flat dataflow passes (`const_prop_tmp`, `setif_or_taut`,
`const_var_prop`, `value_tracking`, `const_agg_fold`, `symref_prop`) are **not** DSL-portable
— whole-function/merge-aware analyses, not opcode peepholes. `symref_prop` in particular is a
single linear sweep carrying a tmp→symref map; the per-instruction gen model can only express
it by re-scanning per producer, which is quadratic in block size. A3 terminal-N/A.

## Terminal — retired (SSA subsumes)

`const_prop` · `ptr_load_cse` · `diamond_store_fwd` · `ptr_store_load_fwd` ·
`store_redundant` · `lea_cse` · `bool_cse` · `cse_param_add` · `local_alu_cse` ·
`redundant_init_elim` · `deref_fwd` · `neg_chain_cse` · `single_val_tmp` · `vrp` ·
`stack_nonnull` · `float_branch` · `copy_prop` · `return_reuse`

`copy_prop` (`cse_global_load` + `local_load_cse`) and `return_reuse`
(`return_const_reuse`) were deleted 2026-07-19: both had **zero call sites** — SSA
(`ssa:load_cse`, `ssa:branch` `branch_retreuse`) already owned them. Object-diff 0 by
construction, so no fuzz gate. `gsym_cse_insert_before` was rescued from the deleted
`memory/load_cse.c` into [`source/opt/util/ir_insert.c`](../source/opt/util/ir_insert.c).

## Terminal — kept flat (Branch A)

All passes marked [A★] in the gap table above, plus the memory/DCE cluster
(`sl_forward`, `global_sl_fwd`, `entry_store_prop`, `dse`, `dead_var_store_elim`),
`known_bits`, `const_prop_tmp`, `var_tmp_fwd`, `setif_or_taut`, `globalsym_cse`,
`cmp_offset_fold`, `float_narrow` (inert, UT-only driver), `symref_prop`, `stack_bool`.

## Partial (SSA analog live, flat still runs)

`string_calls` → `ssa:const_string_fold` (only open Branch-B pass; residual +12)

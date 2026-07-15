# Plan: relocate `ssa_opt_cprop.c` to `source/opt/ssa` and port to the DSL

Audience: an implementer (small model) working one phase at a time. Every phase
is independently shippable and ends at the **same gate: `make test` zero delta**.
Do **not** start a phase before the previous phase is green. Do **not** batch
phases. If a phase goes red and you cannot make it green in a few edits, `git
restore` the phase and stop.

This plan follows the established playbook in
[`docs/guides/ssa_porting_guide.md`](guides/ssa_porting_guide.md) — read that
first; this document only adds the cprop-specific specifics.

---

## 0. Current state (verified facts — do not re-derive)

Source file: `ir/opt/ssa_opt_cprop.c` (~1819 lines). It exposes **six** public
entry points (all declared in `ir/opt/ssa_opt.h`):

| Entry point | Kind | DSL? |
|---|---|---|
| `ssa_opt_cprop` | runs `cprop_gens[]` via `ssa_opt_run_gens` + calls `ssa_opt_symref_operand_cse` | gen cluster → **Phase B** |
| `ssa_opt_symref_operand_cse` | block-local operand rewrite, own loop | hand-written, **keep** |
| `ssa_opt_var_forward` | dominator-based VAR value forward | hand-written, **keep** |
| `ssa_opt_var_to_param_forward` | dominator-based VAR→PARAM forward | hand-written, **keep** |
| `ssa_opt_var_const_fold` | VAR constant fold | hand-written, **keep** |
| `ssa_opt_const_prop_tmp` | thin wrapper over the **flat** core `tcc_ir_opt_const_prop_tmp_core` (lives in `ir/opt_constprop.c`, declared in `ir/opt.h:142`) | keep wrapper, **keep** |

The gen cluster (`cprop_gens[]` at line ~938) has only two table entries, each a
**chain dispatcher** (same pattern as `source/opt/ssa/scalar/fold.c`):

- `ASSIGN` → `ssa_gen_cprop_assign_any` (line ~888) dispatches by src tag to:
  `ssa_gen_cprop_assign` (TEMP), `ssa_gen_cprop_copy_param` (PARAM/VAR),
  `ssa_gen_cprop_copy_var_stackoff` (STACKOFF), `ssa_gen_cprop_symref_cse`
  (SYMREF), `ssa_gen_cprop_imm` (IMM32/F32 — **currently unreachable-by-design**,
  see the comment at line ~898; preserve that).
- `LOAD` → `ssa_gen_cprop_load_any` (line ~918) tries `ssa_gen_cprop_copy_param`
  then `ssa_gen_cprop_load_redundant`.

Reference ports to imitate:
- Chain dispatcher + PAIR: `source/opt/ssa/scalar/fold.c`
  ([[fold-dsl-chain-dispatcher-port]]).
- VAR-memory forwarding that stays hand-written / uses the `VAR_IMM` primitive:
  `source/opt/ssa/scalar/var_imm_prop.c` +
  `source/opt/framework/opt_dsl_var_const.h`.
- Build-wiring reference: `source/opt/ssa/scalar/strength.c`.

Wiring references that currently point at `ir/opt/ssa_opt_cprop.c` (grep-verified):
- Top `Makefile` `IR_FILES` (line ~328).
- `tests/unit/arm/armv8m/Makefile` lines **197** and **1168**.
- `tests/selfhost/test_selfhost_compile.py` line **81**.
- The UT `tests/unit/arm/armv8m/test_ssa_opt_cprop.c` is **already wired** into
  UT11 (`UT11_SRCS` line 1195 + `test_main11.c` declares & runs suite
  `ssa_opt_cprop`). Do **not** re-wire it; only repoint the module `.c` refs.

---

## Guardrails (read once, apply every phase)

1. **Faithful port.** No behavior change. The gate is `make test` at **zero
   delta** vs. the pre-change count, plus the UT11 `ssa_opt_cprop` suite (67
   tests) staying green. Do **not** run the fuzzer or `diff_olevels`.
2. **Comments:** obey [[comments-max-one-liner]] — no comment blocks; at most a
   single-line comment for a real constraint. The existing file has big comment
   blocks; when you touch a function you may compress them, but in Phase A
   (plain move) leave them alone to keep the diff a pure relocation.
3. **File header:** the new `.c`/`.h` files must start with the standard
   copyright block (see CLAUDE.md).
4. Establish the baseline count **before touching anything**:
   ```bash
   ASAN_OPTIONS=detect_leaks=0 make test -j16 2>&1 | tail -3   # record "N passed"
   ```

---

## Phase A — plain relocation (NO DSL). Ship this first.

Goal: move the whole file, byte-for-byte, into the componentized tree and fix
the build. Zero logic edits.

### A1. Create the implementation file
```bash
git mv ir/opt/ssa_opt_cprop.c source/opt/ssa/scalar/cprop.c
```
(Keep it as one file — do not split the six entry points yet.)

### A2. Extract the public API into a component header
Create `source/opt/ssa/include/opt/ssa/cprop.h` with the standard file header and:
```c
#pragma once
struct IRSSAOptCtx;
int ssa_opt_cprop(struct IRSSAOptCtx *ctx);
int ssa_opt_symref_operand_cse(struct IRSSAOptCtx *ctx);
int ssa_opt_var_forward(struct IRSSAOptCtx *ctx);
int ssa_opt_var_to_param_forward(struct IRSSAOptCtx *ctx);
int ssa_opt_var_const_fold(struct IRSSAOptCtx *ctx);
int ssa_opt_const_prop_tmp(struct IRSSAOptCtx *ctx);
```
Then in `ir/opt/ssa_opt.h` **delete** the six `int ssa_opt_*` declarations for
exactly these symbols (currently lines ~127, 139, 146, 147, 151; leave the
surrounding comments/other decls). Confirm the exact set first:
```bash
grep -n "ssa_opt_cprop\|ssa_opt_symref_operand_cse\|ssa_opt_var_forward\|ssa_opt_var_to_param_forward\|ssa_opt_var_const_fold\|ssa_opt_const_prop_tmp" ir/opt/ssa_opt.h
```

Add the include to **every consumer** `.c`/`.h`. Find them (authoritative — do
not trust this list, run it):
```bash
grep -rln "ssa_opt_cprop\|ssa_opt_symref_operand_cse\|ssa_opt_var_forward\|ssa_opt_var_to_param_forward\|ssa_opt_var_const_fold\|ssa_opt_const_prop_tmp" \
  --include=*.c --include=*.h ir/ source/ tests/unit/
```
Expected consumers: `ir/opt/ssa_opt.c`, `ir/regalloc.c`, and UT files
(`tests/unit/arm/armv8m/ra_link_stubs.c`, `test_ssa_metamorphic.c`,
`test_ssa_opt_cprop.c`). In each, add `#include "opt/ssa/cprop.h"` (component
path) next to the other `opt/ssa/*.h` includes, and remove any now-stale local
forward declaration of these symbols.

In the moved `source/opt/ssa/scalar/cprop.c`, add the two includes it now needs:
```c
#include "opt/ssa/cprop.h"
#include "opt/ssa/ssa_opt_helpers.h"
```

### A3. Build wiring (4 edits)
1. **Top `Makefile` `IR_FILES` (line ~328):** delete the
   `ir/opt/ssa_opt_cprop.c` token (the file now rides in `$(SSA_OPT_SRC)`).
2. **`source/opt/ssa/Makefile` `SSA_OPT_SRC`:** add a line
   `source/opt/ssa/scalar/cprop.c \` (next to `var_imm_prop.c`). Add
   `source/opt/ssa/include/opt/ssa/cprop.h` to `SSA_OPT_HDRS`.
3. **`tests/unit/arm/armv8m/Makefile`:** repoint lines **197** and **1168** from
   `$(TOP)/ir/opt/ssa_opt_cprop.c` to `$(TOP)/source/opt/ssa/scalar/cprop.c`.
4. **`tests/selfhost/test_selfhost_compile.py` line 81:** delete the
   `"ir/opt/ssa_opt_cprop.c",` entry (relocated passes are out of that list's
   scope — a stale path fails its `missing source files` assert).

(`ir/README.md` tree entry is cosmetic — update if convenient, not required.)

### A4. Gotchas specific to cprop
- **Flat-core visibility.** `ssa_opt_const_prop_tmp` calls
  `tcc_ir_opt_const_prop_tmp_core`, declared in `ir/opt.h:142`. After the move,
  make sure that declaration is still visible in `cprop.c`. If `ssa_opt.h` does
  not transitively pull it in and the build errors with an implicit decl, add a
  local `int tcc_ir_opt_const_prop_tmp_core(struct TCCIRState *ir);`
  forward declaration in `cprop.c` (do **not** `#include "opt.h"` from the
  source/ tree — that pulls the flat world in).
- **Helper collisions.** If `cprop.c` defines a static helper whose name now
  collides with one already in `ssa_opt_helpers.h` (e.g. `is_imm32`,
  `is_i64f64`), delete the local copy and use the shared one. Otherwise leave
  all statics as-is.

### A5. Phase A gate
```bash
make cross                                              # clean under -Werror; cprop.o in link line
cd tests/unit/arm/armv8m && make build_ssaopt/run_unit_tests_ssaopt
ASAN_OPTIONS=detect_leaks=0 ./build_ssaopt/run_unit_tests_ssaopt 2>&1 | grep -A30 "suite ssa_opt_cprop"   # 0 failed
cd - && ASAN_OPTIONS=detect_leaks=0 make test -j16 2>&1 | tail -3   # must equal baseline N
```
If the "N passed" equals the baseline, **commit Phase A** and stop. This is a
complete, shippable deliverable on its own.

---

## Phase B — port the gen cluster to the DSL

Only the gen functions feeding `cprop_gens[]` are in scope. The five
hand-written entries (`symref_operand_cse`, `var_forward`,
`var_to_param_forward`, `var_const_fold`, `const_prop_tmp`) are **out of scope
for Phase B — do not touch them** (see Phase D).

Do this **one sub-gen at a time**, re-running the Phase A gate after each. Order
from simplest to hardest: `cprop_assign` → `cprop_imm` → `cprop_copy_param` →
`cprop_copy_var_stackoff` → `cprop_load_redundant` → `cprop_symref_cse`.

### B1. Keep the chain dispatchers; convert the leaves
The table stays two entries (one per opcode — `ssa_opt_run_gens` matches the
first gen for an op then breaks, so you must keep exactly one entry per op).
`ssa_gen_cprop_assign_any` / `..._load_any` remain plain chain functions that
call the converted leaf dispatchers in the **exact same order** — this is the
`fold_binary` pattern from `source/opt/ssa/scalar/fold.c`. Do not reorder or
newly enable `cprop_imm` in the ASSIGN chain (preserve the "intentionally
skipped" behavior at line ~898).

### B2. Convert one leaf to `OPT_GEN_SSA`
For a per-instruction match-and-rewrite leaf, replace its hand-written body with:
```c
OPT_GEN_SSA(cprop_assign, TCCIR_OP_ASSIGN) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });   /* first stmt, non-empty */
  GUARD(when(<the leaf's exact precondition, using imm()/vreg()/lval()/stackoff()>));
  REWRITE(.new_op = ..., .src1 = ...);
}
```
Then expose it through the chain via `opt_dsl_dispatch_cprop_assign(ctx, idx)`
(the dispatch symbol `OPT_GEN_SSA` generates), keeping the `assign_any`
tag-switch calling those dispatch fns in the original order.

Apply the fidelity rules from the porting guide and [[fold-dsl-chain-dispatcher-port]]:
- `IR_CONSTRAINT_IMM` is broader than `is_imm32` (ignores `is_lval`); keep the
  exact `.u.imm32` predicate in a `GUARD`, use the constraint only as a
  pre-filter.
- Mutating-but-return-0 steps (materializing a const into an operand without
  firing a rewrite) stay as a plain helper called from the chain, **not** a
  rule — `REWRITE` always returns 1.
- A leaf that forwards a source vreg past its producer is a **def-use peephole**
  → use `PAIR` / `RETIRE_PAIR` from `opt_dsl_ssa.h` (reference:
  `source/opt/ssa/scalar/narrow.c`). `PAIR` bails the whole dispatch on
  mismatch, so "try both operand orders" must be split into two rules.
- Any rule that forwards/matches a vreg across sites needs single-def
  (`def_count == 1`), not just TEMP-ness ([[fold-dsl-chain-dispatcher-port]]).

### B3. Copy-forwarding leaves (`cprop_copy_param`, `cprop_copy_var_stackoff`)
These forward **memory-backed VAR/PARAM/stackoff** values with a dataflow safety
scan — the same shape that resisted plain `OPT_GEN_SSA` in
`var_imm_prop.c`. Prefer reusing the existing primitive in
`source/opt/framework/opt_dsl_var_const.h` (`VAR_IMM` /
`opt_dsl_var_imm_state_build` / `opt_dsl_var_imm_dominates`) rather than a raw
rewrite. If the leaf's analysis does not fit that primitive's shape, **leave the
leaf hand-written** and note why in a one-line comment — a partial DSL port is
acceptable and still passes the gate. Do not invent a new framework primitive in
this phase.

### B4. Gate after each leaf
Re-run the Phase A gate (§A5). Zero delta + UT11 `ssa_opt_cprop` green. Commit
per leaf so a regression bisects to one conversion.

---

## Phase C (optional, only after B is green) — tidy

- Consider splitting the VAR-forwarding trio (`var_forward`,
  `var_to_param_forward`, `var_const_fold`) into
  `source/opt/ssa/scalar/var_forward.c` with its own component header, mirroring
  how `var_imm_prop.c` is its own file. This is a pure relocation (same Phase A
  mechanics, its own gate). Skip unless asked.
- Compress the large comment blocks per [[comments-max-one-liner]] on functions
  you rewrote in Phase B (do not do a repo-wide comment sweep).

---

## Phase D — explicitly out of scope (do not touch)

The five hand-written entries stay hand-written, exactly as
`var_imm_prop.c`/`reassoc.c` precedent established: they need real dominator
checks, whole-function precompute, block-local barriers, or wrap the flat core.
Forcing them onto `OPT_GEN_SSA` would be unfaithful. Leave them as plain static
C inside `cprop.c` (they moved with the file in Phase A; that is their final
home unless Phase C splits them).

---

## Master gate (definition of done)

```bash
make cross                                             # clean, -Werror, cprop.o linked
cd tests/unit/arm/armv8m && make build_ssaopt/run_unit_tests_ssaopt
ASAN_OPTIONS=detect_leaks=0 ./build_ssaopt/run_unit_tests_ssaopt   # 0 failed, ssa_opt_cprop 67/67
cd - && ASAN_OPTIONS=detect_leaks=0 make test -j16     # "N passed" == recorded baseline
grep -rn "ir/opt/ssa_opt_cprop.c" Makefile tests/ ir/  # must return nothing
```
`make test` zero delta is the hard requirement. If red and not green in a few
edits: `git restore .` the current phase and stop.

## Rollback
Each phase is one commit. `git revert <phase-commit>` restores the prior green
state; no cross-phase entanglement because every phase gates independently.

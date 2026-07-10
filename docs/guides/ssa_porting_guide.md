# Porting an SSA optimization pass to `source/opt/ssa`

How to relocate an `ir/opt/ssa_opt_*.c` pass into the componentized
`source/opt/ssa/` tree, optionally refactoring it onto the DSL framework in
`source/opt/framework`. Written to keep the next port a short, mechanical loop
instead of a rediscovery exercise. Reference port: `ssa_opt_strength`
(`git log` around `source/opt/ssa/strength.c`).

## The target layout

```
source/opt/ssa/<pass>.c                       implementation
source/opt/ssa/include/opt/ssa/<pass>.h       public pass-entry decl
source/opt/ssa/include/opt/ssa/ssa_opt_helpers.h   shared static-inline helpers
```

Include paths are `-Isource/opt/ssa` **and** `-Isource/opt/ssa/include`
(already in `SSA_OPT_INC` and `UT_CFLAGS`). Headers are reached by their
**component path**, e.g. `#include "opt/ssa/strength.h"` — the `opt/ssa/`
prefix is what makes the include collision-proof and unambiguous (there is no
`ir/opt/ssa/` dir, so it can only resolve under `include/`). Add shared
predicates to `ssa_opt_helpers.h`; keep pass-specific helpers `static` in the
`.c`.

## Decide: DSL or hand-written?

Use the DSL (`OPT_GEN_SSA`) when the pass is a **per-instruction
match-and-rewrite** keyed on one opcode: strength reduction, algebraic
identities, single-op canonicalization.

**Def-use peepholes** — the rewrite depends on the SSA def of a source operand
and forwards past it ("if src1's producer is a SHL, fold the pair") — are also
DSL-expressible via `opt_dsl_ssa.h`'s `PAIR` / `RETIRE_PAIR` (see the framework
spec's *SSA cross-instruction peepholes* section; reference port
`source/opt/ssa/narrow.c`). `PAIR` binds the producer and its operands; a
`RETIRE_PAIR` before the `REWRITE` handles the use-list surgery.

Phi simplification rules can use `opt_dsl_phi.h`'s `OPT_GEN_PHI` /
`PATTERN_PHI` / `REWRITE_PHI` surface and fixed-point runner. Other work needing
cross-instruction analysis, CFG/dominators, or genuine multi-instruction
rewrites (more than one producer, inserting instructions) stays hand-written.

### DSL cheat-sheet

```c
#define USING_GLOBALS
#include "ir.h"            /* first: IROperand, setters */
#include "ssa_opt.h"       /* IRSSAOptCtx, IRSSAOptGen, ssa_opt_run_gens */
#include "opt_dsl.h"       /* the DSL */
#include "opt/ssa/<pass>.h"
#include "opt/ssa/ssa_opt_helpers.h"

OPT_GEN_SSA(rule_name, TCCIR_OP_MUL) {
  int k = 0;
  PATTERN(.constraints = { .src2 = IR_CONSTRAINT_IMM });   /* first stmt; keep non-empty */
  GUARD(when(is_imm32(src2)); and(is_power_of_2((uint32_t)src2.u.imm32, &k)));
  REWRITE(.new_op = TCCIR_OP_SHL, .src2 = mk_imm(k));       /* .new_op defaults to keep */
}

static const IRSSAOptGen <pass>_gens[] = { OPT_GEN_ENTRY(rule_name, TCCIR_OP_MUL) };

int ssa_opt_<pass>(IRSSAOptCtx *ctx)
{ return ssa_opt_run_gens(ctx, <pass>_gens, OPT_DSL_TABLE_COUNT(<pass>_gens)); }
```

- `PATTERN` binds `ir,q,dest,src1,src2`; read operands in GUARD with
  `imm()/vreg()/stackoff()/lval()`, build in REWRITE with `mk_imm()`.
- `GUARD` clauses: `when(e)` sets, `and(e)`/`and_not(e)` accumulate. `&&`
  short-circuits, so a later `and()` that reads an operand is safe when an
  earlier `when()` already failed.

## DSL fidelity gotchas (the ones that bite)

1. **`IR_CONSTRAINT_IMM` is broader than you think.** It is
   `irop_is_immediate` = tag ∈ {IMM32,F32,I64,F64}, and it ignores `is_lval`.
   If your rewrite reads the raw `.u.imm32` field, that is only valid for
   `tag==IROP_TAG_IMM32 && !is_lval` — keep that exact check in the GUARD via
   `is_imm32()` (in `ssa_opt_helpers.h`). Use the PATTERN constraint only as a
   cheap pre-filter, never as the semantic gate.
2. **`ssa_opt_run_gens` matches the FIRST gen for an op, then `break`s.** Two
   table entries for the same opcode → only the first runs. Commutative "immediate
   in either source" must live in **one** dispatch: pick with a helper, then
   `REWRITE(.src1 = val_op, ...)`. REWRITE applies any operand field whose tag
   != NONE, so you pass the existing `IROperand` straight through — there is no
   `mk_vreg`, and you don't need one.
3. **REWRITE sets `q->op` before the operands.** The new opcode's operand
   config (`has_dest/has_src1/has_src2`) must match the old one, or the operand
   offsets shift under you. All binary-ALU→binary-ALU rewrites are safe.
4. **`PATTERN` must be the first statement** after any locals it references,
   and empty `{}` is a GNU extension — give it a non-empty initializer
   (`.constraints = { .dest = IR_CONSTRAINT_ANY }` is a harmless no-op).
5. `static inline` helpers in a shared header avoid `-Wunused-function` in TUs
   that don't use them all; plain `static` in a header does not.

## Build-wiring checklist (5 spots + the header extraction)

1. **Top `Makefile` `IR_FILES`**: delete the `ir/opt/ssa_opt_<pass>.c` entry
   (the new file rides in `$(SSA_OPT_SRC)`).
2. **`source/opt/ssa/Makefile`**: add `.c` to `SSA_OPT_SRC`; add the new
   header(s) to `SSA_OPT_HDRS`; add `$(OPT_DSL_HEADERS)` there too if it uses the
   DSL. `OPT_DSL_INC`/`SSA_OPT_INC` are already in the top Makefile's global
   `DEFINES`, so nothing else is needed for the core build.
3. **Extract the API**: remove `int ssa_opt_<pass>(...)` from
   `ir/opt/ssa_opt.h`, put it in `source/opt/ssa/include/opt/ssa/<pass>.h`
   (forward-declare `struct IRSSAOptCtx;`), then add
   `#include "opt/ssa/<pass>.h"` to every consumer. For the reference port those
   were: `ir/opt/ssa_opt.c`, `ir/regalloc.c`, and the UT files below. Grep first:
   `grep -rln ssa_opt_<pass> --include=*.c`.
4. **`tests/unit/arm/armv8m/Makefile`**: repoint the two `$(TOP)/ir/opt/…` refs
   (in `UT_COVERAGE_ONLY_SRCS` and `UT11_MODULE_SRCS`) to
   `$(TOP)/source/opt/ssa/<pass>.c`. `UT_CFLAGS` already carries
   `-I…/source/opt/ssa`, `-I…/source/opt/ssa/include`, and
   `-I…/source/opt/framework` — no flag change unless you add a new include root.
5. **`tests/selfhost/test_selfhost_compile.py`**: drop the hard-coded
   `ir/opt/ssa_opt_<pass>.c` string. That list has no `source/opt/*` entries by
   design — relocated passes are simply out of its scope. A stale path fails the
   `missing source files` assert (which fires *before* the no-sysroot skip).
6. **`ir/README.md`** tree entry (cosmetic).

## Wire the unit test (don't leave it orphaned)

A dedicated `tests/unit/arm/armv8m/test_ssa_opt_<pass>.c` may already exist but
be unreferenced (compiled by nothing, run by nothing). Wire it into UT11:

- add `test_ssa_opt_<pass>.c` to `UT11_LOCAL_SRCS`;
- add `UT_DECLARE_SUITE(ssa_opt_<pass>)` + `UT_RUN_SUITE(ssa_opt_<pass>)` to
  `test_main11.c`.

Builder helpers (`utb_temp/utb_imm/utb_lval`, `ssa_add_instr3`, `ssa_ctx_*`)
come from `ir_build.h`/`ssa_build.h`; `.vr`/`.tag`/`.u.imm32` are real
`IROperand` fields. Confirm the suite actually *runs* (grep the run output for
its test names), not just links.

## Validation gate (do these, in order)

```bash
make cross                                   # clean under -Werror; strength.o in link line
cd tests/unit/arm/armv8m
make build_ssaopt/run_unit_tests_ssaopt
ASAN_OPTIONS=detect_leaks=0 ./build_ssaopt/run_unit_tests_ssaopt   # 0 failed; +N tests
cd - && ASAN_OPTIONS=detect_leaks=0 make test -j16                 # delta 0
```

A behavior-preserving port must land `make test` at **zero delta**. Do not run
the fuzzer or `diff_olevels` — the gate is `make test` + UT + a clean build.
`ASAN_OPTIONS=detect_leaks=0` suppresses ASan leak-exit false failures.

## Naming note

In-tree passes are `ssa_opt_<name>`; the first relocations
(`tcc_ir_ssa_opt_const_string_fold`, `…_bitop_const_fold`) took the
`tcc_ir_ssa_opt_` public-API prefix. Keeping the existing `ssa_opt_<name>`
symbol on a move is cheapest (only the declaration relocates). Rename only if
you want the `tcc_ir_` prefix — that touches every call site and the UT stub.

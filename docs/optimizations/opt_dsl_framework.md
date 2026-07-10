# Optimization DSL Framework

> Source: `source/opt/framework/`
> Design spec: [docs/optimization_dsl_proposal.md](../optimization_dsl_proposal.md)

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  User code: OPT_GEN_SSA(my_pass, OP) { ... }            │
├─────────────────────────────────────────────────────────┤
│  L1 — Pattern grammar (PATTERN / GUARD / REWRITE)       │
│  L2 — Generator table entry (OPT_GEN_ENTRY)             │
│  L3 — Pass descriptor (existing pipeline, no change)    │
├─────────────────────────────────────────────────────────┤
│  Helpers: constraint checks, guard DSL, rewrite API     │
│  Types: constraint enums, spec structs                  │
└─────────────────────────────────────────────────────────┘
         │
         ▼
   Real C: tcc_ir_set_src1/2/dest, q->op, etc.
```

## Files

| File | Purpose |
|------|---------|
| `opt_dsl.h` | Main header — `OPT_GEN_SSA` / `OPT_GEN_FLAT` / `PATTERN` / `GUARD` / `REWRITE` |
| `opt_dsl_types.h` | Type definitions: `IROptConstraint`, `IROptPatternSpec`, `IROptRewriteSpec` |
| `opt_dsl_helpers.h` | Helper macros: binding accessors, constraint checks, guard DSL, rewrite API |
| `opt_dsl_entry.h` | Table entry macros: `OPT_GEN_ENTRY`, `OPT_GEN_ENTRY_FLAT`, `OPT_DSL_TABLE_COUNT` |
| `opt_dsl_ssa.h` | SSA-only cross-instruction (def-use) support: `PAIR` / `RETIRE_PAIR` |
| `opt_dsl_phi.h` | SSA phi rules and fixed-point CFG traversal |
| `example_strength.c` | Standalone smoke test |

## Design Principles

1. **C-native** — No code generation step, no external tools. Pure C expansion macros.
2. **Generative, not interpreted** — Macros expand to ordinary imperative C at compile time. No runtime table-walking match engine.
3. **Observable** — Every match fires through `TCC_TRACE_OPT` hook.
4. **Incremental** — A generator is 100% DSL or 0% DSL. DSL and imperative generators coexist.
5. **Self-hostable** — C11-only: variadic macros, designated initializers, compound literals. No nested functions, statement expressions, or C23 features.

## Usage

```c
#include "ir.h"
#include "opt/optimization_dsl.h"

OPT_GEN_SSA(my_pass, TCCIR_OP_MUL) {
  PATTERN(
    .constraints = { .src1 = IR_CONSTRAINT_VREG,
                     .src2 = IR_CONSTRAINT_IMM });
  GUARD(
    when(ir_op_any(q->op, IROP_P_ALU))
    and(is_power_of_two(imm(src2))));
  REWRITE(
    .new_op = TCCIR_OP_SHL,
    .src2   = imm(ctz64(imm(src2))));
}

const IRSSAOptGen my_pass_gens[] = {
  OPT_GEN_ENTRY(my_pass, TCCIR_OP_MUL),
};
```

## SSA cross-instruction peepholes (`PAIR` / `RETIRE_PAIR`)

`PATTERN`/`GUARD`/`REWRITE` are single-instruction: they cannot look at the SSA
def of a source operand or maintain use lists. Def-use peepholes — "the producer
of my src1 is a SHL, fold the pair" — used to stay hand-written. `opt_dsl_ssa.h`
(SSA engine only; include after `ir.h` + `ssa_opt.h`) closes that gap with two
macros that bracket the rewrite:

```c
OPT_GEN_SSA(narrow_ubfx, TCCIR_OP_UBFX) {
  PATTERN(.constraints = { .src2 = IR_CONSTRAINT_IMM });
  PAIR(.link = IR_PAIR_DEF_OF_SRC1, .op = TCCIR_OP_SHR, .single_use = 1);
  GUARD(when(is_imm32(src2) && is_imm32(psrc2)));
  /* ... read psrc1/psrc2, compute the merged field ... */
  RETIRE_PAIR(psrc1, /*delete_second=*/1);   /* SHR is now dead */
  REWRITE(.new_op = TCCIR_OP_UBFX, .src1 = psrc1, .src2 = mk_imm(...));
}
```

- **`PAIR(.link, .op, .single_use)`** — placed right after `PATTERN`. Resolves
  the linked source (`IR_PAIR_DEF_OF_SRC1` / `_SRC2`) to its single-def TEMP-vreg
  producer, checks the producer opcode (`.op = -1` for any) and, if `.single_use`,
  that its result has exactly one use. Bails the dispatch on any miss, else binds
  `pidx` / `pop` / `pvi` / `pdest` / `psrc1` / `psrc2`.
- **`RETIRE_PAIR(new_linked, delete_second)`** — called once guards pass, just
  before `REWRITE`, when the rewrite forwards instruction `i` past the producer
  (its linked source becomes `new_linked`). `delete_second = 1` when the producer
  was used only here (it dies: use count zeroed + NOP'd); `= 0` to just drop the
  single use edge (the producer may survive, or is left for DCE).

The single-instruction macros are unchanged, so this is opt-in — passes that
don't call `PAIR` are unaffected. Reference: `source/opt/ssa/narrow.c`.

## Multi-rule generators (`opt_dsl_chain`) and use-edge helpers

When one opcode has several candidate rewrites tried in order (the old
monolithic generators), keep each as its own `OPT_GEN_SSA` rule and register a
plain chain function in the gen table; the try-order becomes a data table:

```c
static const OptDslSSARule my_rules[] = {
  opt_dsl_dispatch_rule_a, opt_dsl_dispatch_rule_b, /* order = priority */
};
static int my_chain(IRSSAOptCtx *ctx, int i)
{ return opt_dsl_chain(ctx, i, my_rules, OPT_DSL_TABLE_COUNT(my_rules)); }
```

Steps that must *stop* the chain regardless of success (e.g. a barrel-annotated
op that may only const-fold) stay as explicit `return dispatch(...)` lines in
the chain function. Rules whose reachability depended on an earlier terminal
must encode it locally in their GUARD (see `fold_identity_src2`'s
both-imm exclusion in `source/opt/ssa/fold.c`).

For rewrites the `RETIRE_PAIR` machinery doesn't cover, `opt_dsl_ssa.h` also
provides operand-level use-edge maintenance:

- `opt_dsl_drop_use(ctx, op, i)` — drop instruction `i`'s use edge for `op`'s
  vreg (no-op unless a tracked TEMP).
- `opt_dsl_add_use(ctx, op, i)` — add a use edge for a plain (non-sym) vreg
  operand newly consumed by `i`.

`opt_dsl_helpers.h` additionally has `mk_imm_bt(v, bt)` — `mk_imm` with an
explicit btype (e.g. the folded dest's width). Reference:
`source/opt/ssa/fold.c`.

## Phi rules

Phi nodes live on CFG block lists rather than in the compact instruction array.
`opt_dsl_phi.h` provides a separate rule shape for them:

```c
OPT_GEN_PHI(phi_trivial)
{
  PATTERN_PHI(.kind = IR_PHI_PATTERN_TRIVIAL);
  REWRITE_PHI(.replacement = replacement_vreg);
}
```

Register rules with `OPT_PHI_ENTRY` and run them through
`opt_dsl_run_phi_rules`. The runner applies replacements, preserves phis whose
uses cannot be rewritten, removes matched phis, and repeats to a fixed point.
Reference: `source/opt/ssa/cfg/phi.c`.

## Building

```bash
make cross OPT_DSL_EXAMPLES=yes   # build with DSL examples
make opt-dsl-check                # verify DSL headers parse under gcc
make opt-dsl-expand               # expand DSL headers with gcc -E
```

## Dependencies

The DSL depends on the predicate framework (L1/L2/L4/L5) for:
- `ir_op_any()` / `IROP_P_*` — op-property masks
- `ir_q_barrel_shifted()` / `irop_is_direct_stack_slot()` — operand queries
- `ssa_single_use()` — single-use check
- Guard DSL composition (`when`/`and`/`and_not`)
- Mutation funnel (L5) for Expand-mode inserts

See [docs/optimization_dsl_proposal.md](../optimization_dsl_proposal.md) §13 for the full dependency gate.

## Migration Path

See [docs/optimization_dsl_proposal.md](../optimization_dsl_proposal.md) §8 for the staged migration plan:

1. Land framework L1/L2/L4/L5 (separate effort)
2. Add DSL headers (this directory)
3. Convert 1–2 trivial generators
4. Measure: `gcc -E` output, `make test`, `TCC_PASS_TIMING`
5. Convert 5–10 medium generators
6. Convert ARM fusion generators
7. Remaining generators opt-in

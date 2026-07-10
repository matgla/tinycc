# Optimization DSL Framework

Declarative, generative macro layer on top of the existing IR generator model.

**Full spec:** [docs/optimizations/opt_dsl_framework.md](../../docs/optimizations/opt_dsl_framework.md)

## Quick Start

```c
#include "ir.h"        /* must come first — provides IROperand, setters */
#include "opt_dsl.h"   /* found via -Isource/opt/framework */

OPT_GEN_SSA(my_pass, TCCIR_OP_MUL) {
  int shift;
  PATTERN(.constraints = { .src1 = IR_CONSTRAINT_VREG, .src2 = IR_CONSTRAINT_IMM });
  GUARD(when(is_power_of_2((uint32_t)imm(src2), &shift)));
  REWRITE(.new_op = TCCIR_OP_SHL, .src2 = mk_imm(shift));
}

static const IRSSAOptGen my_pass_gens[] = {
  OPT_GEN_ENTRY(my_pass, TCCIR_OP_MUL),
};
```

`PATTERN` must be the first statement in the body (after any locals it references);
it binds `ir`, `q`, `dest`, `src1`, `src2` and the guard accumulator. In a `GUARD`,
`imm(x)`/`vreg(x)`/`stackoff(x)` *read* an operand; in a `REWRITE`, `mk_imm(v)`
*builds* one. `.new_op` defaults to keeping the current opcode.

## Build

```bash
make cross OPT_DSL_EXAMPLES=yes   # compile the example generator TU
make opt-dsl-check                # compile the example (fails on any DSL error)
make opt-dsl-expand               # expand the example with gcc -E
```

## Files

| File | Purpose |
|------|---------|
| `opt_dsl.h` | Main header — `OPT_GEN_SSA` / `OPT_GEN_FLAT` / `PATTERN` / `GUARD` / `REWRITE` |
| `opt_dsl_types.h` | Type definitions |
| `opt_dsl_helpers.h` | Helper macros |
| `opt_dsl_entry.h` | Table entry macros |
| `opt_dsl_ssa.h` | SSA-only def-use peephole support — `PAIR` / `RETIRE_PAIR` |
| `example_strength.c` | Smoke test |

## Dependencies

See [docs/optimization_dsl_proposal.md](../../docs/optimization_dsl_proposal.md) §13.

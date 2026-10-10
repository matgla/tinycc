# Optimization DSL Framework

Declarative, generative macro layer on top of the existing IR generator model.

**Full spec:** [docs/optimizations/opt_dsl_framework.md](../../docs/optimizations/opt_dsl_framework.md)

## Quick Start

```c
#include "ir.h"        /* must come first — provides IROperand, setters */
#include "opt_dsl.h"   /* found via -Isource/opt/framework */

OPT_GEN_SSA(my_pass, TCCIR_OP_MUL) {
  int shift;
  MATCH();
  BIND_VREG(src1);
  BIND_IMM(src2);
  GUARD(when(is_power_of_2((uint32_t)imm(src2), &shift)));
  REWRITE(set_op(TCCIR_OP_SHL), set_src2(mk_imm(shift)));
}

static const IRSSAOptGen my_pass_gens[] = {
  OPT_GEN_ENTRY(my_pass, TCCIR_OP_MUL),
};
```

`MATCH()` opens the body (binds `ir`, `q` and the guard accumulator); `BIND*(slot)`
binds only the operands the rule reads. In a `GUARD`, `imm(x)`/`vreg(x)`/`stackoff(x)`
*read* an operand; `REWRITE(set_op(..), set_srcN(..), ...)` applies its setters in
order, and `mk_imm(v)` *builds* an operand.

## Build

```bash
make cross OPT_DSL_EXAMPLES=yes   # compile the example generator TU
make opt-dsl-check                # compile the example (fails on any DSL error)
make opt-dsl-expand               # expand the example with gcc -E
```

## Files

| File | Purpose |
|------|---------|
| `opt_dsl.h` | Main header — `OPT_GEN_SSA` / `OPT_GEN_FLAT` / `MATCH` / `BIND*` / `GUARD` / `REWRITE` |
| `opt_dsl_types.h` | Type definitions |
| `opt_dsl_helpers.h` | Helper macros |
| `opt_dsl_entry.h` | Table entry macros |
| `opt_dsl_ssa.h` | SSA-only def-use peephole support — `PAIR` / `PBIND` / `RETIRE_PAIR` |
| `example_strength.c` | Smoke test |

## Dependencies

See [docs/optimization_dsl_proposal.md](../../docs/optimization_dsl_proposal.md) §13.

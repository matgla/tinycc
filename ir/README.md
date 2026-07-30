# TCC IR Subsystem - Internal Modules

## Overview

This directory contains internal IR module headers and implementation files.
These are NOT part of the public API - they are implementation details.

The public API is in `tccir.h` at the project root.

## Directory Structure

```
ir/
├── README.md           # This file
├── ir.h               # Internal IR header (includes all modules)
│
├── Core infrastructure
│   ├── core.h/c         # Core IR definitions, opcode table, IRBuilder
│   ├── type.h/c         # Type helpers
│   ├── pool.h/c         # Operand pool management
│   ├── vreg.h/c         # Virtual register management
│   ├── live.h/c         # Liveness analysis
│   ├── stack.h/c        # Stack layout
│   ├── mat.h/c          # Value materialization
│   ├── dump.h/c         # Debug dumping
│   ├── operand.h/c      # IROperand definitions
│   └── machine_op.h/c   # Machine-specific opcode helpers
│
├── SSA
│   └── ssa.h/c          # SSA form construction and manipulation
│
├── Code generation
│   ├── codegen.h/c        # Codegen helpers
│   ├── regalloc.h/c       # Register allocation (pre-RA passes: source/opt/ra/)
│   └── gen/               # IR instruction generators
│       ├── arith.c        # Arithmetic instruction gen
│       ├── asm.c          # Assembler instruction gen
│       ├── config.c       # Configuration helpers
│       ├── control.c      # Control flow gen
│       ├── float.c        # Floating-point gen
│       ├── jump.c         # Jump/branch gen
│       ├── live.c         # Live range gen
│       ├── params.c       # Parameter handling gen
│       ├── put.c          # Store/put gen
│       ├── softfloat.c    # Soft-float gen
│       ├── state.c        # State management gen
│       └── vla.c          # VLA handling gen
│
└── Build
    └── Makefile           # (project root) compiles ir/*.c
```

The optimizer lives entirely under `source/opt/` — no pass, engine, or
interface header remains here:

```
source/opt/include/     opt.h, opt_utils.h, opt_du.h, opt_alias.h, opt_engine.h,
                        opt_pipeline.h, opt_xform.h, opt_gens_fusion.h,
                        opt_loop_utils.h, opt_loop_const_sim.h, opt_reroll.h,
                        licm.h, tccopt.h   (-I$(TOP)/source/opt/include)
source/opt/util/        shared helpers (const eval, cond tokens, purity, ...)
source/opt/analysis/    def-use chains, stack-slot aliasing
source/opt/engine/      pass context, gen drivers, pipeline tables, registry
source/opt/flat/        pre-SSA passes  (scalar/cfg/fusion/memory/dce/loop/ipa)
source/opt/ssa/         SSA passes + engine/, include/ssa_opt.h
source/opt/ra/          pre-RA cleanup passes
source/opt/framework/   the OPT_GEN DSL
```

## Module Architecture

The IR pipeline processes IR in this order:

1. **IRBuilder** (`core.h/c`) — constructs IR from C AST
2. **SSA** (`ssa.h/c`) — converts to SSA form
3. **Optimizations** (`source/opt/`) — pass-based optimization
4. **Register allocation** (`regalloc.h/c`) — assigns physical registers
5. **Code generation** (`codegen.h/c`, `gen/`) — emits machine code

## Naming Convention

### Internal API: `ir_<module>_<action>()`

Static functions within each module use the `ir_<module>_` prefix.

### Public API: `tcc_ir_<action>()`

Functions exported to the rest of the compiler (declared in `tccir.h`)
use the `tcc_ir_` prefix.

## Including Headers

All module headers are included via `ir/ir.h`, which in turn includes `tcc.h`
(first, for VT_* definitions) and then all public module headers. Internal
`.c` files include `ir.h`; external code includes `tccir.h`.

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
├── Optimization pipeline
│   ├── opt.h/c          # Optimization framework, pass engine, pipeline
│   ├── opt_engine.h/c   # Pass registration and execution engine
│   ├── opt_utils.h/c    # Shared optimization utilities
│   ├── opt_xform.h/c    # IR transformations
│   ├── opt_du.h/c       # Definition-use chains
│   ├── opt_alias.h/c    # Alias analysis
│   ├── opt_constfold.c  # Constant folding
│   ├── opt_constprop.c  # Constant propagation
│   ├── opt_copyprop.c   # Copy propagation
│   ├── opt_dce.c        # Dead code elimination
│   ├── opt_branch.c     # Branch optimization
│   ├── opt_memory.c     # Memory optimization
│   ├── opt_loop.c       # Loop optimizations
│   ├── opt_loop_const_sim.c  # Loop constant simulation
│   ├── opt_loop_utils.c     # Loop utility functions
│   ├── opt_fusion.c     # Instruction fusion
│   ├── opt_gens_bool.c      # Boolean generation
│   ├── opt_gens_branch.c    # Branch generation
│   ├── opt_gens_call_result.c  # Call result handling
│   ├── opt_gens_fusion.c    # Fusion generation
│   ├── opt_jump_thread.c    # Jump threading
│   ├── opt_knownbits.c      # Known bits analysis
│   ├── opt_neg_chain.c      # Negation chain elimination
│   ├── opt_pack64.c         # 64-bit packing
│   ├── opt_pipeline.c       # Pipeline scheduling
│   ├── opt_promote.c        # Type promotion
│   ├── opt_reroll.c         # Loop rerolling
│   ├── opt_setif_or_taut.c  # SETIF/tautology optimization
│   ├── opt_switch_data.c    # Switch data optimization
│   ├── opt_dead_vla.c       # Dead VLA elimination
│   ├── opt_dead_lea_store.c # Dead LEA/store elimination
│   ├── opt_const_aggregate.c # Constant aggregate handling
│   └── opt_cmp_fuse.c       # Compare fusion
│
├── SSA
│   ├── ssa.h/c            # SSA form construction and manipulation
│   └── opt/               # SSA-optimized passes
│       ├── ssa_opt.h      # SSA optimization framework header
│       ├── ssa_opt.c      # SSA pass orchestration
│       ├── ssa_opt_gvn.c       # Global value numbering
│       ├── ssa_opt_sccp.c      # Sparse conditional constant propagation
│       ├── ssa_opt_reassoc.c   # Reassociation
│       ├── ssa_opt_loop.c      # Loop optimizations (SSA)
│       ├── ssa_opt_load_cse.c  # Load CSE
│       ├── ssa_opt_cprop.c     # SSA copy propagation
│       ├── ssa_opt_dce.c       # SSA dead code elimination
│       ├── ssa_opt_branch.c    # SSA branch optimization
│       ├── ssa_opt_dead_loop.c # Dead loop elimination
│       └── ssa_opt_sccp.c      # (listed above)
│
├── Code generation
│   ├── codegen.h/c        # Codegen helpers
│   ├── regalloc.h/c       # Register allocation
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

## Module Architecture

The IR pipeline processes IR in this order:

1. **IRBuilder** (`core.h/c`) — constructs IR from C AST
2. **SSA** (`ssa.h/c`) — converts to SSA form
3. **Optimizations** (`opt/`, `opt/*.c`) — pass-based optimization
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

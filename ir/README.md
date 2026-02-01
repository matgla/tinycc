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
├── type.h/c           # Type helpers
├── pool.h/c           # Operand pool management
├── vreg.h/c           # Virtual register management
├── live.h/c           # Liveness analysis
├── stack.h/c          # Stack layout
├── mat.h/c            # Value materialization
├── opt.h/c            # Optimizations
├── codegen.h/c        # Codegen helpers
├── dump.h/c           # Debug dumping
└── operand.h/c        # IROperand definitions
```

## Usage

These headers are internal to the IR implementation. They should only be
included by the .c files in this directory, not by external code.

When splitting tccir.c, each new .c file will include "ir.h" which
includes all module headers.

## Naming Convention

### Internal API: `ir_<module>_<action>()`

Static functions within each module use the `ir_<module>_` prefix.

### Public API: `tcc_ir_<action>()`

Functions exported to the rest of the compiler (declared in tccir.h)
use the `tcc_ir_` prefix.

## Migration Plan

1. Create all module headers (DONE)
2. Create all module .c files with implementations
3. Update Makefile to compile ir/*.c
4. Remove original tccir.c
5. Test everything works

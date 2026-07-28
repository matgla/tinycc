# Coding Guidelines

## Build & Test

```bash
make cross -j$(nproc)                       # build armv8m-tcc (rebuild after EVERY edit)
make test -j16                              # IR test suite (primary gate)
python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu   # fuzz self-consistency
```

One-time setup: `./configure` (ASan is ON by default).

## Code Style

- **Compiler flags:** `-std=c11 -Wunused-function -Werror` (treat warnings as build failures).
- **Braces** follow `.clang-format`: function body brace on its own line, inner braces on same line:

```c
void function_name(int arg)
{
  if (condition) {
    do_something();
  } else {
    do_other();
  }
}
```

## File Headers

Every new `.c` and `.h` file MUST start with the standard copyright/license block (an optional one-line description may precede the copyright line):

```c
/*
 *  TCC <Component> - <Short description>
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
```

Every new `.h` file uses `#pragma once` instead of `#ifndef`/`#define` header guards.

## Comments

- No multi-line comment blocks.
- At most a single-line comment, and only for a constraint the code cannot express.
- Delete existing comments in any code you touch (or compress to one line if a real constraint remains).
- If something needs a longer explanation, put it in a `docs/` file and reference it with a one-line link — never inline the prose.

## Memory Ownership

New code must use the ownership utilities under `source/memory/include/memory/` instead of raw `tcc_malloc*`/`tcc_realloc`/`tcc_free` pairs:

- `vector` — growable typed arrays.
- `small_sequence` — fixed-size sequences that stay inline at common sizes and fall back to the heap.
- `dynamic_bitset` — owned bitsets with inline word storage and heap fallback.
- `unique_ptr` — scope-owned single allocations; its deleter is `tcc_free`, so it owns `tcc_malloc*`/`tcc_realloc` memory and **never** plain `malloc` memory.

Raw pointers are acceptable only at existing shared ABI/layout boundaries that cannot yet be represented by these utilities. Keep the framework owner inside the implementation wherever possible, and extend `source/memory` for recurring ownership patterns rather than adding new manual allocation/free pairs.

## IR Naming Conventions

- Internal IR functions: `ir_<module>_<action>()` (static).
- Public IR API (in `tccir.h`): `tcc_ir_<action>()`.
- IR internals live in `ir/` (included via `ir/ir.h`); the public IR interface is `tccir.h`.

## Conventions for Changes

- **Never commit without a regression test** for a bug fix — verbatim or reduced repro under `tests/ir_tests/`, expected output in a `.expect` file.
- New IR opcode → lowering in `arm-thumb-gen.c` + test.
- New asm instruction → builder in `arm-thumb-opcodes.c` + token + parser + test.
- Don't commit the temporary `TCC_SKIP_SSA*` env-var bisection gates — they are investigation-only scaffolding.

## Debug Logging

Unified logging system defined in `log.h`. Each scope is a compile-time switch:

```bash
make CFLAGS+='-DTCC_LOG_ALL=1'          # enable ALL logging scopes
make CFLAGS+='-DTCC_LOG_IR_GEN=1'       # IR generation & optimization passes
make CFLAGS+='-DTCC_LOG_LOOP_OPT=1'     # loop optimization (induction vars)
make CFLAGS+='-DTCC_LOG_IV_SR=1'        # induction variable / strength reduction
make CFLAGS+='-DTCC_LOG_LICM=1'         # loop-invariant code motion
make CFLAGS+='-DTCC_LOG_LS=1'           # linear scan register allocator
make CFLAGS+='-DTCC_LOG_STACK_ALLOC=1'  # stack frame allocation
make CFLAGS+='-DTCC_LOG_CODEGEN=1'      # frontend code generation (tccgen.c)
make CFLAGS+='-DTCC_LOG_INLINE_STRUCT=1' # inline struct return expansion
make CFLAGS+='-DTCC_LOG_CALLSITE=1'     # call site processing
make CFLAGS+='-DTCC_LOG_YAFF=1'         # YAFF object format
make CFLAGS+='-DTCC_LOG_THOP=1'         # thumb opcode encoding trace
make CFLAGS+='-DTCC_LOG_THUMB=1'        # thumb code generation (general)
make CFLAGS+='-DTCC_LOG_MACH=1'         # machine-level store/assign
make CFLAGS+='-DTCC_LOG_BRANCH_OPT=1'   # branch size optimization
make CFLAGS+='-DTCC_LOG_SCRATCH=1'      # scratch register management
make CFLAGS+='-DTCC_LOG_RELOC=1'        # ELF relocation processing
make CFLAGS+='-DTCC_LOG_POOL=1'         # IR memory pool
```

Use `LOG_<SCOPE>(fmt, ...)` macros in code. Output goes to stderr with `[SCOPE]` prefix.

Other debug flags:

```bash
make CFLAGS+='-DCONFIG_TCC_DEBUG'   # enables -dump-ir flag
```

At runtime:

```bash
./armv8m-tcc -dump-ir -c test.c     # dump IR
./armv8m-tcc -vv -c test.c          # verbose output
```

## Debugging an Optimizer Miscompilation

When a fuzz seed diverges between O-levels (`tcc -O0` correct, `-O1`/`-O2` wrong):

1. `scripts/bisect_opt.py --seed N --high=-O1` — QEMU-confirms the culprit
   knob(s) and flags the exact IR line where a memory read is misfolded to a
   constant, naming the pass group and the gated pass functions.
2. Write a **regression test first** (`tests/ir_tests/NN_fuzz_<cause>.c` +
   `.expect`, registered in `tests/ir_tests/test_qemu.py`); confirm it fails
   before the fix and passes after.
3. Fix, rebuild, re-run the IR suite + a fuzz sweep; confirm zero *new*
   divergences.

Ground truth oracle is `gcc -m32 -funsigned-char` (ARM ABI: unsigned char,
32-bit long). Sweep/triage infrastructure is documented in
`docs/fuzz_triage_guide.md`.

## Don't

- Don't disable ASan/leak checks to "fix" a failure; investigate the root cause.
- Don't commit secrets, force-push, or create empty commits.

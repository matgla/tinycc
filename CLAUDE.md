# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a specialized fork of **TinyCC (Tiny C Compiler)** targeting **ARMv8-M** (Cortex-M33, Cortex-M23). It features a custom IR-based compilation pipeline for embedded ARM Thumb-2 targets.

## Build Commands

```bash
# One-time setup
./configure
make download-gcc-tests  # optional: sparse-fetch GCC torture tests (~16 MB, not the full gcc repo)

# Build ARMv8-M cross compiler
make cross

# Build everything including floating point libraries
make cross fp-libs

# Run tests
make test -j16               # IR tests (primary test suite)
make test-asm -j16           # Assembly instruction tests
make test-all                # IR + GCC torture tests
make test-gcc-torture-compile  # GCC compile-only tests

make clean                   # Clean build artifacts
```

Output binaries: `armv8m-tcc` (cross compiler), `armv8m-libtcc1.a` (runtime library).

## Running Tests

```bash
# Quick manual test for a single file
cd tests/ir_tests
python run.py -c mytest.c
python run.py -c mytest.c --cflags="-O1"
python run.py -c mytest.c --dump-ir       # dump IR
python run.py -c mytest.c --gdb           # QEMU GDB debugging

# Run specific pytest IR tests
cd tests/ir_tests && pytest -s -n auto
pytest tests/ir_tests/ -v -k "test_name"

# Run pytest for other suites
pytest tests/gcctestsuite/ -v             # GCC torture tests
pytest tests/thumb/armv8m/ -v            # assembler tests
```

## Adding Tests

- **IR tests (preferred)**: Create `tests/ir_tests/NN_test_name.c` + add to `TEST_FILES` in `tests/ir_tests/test_qemu.py`. Each `.c` file has a corresponding `.expect` file with expected output.
- **Assembly tests**: Add to `tests/thumb/armv8m/`.
- Avoid adding to `tests/tests2/` (legacy).

## Compilation Pipeline

```
C Source → Preprocessor (tccpp.c)
         → Parser + type checker (tccgen.c)
         → IR generation (tccir.h / ir/core.c)
         → IR optimizations (ir/opt.c, ir/licm.c)
         → Register allocation (tccls.c + ir/live.c)
         → Thumb-2 code gen (arm-thumb-gen.c)
         → ELF output (tccelf.c, tccld.c)
```

## Code Architecture

### Key Source Files

| File | Role |
|------|------|
| `tccgen.c` | C parser, type system, semantic analysis (largest file) |
| `arm-thumb-gen.c` | IR → Thumb-2 code generation backend |
| `tccpp.c` | C preprocessor (macros, includes, conditionals) |
| `tccelf.c` | ELF object file: sections, relocations, symbols |
| `tccls.c` | Liveness analysis + linear scan register allocator |
| `tccld.c` | Linker: symbol resolution, section merging |
| `tccdbg.c` | DWARF/STAB debug info generation |
| `libtcc.c` | Public API for using TCC as a JIT library |
| `arm-thumb-opcodes.c` | Thumb-2 opcode builders |
| `arm-thumb-asm.c` | Inline assembly parser |
| `arch/arm_aapcs.c` | ARM Procedure Call Standard (parameter passing) |

### IR Subsystem (`ir/`)

Internal IR modules — included via `ir/ir.h`, not part of public API. Public IR interface is `tccir.h`.

| File | Role |
|------|------|
| `ir/opt.c` | Main optimizations: constant folding, DCE, etc. |
| `ir/licm.c` | Loop-invariant code motion |
| `ir/core.c` | IR construction and manipulation |
| `ir/live.c` | Liveness analysis for register allocation |
| `ir/mat.c` | Value materialization (reg/memory allocation) |
| `ir/codegen.c` | Central dispatch: unified two-pass loop (dry-run + real-run) routing IR ops to backend `_mop` handlers |
| `ir/vreg.c` | Virtual register management |
| `ir/stack.c` | Stack frame layout |

IR naming conventions:
- Internal functions: `ir_<module>_<action>()` (static)
- Public API (in `tccir.h`): `tcc_ir_<action>()`

### IR Opcodes

Defined in `tccir.h` as `TccIrOp` enum. Key opcode groups:
- Arithmetic: `TCCIR_OP_ADD`, `SUB`, `MUL`, `DIV`
- Memory: `LOAD`, `STORE`, `LEA`, `LOAD_INDEXED`, `STORE_INDEXED`
- Control: `JUMP`, `JUMPIF`, `IJUMP`, `SWITCH_TABLE`
- Functions: `FUNCPARAMVAL`, `FUNCCALLVAL`, `RETURNVALUE`
- FP: `FADD`, `FSUB`, `FMUL`, `CVT_ITOF`, `CVT_FTOI`

### Register Allocation

Two-phase in `tccls.c`:
1. Liveness analysis (`ir/live.c`) — compute live ranges
2. Linear scan — assign physical registers (r0–r12), spill overflow

ARM AAPCS: r0–r3 for first 4 arguments; caller-saved r0–r3, r12, lr; callee-saved r4–r11.

## Coding Conventions

Style defined in `.clang-format`. Function body brace on new line, inner braces on same line:

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

Build uses `-std=c11 -Wunused-function -Werror`.

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

Other debug flags (not part of log.h):
```bash
make CFLAGS+='-DCONFIG_TCC_DEBUG'   # enables -dump-ir flag
```

At runtime:
```bash
./armv8m-tcc -dump-ir -c test.c     # dump IR
./armv8m-tcc -vv -c test.c          # verbose output
```

## Extending the Compiler

**New IR instruction:**
1. Add opcode to `TccIrOp` in `tccir.h`
2. Add lowering in `arm-thumb-gen.c`
3. Add test in `tests/ir_tests/`

**New assembly instruction:**
1. Add opcode builder in `arm-thumb-opcodes.c`
2. Add token in `thumb-tok.h`
3. Add parser support in `arm-thumb-asm.c`
4. Add test in `tests/thumb/armv8m/`

## Floating Point Libraries

Located in `lib/fp/`. Build variants:

```bash
cd lib/fp && make FPU=soft          # software FP (no FPU)
cd lib/fp && make FPU=vfpv4-sp      # Cortex-M4F (single-precision)
cd lib/fp && make FPU=vfpv5-dp      # Cortex-M7 (double-precision)
cd lib/fp && make FPU=rp2350        # RP2350 DCP
```

## Test Infrastructure Notes

- IR tests run via QEMU (`qemu-system-arm`) against MPS2-AN505 board model
- The first run builds newlib: `cd tests/ir_tests/qemu/mps2-an505 && sh ./build_newlib.sh`
- GCC torture tests use a git submodule at `tests/gcctestsuite/gcc-testsuite`; tests using `__builtin_*` or `_Complex` are auto-skipped
- Each tests2 test runs at both `-O0` and `-O1`

# TinyCC for ARMv8-M - Agent Guide

## Project Overview

This is a specialized fork of **TinyCC (Tiny C Compiler)** focused on **ARMv8-M architecture** support (Cortex-M33, Cortex-M23, and similar ARMv8-M microcontrollers). It features a custom Intermediate Representation (IR) and code generation pipeline optimized for embedded ARM targets.

### Key Characteristics

- **Primary Target**: ARMv8-M (Cortex-M33) with Thumb-2 instruction set
- **Architecture**: IR-based compilation with separate front-end and back-end
- **Floating Point**: Multiple FP options (software, VFPv4-sp, VFPv5-dp, RP2350 DCP)
- **Library**: Can be used as `libtcc.a` library for JIT compilation
- **License**: GNU Lesser General Public License (LGPL)

## Project Structure

```
.
├── Core Compiler Sources
│   ├── tcc.c              # Main driver/CLI entry point
│   ├── tccpp.c            # C preprocessor
│   ├── tccgen.c           # C parser and type system
│   ├── tccir.c            # Intermediate Representation (IR) generator
│   ├── tccir.h            # IR definitions and opcodes
│   ├── tccir_operand.c    # IR operand handling
│   ├── tccir_operand.h    # IR operand definitions
│   ├── tccls.c            # Liveness analysis and register allocation
│   ├── tccld.c            # Linker
│   ├── tccelf.c           # ELF file format support
│   ├── tccasm.c           # Inline assembler
│   ├── tccdbg.c           # Debug info generation
│   ├── tccdebug.c         # Debug utilities
│   ├── libtcc.c           # Library API implementation
│   └── tccyaff.c          # YAFF (Yet Another File Format) support
│
├── ARM-Specific Sources
│   ├── arm-thumb-gen.c    # ARM Thumb-2 code generator (from IR)
│   ├── arm-thumb-opcodes.c# Thumb-2 opcode builders
│   ├── arm-thumb-opcodes.h# Thumb-2 instruction definitions
│   ├── arm-thumb-asm.c    # ARM assembler parser
│   ├── arm-thumb-callsite.c# Call site handling for ARM
│   ├── arm-thumb-defs.h   # ARM-specific definitions
│   ├── arm-link.c         # ARM linker support
│   ├── arch/armv8m.c      # ARMv8-M architecture configuration
│   └── arch/arm_aapcs.c   # ARM Procedure Call Standard support
│
├── Headers
│   ├── tcc.h              # Main compiler header
│   ├── libtcc.h           # Public library API
│   ├── tcctok.h           # Token definitions
│   ├── tccld.h            # Linker interface
│   ├── tccls.h            # Liveness analysis interface
│   ├── tccabi.h           # ABI definitions
│   ├── thumb-tok.h        # ARM Thumb token definitions
│   └── svalue.h           # Stack value definitions
│
├── Libraries
│   ├── lib/               # Runtime library sources (libtcc1.a)
│   │   ├── libtcc1.c      # Core runtime functions
│   │   ├── armeabi.c      # ARM EABI helper functions
│   │   ├── armv8m_eabi.c  # ARMv8-M EABI specific
│   │   └── fp/            # Floating point libraries
│   │       ├── soft/      # Software FP implementation
│   │       ├── arm/vfpv4-sp/  # VFPv4 single-precision
│   │       ├── arm/vfpv5-dp/  # VFPv5 double-precision
│   │       └── arm/rp2350/    # RP2350 DCP support
│   └── include/           # System headers (tcclib.h, stddef.h, etc.)
│
├── Tests
│   ├── tests/ir_tests/    # IR-level tests (pytest-based)
│   ├── tests/thumb/armv8m/# Assembly instruction tests
│   ├── tests/tests2/      # C language compliance tests
│   ├── tests/pp/          # Preprocessor tests
│   └── tests/benchmarks/  # Performance benchmarks
│
├── Build System
│   ├── configure          # Configuration script (POSIX shell)
│   ├── Makefile           # Main build rules
│   ├── config.mak         # Generated configuration
│   └── config.h           # Generated C headers
│
└── Documentation
    ├── tcc-doc.texi       # Texinfo documentation source
    ├── LAZY_SECTION_LOADING.md    # Lazy loading design doc
    └── asm_port.md        # Assembler porting notes
```

## Build System

### Prerequisites

- GCC or Clang compiler
- GNU Make
- Python 3 with virtualenv (for tests)
- `arm-none-eabi-gcc` (for ARMv8-M cross-compilation)

### Configure Options

```bash
./configure [options]
  --prefix=PREFIX          # Installation prefix [/usr/local]
  --enable-cross           # Build cross compilers
  --debug                  # Include debug info
  --enable-asan            # Enable AddressSanitizer
  --disable-static         # Build shared library (libtcc.so)
```

### Build Commands

```bash
# Configure for native build (x86_64)
./configure

# Build ARMv8-M cross compiler
make cross

# Build everything including fp-libs
make cross fp-libs

# Run tests
make test

# Clean build artifacts
make clean

# Install (default: /usr/local)
make install
```

### Output Files

- `armv8m-tcc` - ARMv8-M cross compiler executable
- `armv8m-libtcc1.a` - Runtime library for ARMv8-M
- `libtcc1-fp-*.a` - Floating point libraries for different FPU configs
- `libtcc.a` or `libtcc.so` - Library version of compiler

## Testing

### Test Structure

The project uses multiple testing frameworks:

1. **IR Tests** (`tests/ir_tests/`): pytest-based functional tests
   - Test C code compilation to IR and execution via QEMU
   - Requirements: `pytest`, `pytest-xdist`, `pexpect`
   - Tests are numbered: `01_hello_world.c`, `20_op_add.c`, etc.
   - Each `.c` file has a corresponding `.expect` file with expected output

2. **Assembly Tests** (`tests/thumb/armv8m/`): pytest-based assembler tests
   - Test individual Thumb-2 instructions
   - Compares TCC output against `arm-none-eabi-gcc`

3. **Legacy Tests** (`tests/tests2/`, `tests/pp/`): Makefile-based tests
   - C language compliance tests
   - Preprocessor tests

### Running Tests

```bash
# Full test suite (requires ARM cross toolchain)
make test

# Run only IR tests
make test-venv test-prepare
cd tests/ir_tests && pytest -s -n auto

# Run only assembly tests
make test-asm

# Run legacy tests
make test-legacy

# Run AEABI host tests
make test-aeabi-host
```

### Test Requirements for IR Tests

The first run will build newlib for the ARM target:
```bash
cd tests/ir_tests/qemu/mps2-an505 && sh ./build_newlib.sh
```

This creates `newlib_build/arm-none-eabi/newlib/libc.a` needed for linking.

## Code Architecture

### Compilation Pipeline

```
C Source (.c)
    ↓
Preprocessor (tccpp.c) - macro expansion, includes
    ↓
Parser (tccgen.c) - semantic analysis, type checking
    ↓
IR Generation (tccir.c) - platform-independent IR
    ↓
IR Optimization - constant folding, dead code elimination
    ↓
Register Allocation (tccls.c) - liveness analysis, register assignment
    ↓
Code Generation (arm-thumb-gen.c) - Thumb-2 machine code
    ↓
ELF Output (tccelf.c) - relocations, sections, symbols
```

### IR (Intermediate Representation)

The IR is a three-address code representation with:

- **Operations**: `TCCIR_OP_ADD`, `TCCIR_OP_LOAD`, `TCCIR_OP_FUNCCALLVAL`, etc.
- **Operands**: Registers, immediates, memory references, symbols
- **Types**: `IR_TYPE_S32`, `IR_TYPE_F32`, `IR_TYPE_F64`, etc.

Key files:
- `tccir.h` - IR opcodes and structures
- `tccir_operand.h` - Operand types and accessors
- `tccir.c` - IR generation from AST
- `arm-thumb-gen.c` - IR to Thumb-2 code generation

### Register Allocation

Two-phase register allocation in `tccls.c`:

1. **Liveness Analysis**: Compute live ranges for virtual registers
2. **Register Allocation**: Assign physical registers using linear scan

Architecture configuration in `arch/armv8m.c`:
```c
ArchitectureConfig architecture_config = {
    .pointer_size = 4,
    .stack_align = 8,
    .reg_size = 4,
    .parameter_registers = 4,  // r0-r3 for arguments
    .has_fpu = 0,
};
```

## Coding Conventions

### Style Guidelines

- **C Standard**: C11 (`-std=c11`)
- **Indentation**: 2 spaces (no tabs)
- **Line Length**: ~100 characters
- **Braces**: K&R style, opening brace on same line

Example:
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

### Compiler Warnings

The build uses strict warnings:
```makefile
CFLAGS += -std=c11 -Wunused-function -Wno-declaration-after-statement -Werror
```

### Debug Macros

Enable debug output with build flags:
```bash
make CFLAGS+='-DPARSE_DEBUG'      # Parser debug
make CFLAGS+='-DPP_DEBUG'          # Preprocessor debug
make CFLAGS+='-DASM_DEBUG'         # Assembler debug
make CFLAGS+='-DCONFIG_TCC_DEBUG'  # IR dump (-dump-ir)
```

## Floating Point Support

The compiler supports multiple FP configurations via `lib/fp/`:

| FPU Type | Library | Description |
|----------|---------|-------------|
| Software | `libtcc1-fp-soft-armv8m.a` | Pure C soft-float (no FPU) |
| VFPv4-sp | `libtcc1-fp-vfpv4-sp-armv8m.a` | Cortex-M4F (single-precision) |
| VFPv5-dp | `libtcc1-fp-vfpv5-dp-armv8m.a` | Cortex-M7 (double-precision) |
| RP2350 | `libtcc1-fp-rp2350-armv8m.a` | RP2350 double coprocessor |

Build specific FP library:
```bash
cd lib/fp && make FPU=vfpv4-sp
```

## Key Development Notes

### Adding a New IR Instruction

1. Add opcode to `TccIrOp` enum in `tccir.h`
2. Add lowering logic in `arm-thumb-gen.c`
3. Add test case in `tests/ir_tests/`

### Adding Assembly Instructions

1. Add opcode builder in `arm-thumb-opcodes.c`
2. Add token definition in `thumb-tok.h`
3. Add parser support in `arm-thumb-asm.c`
4. Add test case in `tests/thumb/armv8m/`

### Important Limitations

- This fork is specifically tailored for ARMv8-M (Cortex-M33)
- Native compilation on x86_64 is not the primary use case
- Some standard C features may be incomplete (check test suite)

## Library API (libtcc)

The compiler can be used as a library for JIT compilation:

```c
#include <libtcc.h>

TCCState *s = tcc_new();
tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
tcc_compile_string(s, "int square(int x) { return x*x; }");
tcc_relocate(s);
int (*square)(int) = tcc_get_symbol(s, "square");
int result = square(5);
tcc_delete(s);
```

See `libtcc.h` for full API and `tests/libtcc_test.c` for examples.

## Security Considerations

- The compiler processes untrusted C code; input validation is essential
- Buffer bounds are checked in most places but fuzzing is recommended
- The `-b` option enables runtime bounds checking (when available)
- Stack protector support varies by target

## Troubleshooting

### Common Build Issues

1. **Missing `config.mak`**: Run `./configure` first
2. **Missing `arm-none-eabi-gcc`**: Install ARM GNU toolchain
3. **Tests fail with QEMU errors**: Ensure qemu-arm is installed

### Debug Techniques

```bash
# Dump IR for a file
./armv8m-tcc -dump-ir -c test.c

# Show verbose output
./armv8m-tcc -vv -c test.c

# Enable bounds checking
./armv8m-tcc -b -run test.c
```

## Related Documentation

- `README` - Original TinyCC README
- `LAZY_SECTION_LOADING.md` - Design for lazy section loading
- `asm_port.md` - Assembler porting notes
- `lib/fp/README.md` - Floating point library documentation
- `tcc-doc.html` - Full documentation (requires `makeinfo`)

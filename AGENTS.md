# AGENTS.md

Guidance for autonomous coding agents working in this repository (TinyCC fork
targeting ARMv8-M). Read this first, then `CLAUDE.md` for the full project
overview, build commands, and architecture.

## Project Overview

This is a specialized fork of **TinyCC (Tiny C Compiler)** targeting **ARMv8-M**
(Cortex-M33, Cortex-M23). It features a custom IR-based compilation pipeline
for embedded ARM Thumb-2 targets.

Output binaries: `armv8m-tcc` (cross compiler), `armv8m-libtcc1.a` (runtime
library).

## Build Commands

```bash
# One-time setup
./configure              # AddressSanitizer is ON by default; ./configure --disable-asan for fast/production builds
make download-gcc-tests  # optional: sparse-fetch GCC torture tests (~16 MB, not the full gcc repo)

# Build ARMv8-M cross compiler
make cross
make cross fp-libs       # everything including floating point libraries

# Run tests
make test -j16               # IR tests (primary test suite)
make test-asm -j16           # Assembly instruction tests
make test-all                # IR + GCC torture tests
make test-gcc-torture-compile  # GCC compile-only tests

make clean                   # Clean build artifacts
```

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
pytest tests/thumb/armv8m/ -v             # assembler tests
```

## Adding Tests

- **IR tests (preferred):** Create `tests/ir_tests/NN_test_name.c` + add to
  `TEST_FILES` in `tests/ir_tests/test_qemu.py`. Each `.c` file has a
  corresponding `.expect` file with expected output.
- **Assembly tests:** Add to `tests/thumb/armv8m/`.
- Avoid adding to `tests/tests2/` (legacy).

## Compilation Pipeline

```
C Source → Preprocessor (tccpp.c)
         → Parser + type checker (tccgen.c)
         → IR generation (tccir.h / ir/core.c)
         → IR optimizations (ir/opt.c, ir/licm.c)
         → Register allocation (tcls.c + ir/live.c)
         → Thumb-2 code gen (arm-thumb-gen.c)
         → ELF output (tccelf.c, tccld.c)
```

## Code Architecture

### IR Subsystem (`ir/`)

Internal IR modules — included via `ir/ir.h`, not part of public API. Public
IR interface is `tccir.h`.

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

ARM AAPCS: r0–r3 for first 4 arguments; caller-saved r0–r3, r12, lr;
callee-saved r4–r11.

## Build & Test (always current)

```bash
make cross -j$(nproc)                       # build armv8m-tcc (rebuild after EVERY edit)
make test -j16                              # runs all available tests including frontend tests
python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu   # fuzz self-consistency
```

### Test Targets

- `test-frontend` — frontend golden-IR tests (preprocessor / type-system / diagnostics, QEMU-free)
- `test-asm` — assembly instruction tests
- `test-ir` — IR test suite (primary gate)
- `test-sequential` — fully sequential test run (no pytest-xdist)
- `test-golden-ir` — golden IR snapshot tests (requires CONFIG_TCC_DEBUG build)
- `test-legacy` — legacy tests2 tests
- `test-gcc-torture-compile` — GCC compile-only tests
- `test-gcc-torture` — GCC torture tests (compile + execute)
- `test-full` — full test suite (compile-only variant)
- `test-all` — full test suite (execute variant)

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

Use `LOG_<SCOPE>(fmt, ...)` macros in code. Output goes to stderr with
`[SCOPE]` prefix.

Other debug flags (not part of log.h):

```bash
make CFLAGS+='-DCONFIG_TCC_DEBUG'   # enables -dump-ir flag
```

At runtime:

```bash
./armv8m-tcc -dump-ir -c test.c     # dump IR
./armv8m-tcc -vv -c test.c          # verbose output
```

## Debugging an Optimizer Miscompilation

When a fuzz seed diverges between O-levels (`tcc -O0` correct, `-O1`/`-O2` wrong),
follow **`docs/debugging_fuzz_divergences.md`** end-to-end:

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

## Coding Guidelines

All coding conventions (style, file headers, comments, memory ownership,
change conventions) are consolidated in
[`docs/coding_guidelines.md`](docs/coding_guidelines.md). Read that file for
the full reference.

## Don't

- Don't disable ASan/leak checks to "fix" a failure; investigate the root cause.
- Don't commit secrets, force-push, or create empty commits.
- Don't commit the temporary `TCC_SKIP_SSA*` env-var bisection gates (see the
  triage guide); they are investigation-only scaffolding.

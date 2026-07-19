# TinyCC ARMv8-M — Copilot Instructions

## Project Overview

Specialized TinyCC fork targeting **ARMv8-M** (Cortex-M33) Thumb-2. IR-based pipeline: C source → preprocessor (`tccpp.c`) → parser (`tccgen.c`) → IR (`tccir.h`, `ir/core.c`) → optimizations (`ir/opt.c`, `source/opt/flat/loop/licm.c`) → register allocation (`tccls.c`, `ir/live.c`) → Thumb-2 codegen (`arm-thumb-gen.c`) → ELF (`tccelf.c`).

## Build & Test

```bash
./configure && make cross          # build armv8m-tcc cross compiler
make test -j16                     # primary IR test suite (pytest + QEMU)
make test-asm -j16                 # assembly instruction tests
make test-all                      # IR + GCC torture tests
```

Single-file testing: `cd tests/ir_tests && python run.py -c mytest.c` (add `--dump-ir`, `--cflags="-O1"`, `--gdb` as needed).

## Key Source Files

| File | Role |
|---|---|
| `tccgen.c` | C parser + type system (largest file) |
| `arm-thumb-gen.c` | IR → Thumb-2 backend (~12k lines) |
| `ir/codegen.c` | Central dispatch: routes IR ops to backend handlers |
| `ir/machine_op.h` | `MachineOperand` type (8 kinds: REG, SPILL, IMM, FRAME_ADDR, SYMBOL, PARAM_STACK, CHAIN_REL, NONE) |
| `ir/machine_op.c` | `machine_op_from_ir()` — converts IROperand to MachineOperand |
| `tccls.c` | Linear-scan register allocator |
| `arm-thumb-callsite.c` | AAPCS call-site layout builder |
| `arch/arm_aapcs.c` | ARM procedure call standard |

## Architecture Patterns

### Backend Handler Naming

Backend functions follow a dual naming convention during the ongoing materialization refactor:
- `tcc_gen_machine_<op>_mop(MachineOperand ...)` — **new** MachineOperand-based handlers (preferred)
- `tcc_gen_machine_<op>_op(IROperand ...)` — **legacy** IROperand-based handlers (being removed)

All backend handler declarations live in `tcc.h` (~line 2114+). New code should use `_mop` variants exclusively.

### IR Dispatch (ir/codegen.c)

The codegen uses a single unified two-pass loop (`for (pass = 0; pass < 2; pass++)`):
- **Pass 0 (dry-run)**: discovers scratch register needs, collects branch offsets — `ot()` is a no-op; records per-instruction scratch counts.
- **Inter-pass**: analyzes branch encodings, checks LR usage, runs scratch conflict fixup, emits prologue.
- **Pass 1 (real-run)**: emits actual Thumb-2 machine code using dry-run data for consistency checks.

Both passes share a single `switch (cq->op)` dispatch. Pass-specific behavior (e.g. SWITCH_TABLE sizing, RETURNVOID epilogue jump, INLINE_ASM) uses `if (is_dry_run)` / `if (!is_dry_run)` guards. Adding a new IR op requires adding only one `case`.

### IR Subsystem (`ir/`)

Internal modules included via `ir/ir.h` (which pulls in `tcc.h` first). Naming:
- Static/internal: `ir_<module>_<action>()`
- Public API (in `tccir.h`): `tcc_ir_<action>()`

IR opcodes defined as `TccIrOp` enum in `tccir.h`. Key groups: arithmetic (`ADD`, `SUB`, `MUL`), memory (`LOAD`, `STORE`, `LEA`), control (`JUMP`, `JUMPIF`), function (`FUNCPARAMVAL`, `FUNCCALLVAL`, `RETURNVALUE`), FP (`FADD`, `FSUB`, `CVT_ITOF`).

## Coding Conventions

- `.clang-format`: LLVM-based, 120-col, Allman braces (`BreakBeforeBraces: Allman`)
- Build enforces `-std=c11 -Wunused-function -Werror`
- 2-space indentation inside function bodies; function-level braces on new line, inner braces on same line

## Adding New Functionality

**New IR instruction:** add opcode to `TccIrOp` in `tccir.h` → add lowering in `arm-thumb-gen.c` → add test in `tests/ir_tests/`

**New assembly instruction:** add builder in `arm-thumb-opcodes.c` → token in `thumb-tok.h` → parser in `arm-thumb-asm.c` → test in `tests/thumb/armv8m/`

**New IR test:** create `tests/ir_tests/NN_name.c` + `.expect` file → add entry to `TEST_FILES` in `tests/ir_tests/test_qemu.py`. Avoid adding to `tests/tests2/` (legacy).

## Debug Flags

```bash
make cross CFLAGS+='-DCONFIG_TCC_DEBUG'   # enables -dump-ir at runtime
make cross CFLAGS+='-DTCC_LS_DEBUG'       # register allocator tracing
make cross CFLAGS+='-DPARSE_DEBUG'        # parser debug output
```

Runtime: `./armv8m-tcc -dump-ir -c test.c` or `./armv8m-tcc -vv -c test.c`.

## Test Infrastructure

- IR tests run via QEMU (`qemu-system-arm`, MPS2-AN505 board) with semihosting
- First run requires building newlib: `cd tests/ir_tests/qemu/mps2-an505 && sh ./build_newlib.sh`
- GCC torture tests use submodule: `git submodule update --init --depth 1 tests/gcctestsuite/gcc-testsuite`
- ARM register convention (AAPCS): r0–r3 args/caller-saved, r4–r11 callee-saved, r12+lr caller-saved

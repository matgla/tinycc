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
C Source → Preprocessor (source/frontend/tccpp.c)
         → Parser + type checker (source/frontend/gen/)
         → IR generation (source/ir/tccir.h, source/ir/gen/)
         → IR optimizations (source/opt/)
         → Register allocation (source/machine/tccls.c + source/ir/regalloc.c)
         → Thumb-2 code gen (source/backend/arch/arm/thumb/arm-thumb-gen.c)
         → ELF output (source/obj/tccelf.c, source/obj/tccld.c)
```

## Source Tree

All compiler code lives under `source/`, one directory per module. Each module
owns its headers, carries its own `Makefile`, and builds to its own static
library; the top-level `Makefile` includes those module Makefiles and links the
libraries into `armv8m-tcc`. The repo root holds only build inputs
(`configure`, `config.mak`, `conftest.c`), generated headers (`tccdefs_.h`,
`tccdecls_.h`), shipped headers (`include/`), and project metadata.

| Module | Library | Contents |
|--------|---------|----------|
| `source/include/` | — | `tcc.h` (the umbrella header) and `tcctypes.h` |
| `source/driver/` | `libtccdriver.a` | `tcc.c` (CLI main), `tcctools.c` (ar/tool dispatch, `#include`d by `tcc.c`), `libtcc.c` (TCCState, options, compile/link driver), `libtcc.h` |
| `source/frontend/` | `libfrontend.a` | `tccpp.c` (preprocessor/tokenizer), `tccasm.c` (GAS asm frontend), `svalue.c`, `tcctok.h`, and `gen/` — the parser / type checker / IR emission |
| `source/ir/` | `libir.a` | Target-independent IR: pool, CFG, SSA, codegen dispatch, SSA regalloc, `tccir_operand.c`, and `gen/` (instruction emission) |
| `source/opt/` | `libopt.a` | The whole optimizer: `include/`, `util/`, `analysis/`, `engine/`, `flat/`, `ssa/`, `ra/`, `framework/` |
| `source/machine/` | `libmachine.a` | The generic backend boundary: `tccls.c` (linear scan), `tccmachine.c`, `tcc_target.h`, `tccabi.h` |
| `source/obj/` | `libobj.a` | `tccelf.c`, `tccld.c` (linker scripts), `tccyaff.c` (YAFF flat format), `tccdbg.c` (DWARF/STABS), `elf.h`, `dwarf.h`, `stab.h` |
| `source/support/` | `libsupport.a` | `tccdebug.c` (SValue/Sym printers for gdb), `log.h`, `tcc-chained-hash.h`, `tccdbgenv.h` |
| `source/memory/` | `libmemory.a` | `vector.c`, `unique_ptr.c` and the container headers |
| `source/utils/` | — | Header-only helpers (`defer.h`) |
| `source/backend/` | `libarm.a`, `libgenerators.a` | `arch/arm/` (+ `thumb/`, `fpu/`) and the generic `generators/` |

The module libraries are linked `--whole-archive`. That is load-bearing, not
cosmetic: several TUs are referenced by nothing (the gdb pretty-printers, opt
passes not yet wired into the pipeline table), and an ordinary archive link
drops them silently — 53 symbols and 67 KB of `.text` on the first attempt.
`ARCH_LIB` deliberately stays outside the group so its orphaned members keep
being dropped as before.

Adding a module: create `source/<name>/` with a `Makefile` defining
`<NAME>_INC` / `<NAME>_SRC` / `<NAME>_HDRS`, an object rule, and a
`$(<NAME>_LIB): $(<NAME>_OBJ)` rule whose recipe is `$(ar-lib)`. Then add the
`include` line and the `_INC` / `_LIB` / `_SRC` references to the top-level
`Makefile`. Include order matters — see the comment above the include block.

## Code Architecture

### IR Subsystem (`source/ir/`)

Internal IR modules — included via `source/ir/ir.h`, not part of public API.
Public IR interface is `source/ir/tccir.h`.

| File | Role |
|------|------|
| `source/ir/gen/` | IR construction and manipulation (the former `ir/core.c`) |
| `source/ir/gen/live.c` | Liveness analysis for register allocation |
| `source/ir/codegen.c` | Central dispatch: unified two-pass loop (dry-run + real-run) routing IR ops to backend `_mop` handlers |
| `source/ir/regalloc.c` | SSA register allocator, parameterized by `RegAllocTarget` |
| `source/ir/vreg.c` | Virtual register management |
| `source/ir/stack.c` | Stack frame layout |

IR naming conventions:
- Internal functions: `ir_<module>_<action>()` (static)
- Public API (in `source/ir/tccir.h`): `tcc_ir_<action>()`

### IR Opcodes

Defined in `tccir.h` as `TccIrOp` enum. Key opcode groups:
- Arithmetic: `TCCIR_OP_ADD`, `SUB`, `MUL`, `DIV`
- Memory: `LOAD`, `STORE`, `LEA`, `LOAD_INDEXED`, `STORE_INDEXED`
- Control: `JUMP`, `JUMPIF`, `IJUMP`, `SWITCH_TABLE`
- Functions: `FUNCPARAMVAL`, `FUNCCALLVAL`, `RETURNVALUE`
- FP: `FADD`, `FSUB`, `FMUL`, `CVT_ITOF`, `CVT_FTOI`

### Register Allocation

Two-phase in `source/machine/tccls.c`:
1. Liveness analysis (`source/ir/gen/live.c`) — compute live ranges
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
make CFLAGS+='-DTCC_LOG_CODEGEN=1'      # frontend code generation (source/frontend/gen/)
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

### Debug env knobs (`tccdbgenv.h`)

The ~30 `getenv`-driven knobs — pass-disable switches for bisecting a miscompile
(`TCC_DISABLE_PASS`, `TCC_NO_COALESCE`, …), A/B levers an optimization decision
was priced with (`TCC_NO_REHEARSAL`, `TCC_KEEP_FWD_DRY`, …) and pure traces
(`DBG_CLINL`, `DUMP_IR_CG`, `SCAN_OVERLAP`, …) — go through **`tccdbgenv.h`**.
**Never call `getenv` directly from compiler code**: a raw call is unlatched (so
it re-walks `environ` inside a per-pass loop) and it survives into the release
binary. Declare the knob instead, next to the code it gates:

```c
TCC_DBG_ENV_FLAG(cg_no_rehearsal, "TCC_NO_REHEARSAL")   /* set / not set  */
TCC_DBG_ENV_STR (pass_disable_list, "TCC_DISABLE_PASS") /* value or NULL  */
TCC_DBG_ENV_INT (ra_coalesce_env_level, "TCC_COALESCE", 2) /* int + default */

TCC_DBG_TRACE(cg_no_rehearsal, (stderr, "skipping: %s\n", name)); /* fprintf */
TCC_DBG_BLOCK(cg_dump_ir_cg) { tcc_ir_show(ir); }                 /* block   */
```

Each expands to a latched `static inline` accessor. `CONFIG_TCC_DEBUG_ENV=0`
turns them into compile-time constants (`0` / `NULL` / the default), so the
guarded code is dead and the lookups are gone; `TCC_DBG_TRACE` bodies vanish in
the preprocessor, so they go at any `-O` level. Two symbols with no natural home
in a single TU — `dbg_scan_overlap` / `dbg_scan_imm_dest` and
`tcc_ir_opt_pass_disabled` — are declared in `tccir.h` so the release no-op
reaches every call site and the *calls* disappear too, not just the bodies.

Build settings:

| build | knobs |
|---|---|
| `make cross` (default, `CONFIG_TCC_DEBUG` on) | **on** — the host bisect tooling works unchanged |
| `make cross CONFIG_debugenv=no` | off |
| `make cross CONFIG_minimal=yes` | off (no `CONFIG_TCC_DEBUG`) |
| `build_rootfs.sh` cross stage | on |
| `build_rootfs.sh` native/device stage | **off** |
| `build_rootfs.sh --debug-tcc` | on everywhere |

Measured on the armv8m device compiler: `.text` 1,451,584 → 1,437,000
(−14,584 B), `.rodata` −2,360 B, file −17,272 B; `getenv` calls 27.1 → 3.0 per
compile; output byte-identical over 1,545 corpus compiles (`tests/ir_tests`,
−O0/−O1/−O2).

**Consequence:** on a release device compiler `TCC_PASS_TIMING=1` (e.g. via
`YASOS_TCC_ENV_PREFIX`) no longer produces the PASS_TIME table — use `-bench`,
which drives the same instrumentation through `tcc_state->do_bench`.

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

To decide whether the seeds a sweep flagged are *new*, do not re-sweep the band:
`python3 tests/fuzz/batch_sweep.py --profile P --seeds "1,1410,…"
--olevels="-O0,-O1,-O2,gcc-O0" --no-cache` re-checks an explicit list in about a
second, so the same list can be run against HEAD (or with the change's kill
switch) for a real A/B. See "Re-checking only the seeds that failed" in
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

## Developer Scripts (`scripts/`)

- `scripts/*.py` are the runnable entry points; invoke them directly. The
  directory is Python-only — don't add shell scripts.
- `scripts/sources/` holds the importable modules behind them — `disasm_common.py`
  (compile/disassemble/count + the best-known-result cache) and `fuzz_common.py`
  (puts `tests/fuzz` on `sys.path` and re-exports the harness). Import them as
  `from sources.disasm_common import ...`; runners resolve the package because
  Python puts the runner's own directory on `sys.path`.
- `regression_disasm.py` baselines are read from and written to
  `metrics/baselines/`; `--diff p1_baseline` looks there.

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

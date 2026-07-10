# TinyCC Unit-Test Framework Guide

> **Process & conventions live in [`docs/writing_unit_tests.md`](../../docs/writing_unit_tests.md)** —
> read it first. In particular: never modify production source while writing tests
> (it breaks parallel tasks), and report every suspected bug in `docs/bugs.md`.
> This README documents the same framework from the build-mechanics angle.

## Overview

The `tests/unit/` directory contains **host-native C unit tests** for tinycc internal modules. The goal is to test data structures, algorithms, and utility functions in isolation — without pulling in the full compiler, backend code generators, or QEMU.

### Why Host-Native?

- **Fast feedback loop**: Compile with `gcc` and run directly on the build machine.
- **No cross-compilation or emulation**: No need for `arm-none-eabi-gcc` or QEMU.
- **Target fidelity**: The preprocessor defines (`-DTCC_TARGET_ARM`, etc.) mirror the armv8m cross build so `tcc.h` parses identically.

### Key Design Principle: Stub What You Don't Test

Unit tests link **only** the specific source files they exercise, plus minimal stubs for dependencies (memory allocators, global state). This avoids dragging in `ir/gen/*.c`, `tccls.c`, `arm-thumb-gen.c`, and other heavy modules.

---

## Framework API (`tests/unit/ut.h`)

The harness is a single 99-line header. No external libraries.

### Macros

| Macro | Purpose |
|-------|---------|
| `UT_TEST(name)` | Declare a test function (`static int name(void)`). |
| `UT_ASSERT(cond)` | Assert a boolean condition. On failure prints file:line and returns `-1`. |
| `UT_ASSERT_EQ(a, b)` | Assert equality (cast to `long long`). Prints both values on failure. |
| `UT_SUITE(name)` | Declare a suite function (`void ut_suite_##name(void)`). |
| `UT_RUN(test)` | Execute a single test inside a suite. |
| `UT_RUN_SUITE(name)` | Execute a suite from `main()`. |
| `UT_DECLARE_SUITE(name)` | Forward-declare a suite (used in `test_main.c`). |
| `UT_MAIN_IMPL` | Define the shared counters (exactly **one** TU must use this). |
| `UT_REPORT_AND_EXIT()` | Print summary and return `0` on success, `1` on failure. |

### Return Convention

Tests return `0` on success, `-1` on failure. The harness tracks `ut_fail_count` globally, so an early `UT_ASSERT` failure aborts the test but other suites still run.

---

## Directory Layout

```
tests/unit/
├── ut.h                          # Single-header harness
├── Makefile                      # Orchestrator (fans out to per-target dirs)
└── arm/armv8m/                   # Per-target directory
    ├── Makefile                  # Builds run_unit_tests binary
    ├── test_main.c               # Entry point: declares and runs all suites
    ├── stubs.c                   # Memory allocator stubs (tcc_malloc, tcc_free, ...)
    ├── tcc_state_stub.c          # Global TCCState pointer stub
    ├── test_chained_hash.c       # Example: suite for tcc-chained-hash.h
    ├── test_ir_pool.c            # Example: suite for ir/pool.c
    ├── test_ir_type.c            # Example: suite for ir/type.c
    └── test_ir_vreg.c            # Example: suite for ir/vreg.c
```

---

## How to Add a New Test Suite

Follow these steps precisely. They are designed to be agent-friendly and minimize boilerplate.

### Step 1: Identify the Module Under Test

Determine which tinycc source file(s) you are testing. Examples:

| Suite | Module Under Test |
|-------|-------------------|
| `ir_pool` | `ir/pool.c` |
| `ir_type` | `ir/type.c` |
| `ir_vreg` | `ir/vreg.c` |
| `chained_hash` | `tcc-chained-hash.h` (header-only) |

### Step 2: Create the Test File

Create `tests/unit/arm/armv8m/test_<module>.c`. Use this template:

```c
/*
 *  test_<module>.c - suite for <path/to/module>.c
 *
 *  <One-line description of what is tested.>
 */

#define USING_GLOBALS
#include "ir.h"          /* or tcc.h, or whatever the module needs */

#include "ut.h"

/* ------------------------------------------------------------------ helpers */

/* Optional: minimal setup/teardown helpers that avoid full tcc_init(). */

/* ------------------------------------------------------------------ tests */

UT_TEST(test_feature_basic)
{
  /* Arrange */
  int x = 42;

  /* Act / Assert */
  UT_ASSERT_EQ(x, 42);
  UT_ASSERT(x > 0);

  return 0;
}

UT_TEST(test_feature_edge_case)
{
  /* ... */
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(<module>)
{
  UT_RUN(test_feature_basic);
  UT_RUN(test_feature_edge_case);
}
```

**Guidelines:**

- Start with `#define USING_GLOBALS` if the module uses the `tcc_state` global or other globals.
- Include **only** the headers the module under test needs. Do not include the whole compiler front-end.
- If the module needs a `TCCIRState` or `TCCState`, write a minimal helper that `malloc`s a zeroed struct and manually initializes **only** the fields the module touches. See `test_ir_vreg.c` for a detailed example.
- Tests must be **deterministic** and **self-contained** — no file I/O, no network, no randomness.

### Step 3: Register the Suite in `test_main.c`

Edit `tests/unit/arm/armv8m/test_main.c`. Add two lines:

1. `UT_DECLARE_SUITE(<module>);` near the top.
2. `UT_RUN_SUITE(<module>);` inside `main()`.

Example:

```c
#include "ut.h"

UT_MAIN_IMPL;

UT_DECLARE_SUITE(chained_hash);
UT_DECLARE_SUITE(ir_pool);
UT_DECLARE_SUITE(ir_type);
UT_DECLARE_SUITE(ir_vreg);
UT_DECLARE_SUITE(my_new_module);   /* <-- ADD THIS */

int main(void)
{
  UT_RUN_SUITE(chained_hash);
  UT_RUN_SUITE(ir_pool);
  UT_RUN_SUITE(ir_type);
  UT_RUN_SUITE(ir_vreg);
  UT_RUN_SUITE(my_new_module);     /* <-- ADD THIS */
  UT_REPORT_AND_EXIT();
}
```

### Step 4: Add the Source File to the Makefile

Edit `tests/unit/arm/armv8m/Makefile` in **two** places:

1. **Add the test file to `UT_LOCAL_SRCS`:**

```makefile
UT_LOCAL_SRCS := \
    test_main.c \
    test_chained_hash.c \
    test_ir_pool.c \
    test_ir_type.c \
    test_ir_vreg.c \
    test_my_new_module.c \   # <-- ADD THIS
    stubs.c \
    tcc_state_stub.c
```

2. **Add the module under test to `UT_MODULE_SRCS` (if it is a `.c` file from the tinycc tree):**

```makefile
UT_MODULE_SRCS := \
    $(TOP)/ir/pool.c \
    $(TOP)/ir/type.c \
    $(TOP)/ir/vreg.c \
    $(TOP)/ir/my_new.c         # <-- ADD THIS
```

If the module is **header-only** (like `tcc-chained-hash.h`), skip step 2.

### Step 5: Build and Run

```bash
# From project root
make ut

# Or directly
cd tests/unit/arm/armv8m && make run

# Clean rebuild
make ut-clean && make ut
```

Expected output on success:

```
== suite my_new_module ==
    ok   test_feature_basic
    ok   test_feature_edge_case

42 tests, 87 asserts, 0 failed tests, 0 failed asserts
```

---

## Stub System

Because unit tests do not link the full `libtcc`, several symbols must be provided by stubs.

### `stubs.c` — Memory Allocators

Provides `tcc_malloc`, `tcc_mallocz`, `tcc_realloc`, `tcc_free`, `tcc_strdup` using raw libc calls. **This TU must NOT include `tcc.h`** because `tcc.h` redefines `malloc`/`free`.

If your module under test calls **other** tinycc helpers (e.g., `tcc_error`, `tcc_warning`, `dynarray_add`), add stub implementations here. Keep them minimal:

```c
void tcc_error(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  exit(1);
}
```

### `tcc_state_stub.c` — Global State

Provides the `TCCState *tcc_state` symbol. Some functions read fields like `tcc_state->float_abi`. Tests that care about specific values must write them before calling the function under test:

```c
tcc_state->float_abi = ARM_FLOAT_ABI_HARD;
```

---

## Handling Dependencies

When adding a new module, you may encounter linker errors for missing symbols. Resolve them using this priority:

1. **If the symbol is a memory allocator or trivial helper** → add to `stubs.c`.
2. **If the symbol is a global variable** → add to `tcc_state_stub.c` or create a new stub TU.
3. **If the symbol is another module that is itself testable** → add its source to `UT_MODULE_SRCS`.
4. **If the symbol is huge (e.g., codegen, parser)** → reconsider the test boundary. Can you test the module via a smaller public API? Can you refactor the module to reduce coupling?

**Golden rule**: the unit-test binary should remain small and fast to link.

---

## Patterns from Existing Tests

### Pattern A: Pure Predicate Functions (No State)

Best case. Just call the function with various inputs.

**Example**: `test_ir_type.c` tests `tcc_ir_type_is_float()`, `tcc_ir_type_is_64bit()`, etc. No setup needed.

```c
UT_TEST(test_type_is_float)
{
  UT_ASSERT(tcc_ir_type_is_float(VT_FLOAT));
  UT_ASSERT(!tcc_ir_type_is_float(VT_INT));
  return 0;
}
```

### Pattern B: Module with Internal Pools/Arrays

Create a minimal helper that allocates and partially initializes the struct.

**Example**: `test_ir_pool.c` creates a `TCCIRState` with only the operand pool fields set:

```c
static TCCIRState *ut_pool_new(int initial_capacity)
{
  TCCIRState *ir = (TCCIRState *)tcc_mallocz(sizeof(*ir));
  ir->iroperand_pool_capacity = initial_capacity;
  ir->iroperand_pool = (IROperand *)tcc_mallocz(sizeof(IROperand) * initial_capacity);
  return ir;
}
```

### Pattern C: Module with Live-Interval Arrays

When the module expects pre-allocated arrays with sentinel values, initialize them explicitly.

**Example**: `test_ir_vreg.c` initializes live-interval pools with `INTERVAL_NOT_STARTED`, `PREG_NONE`, etc.:

```c
static void ut_init_intervals(IRLiveInterval **arr, int *size, int *next)
{
  *size = UT_INTERVAL_INIT_SIZE;
  *next = 0;
  *arr = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * UT_INTERVAL_INIT_SIZE);
  for (int i = 0; i < UT_INTERVAL_INIT_SIZE; ++i)
  {
    (*arr)[i].start = INTERVAL_NOT_STARTED;
    (*arr)[i].incoming_reg0 = -1;
    /* ... */
  }
}
```

---

## Running Tests

| Command | What it does |
|---------|--------------|
| `make ut` | Build and run all unit tests. |
| `make ut-coverage` | Build instrumented, run, render a gcov coverage report. |
| `make ut-clean` | Remove all build artifacts. |
| `make -C tests/unit/arm/armv8m run` | Run tests for a specific target directly. |
| `make -C tests/unit/arm/armv8m coverage` | Coverage report for a specific target. |
| `make -C tests/unit/arm/armv8m clean` | Clean a specific target. |

The top-level `Makefile` also references `tests/unit/README` in the `ut` target comment; keep this document in sync if the build mechanics change.

---

## Code Coverage (gcov)

`make ut-coverage` (or `make -C tests/unit coverage`) measures **line/branch/function
coverage** of the tinycc modules under test. It complements `PASS_COVERAGE.md`, which
tracks *which optimization passes have a suite*; this tracks *which lines within the
modules under test the suites actually exercise*.

Mechanics: the `coverage` target does a clean instrumented build
(`COVERAGE=1` → `--coverage` on every TU, emitting `build/**/*.gcno` at compile time
and `build/**/*.gcda` when the binary runs), then renders a report with
[`gcovr`](https://gcovr.com):

- **Terminal**: a `lines / functions / branches` summary is printed.
- **`build/coverage/coverage.txt`**: per-file table, sorted worst-covered first, with
  the exact uncovered line numbers.
- **`build/coverage/index.html`**: browsable, line-annotated HTML.

The report is **filtered to the modules genuinely under test** — `ir/`, `arch/arm/`,
and `tccir_operand.c` — so the test harness itself (`test_*.c`, `stubs.c`) is excluded
(see `GCOVR_FILTERS` in `arm/armv8m/Makefile`).

Reading the numbers:
- The top-line aggregate is **low by construction**: `ir/gen/*.c` is linked only for its
  `irop_config[]` table (`ir/gen/config.c`; the rest is `--gc-sections`-stripped at link,
  so it reports ~1%). Look at **per-file** numbers, not the aggregate.
- Passes with focused isolated suites read high (`opt_neg_chain` ~93%, `opt_setif_or_taut`
  ~92%, the `thop_*` encoders 75–100%). The constfold/constprop/copyprop files read low
  because many of their passes are name-gated / pull frontend symbols the isolated harness
  can't yet reach — exactly the Phase F gap tracked in `PASS_COVERAGE.md`.

Requires `gcovr` (and a matching `gcov`, shipped with gcc). The instrumented build is kept
separate from the normal one — `make ut` never builds with coverage, and the `coverage`
target always cleans first so instrumented and plain objects never mix. All artifacts land
under the git-ignored `build/` tree.

---

## Checklist for New Unit Tests

Use this checklist before committing a new suite:

- [ ] Test file is named `test_<module>.c` and lives in `tests/unit/arm/armv8m/`.
- [ ] `UT_SUITE(<module>)` wraps all `UT_RUN()` calls.
- [ ] `test_main.c` has `UT_DECLARE_SUITE` and `UT_RUN_SUITE` for the new suite.
- [ ] `Makefile` lists the test file in `UT_LOCAL_SRCS`.
- [ ] `Makefile` lists the module under test in `UT_MODULE_SRCS` (if not header-only).
- [ ] No full `tcc_init()` or `tcc_ir_alloc()` is called unless absolutely necessary.
- [ ] All memory allocated in helpers is freed (no leaks under valgrind).
- [ ] `make ut` passes with `0 failed tests`.
- [ ] `make ut-clean && make ut` also passes (ensures no stale object files).

---

## Future Extensions

If the project grows unit tests for additional architectures, create new directories under `tests/unit/<arch>/<mcu>/`, model them on `arm/armv8m/Makefile`, and append the relative path to `UT_TARGETS` in `tests/unit/Makefile`.

# armv8m-tcc Test Suite

This directory contains the comprehensive test suite for armv8m-tcc.

## Test Structure

```
tests/
├── conftest.py              # Shared pytest configuration
├── run_tests.py             # Unified test runner
├── README.md                # This file
│
├── tests2/                  # C compliance tests
│   ├── conftest.py          # tests2-specific configuration
│   ├── test_suite.py        # tests2 test definitions
│   ├── README.md
│   ├── *.c                  # C test files (129 tests)
│   └── *.expect             # Expected output files
│
├── gcctestsuite/            # GCC torture tests
│   ├── conftest.py          # GCC test configuration
│   ├── test_gcc_torture.py  # GCC torture test definitions
│   ├── download_gcc_tests.sh
│   └── README.md
│
├── ir_tests/                # IR-level tests
│   ├── qemu_run.py          # Shared test infrastructure
│   ├── test_qemu.py         # IR test definitions
│   ├── *.c                  # IR test files
│   └── ...
│
└── ...
```

## Quick Start

### Run Tests

```bash
# Initialize GCC testsuite submodule (one-time setup)
git submodule update --init --depth 1 tests/gcctestsuite/gcc-testsuite

# Run GCC torture tests (default)
make test-all

# Using the unified runner
python tests/run_tests.py -v           # GCC torture tests
python tests/run_tests.py --gcc -v     # GCC torture tests
python tests/run_tests.py --ir -v      # IR tests

# Using pytest directly
cd tests
pytest -v gcctestsuite/                # GCC torture only
pytest -v ir_tests/                    # IR tests (includes some tests2)
```

### Run Specific Test Suites

```bash
# GCC torture tests (requires submodule init first)
git submodule update --init --depth 1 tests/gcctestsuite/gcc-testsuite
make test-gcc-torture-compile
# or
python tests/run_tests.py --gcc -v

# IR tests (includes curated tests2 tests)
make test
# or
python tests/run_tests.py --ir -v

# tests2 C compliance tests (WARNING: not all executable!)
make test-tests2
# or
python tests/run_tests.py --tests2 -v
```

## Makefile Targets

| Target | Description |
|--------|-------------|
| `make test` | Run IR tests (includes curated tests2) |
| `make test-all` | Run GCC torture tests (default) |
| `make test-gcc-torture-compile` | Run GCC torture compile tests |
| `make test-tests2` | Run tests2 tests (WARNING: not all executable!) |
| `make download-gcc-tests` | Initialize GCC submodule (or download) |
| `make test-full` | Run IR + GCC tests |

## Using the Unified Runner

```bash
# Run GCC torture tests (default)
python tests/run_tests.py
python tests/run_tests.py --gcc -v

# Run specific suites
python tests/run_tests.py --gcc                # GCC torture tests
python tests/run_tests.py --ir                 # IR tests
python tests/run_tests.py --tests2             # tests2 (WARNING: not all executable!)

# Run with options
python tests/run_tests.py --gcc -v -x          # Verbose, stop on first failure
python tests/run_tests.py --gcc --compile-only # Compile tests only
python tests/run_tests.py -n auto              # Parallel execution
```

## Using pytest Directly

```bash
cd tests

# GCC torture tests
pytest -v gcctestsuite/

# IR tests (includes curated tests2)
pytest -v ir_tests/

# tests2 (WARNING: not all executable!)
pytest -v tests2/

# With markers
pytest -v -m gcc_torture    # GCC torture tests
pytest -v -m execute        # Execute tests (QEMU)
pytest -v -m compile_only   # Compile-only tests

# Parallel execution
pytest -v -n auto
```

## Test Categories

### tests2 (129 tests)

**Note:** tests2 tests are primarily executed via `ir_tests/test_qemu.py` which runs a curated subset. Not all tests2 tests are directly executable.

C compliance tests covering:
- Basic C syntax and semantics
- Control flow (if, for, while, switch)
- Functions and recursion
- Pointers and arrays
- Structures and unions
- Preprocessor directives

Each test runs at `-O0` and `-O1` (2× coverage = 258 test runs).

### GCC Torture (~1000 compile + ~400 execute)

Tests from the GCC project:
- **compile/**: ~1000 compile-only tests
- **execute/**: ~400 execute tests

Tests using GCC-specific features (`__builtin_*`, `_Complex`) are auto-skipped.

### IR Tests

IR-level tests from `ir_tests/` using the IR test infrastructure.

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `GCC_TORTURE_PATH` | Path to GCC torture tests | `/tmp/gcc-testsuite/gcc/testsuite/gcc.c-torture` |
| `TCC_PATH` | Path to armv8m-tcc | `../bin/armv8m-tcc` |

## Requirements

- Python 3.8+
- pytest (`pip install pytest pytest-xdist pytest-timeout`)
- armv8m-tcc compiler (built)
- QEMU ARM (`qemu-system-arm`)
- GCC torture tests (optional, via `git submodule update --init` or `make download-gcc-tests`)

## Adding New Tests

### Add to ir_tests (Recommended)

1. Create `tests/ir_tests/NN_test_name.c`
2. Add to `TEST_FILES` in `tests/ir_tests/test_qemu.py`
3. Run `pytest tests/ir_tests/ -v -k "test_name"`

### Add to tests2

**Note:** tests2 is legacy. Prefer adding to ir_tests.

1. Create `tests/tests2/NN_test_name.c`
2. Create `tests/tests2/NN_test_name.expect`
3. Run `pytest tests/tests2/ -v -k "test_name"`

### Add to GCC torture

GCC tests are auto-discovered from `GCC_TORTURE_PATH`. To add more:

1. Download/clone GCC to `GCC_TORTURE_PATH`
2. Tests are automatically picked up

## CI Integration

```yaml
- name: Run tests2
  run: make test-tests2

- name: Run GCC torture compile tests
  run: |
    make download-gcc-tests
    make test-gcc-torture-compile

- name: Run all tests
  run: make test-all
```

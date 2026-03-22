# GCC Torture Test Suite

This directory contains the GCC torture test suite integration for armv8m-tcc.

## Overview

The GCC torture tests are a comprehensive set of C compiler tests from the GCC project:
- **compile/**: ~1000 tests that should compile without errors
- **execute/**: ~400 tests that compile, link, run, and exit with code 0

Tests that use GCC-specific features (`__builtin_*`, `_Complex`, etc.) are automatically skipped.

## Setup

### Option 1: Git Submodule (Recommended)

The GCC testsuite is included as a git submodule:

```bash
# Initialize the submodule (run from project root)
git submodule update --init --depth 1 tests/gcctestsuite/gcc-testsuite

# Or use the helper script
cd tests/gcctestsuite
bash download_gcc_tests.sh
```

### Option 2: Manual Download

If you prefer not to use the submodule, you can download the tests manually:

```bash
cd tests/gcctestsuite
bash download_gcc_tests.sh
# Follow the instructions to set GCC_TORTURE_PATH
```

## Quick Start

```bash
# Run all GCC torture tests
pytest tests/gcctestsuite/ -v

# Run only compile tests
pytest tests/gcctestsuite/ -v -m gcc_compile

# Run only execute tests  
pytest tests/gcctestsuite/ -v -m gcc_execute

# Run with parallel execution
pytest tests/gcctestsuite/ -v -n auto

# Using Make from project root
make download-gcc-tests    # Initialize submodule
make test-gcc-torture-compile
make test-all              # Run GCC torture tests
```

## Requirements

- Python 3.8+
- pytest (`pip install pytest pytest-xdist`)
- armv8m-tcc compiler (built)
- QEMU ARM (`qemu-system-arm`) - for execute tests
- GCC torture tests (via submodule or manual download)

## File Structure

```
tests/gcctestsuite/
├── conftest.py              # Pytest configuration and test discovery
├── test_gcc_torture.py      # Main test definitions
├── download_gcc_tests.sh    # Helper script (submodule init or download)
├── README.md                # This file
└── gcc-testsuite/           # Git submodule (GCC repository)
    └── gcc/testsuite/gcc.c-torture/
        ├── compile/         # Compile-only tests
        └── execute/         # Execute tests
```

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `GCC_TORTURE_PATH` | Path to GCC torture tests | `tests/gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture` |

## Markers

- `gcc_torture` - All GCC torture tests
- `gcc_compile` - Compile-only tests
- `gcc_execute` - Execute tests (compile + run)
- `slow` - Tests with longer timeout

## Skipped Tests

The following GCC features are automatically skipped:
- Complex numbers (`_Complex`, `__complex__`)
- GCC builtins (`__builtin_*`)
- IEEE exception handling
- Architecture-specific tests (mipscop)

To add more skip patterns, edit `should_skip_gcc_test()` in `conftest.py`.

## CI Integration

```yaml
- name: Initialize submodules
  run: git submodule update --init --depth 1 tests/gcctestsuite/gcc-testsuite

- name: Run GCC torture compile tests
  run: pytest tests/gcctestsuite/ -v -m gcc_compile --tb=short -n auto
```

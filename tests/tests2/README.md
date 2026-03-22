# tests2 C Compliance Test Suite

This directory contains C compliance tests for armv8m-tcc.

## Overview

The tests2 suite contains 129 C language compliance tests covering:
- Basic C syntax and semantics
- Control flow (if, for, while, switch)
- Functions and recursion
- Pointers and arrays
- Structures and unions
- Preprocessor directives
- Standard library usage

## Quick Start

```bash
# Run all tests2 tests
cd tests/tests2
pytest -v

# Or use the general runner
python ../run_tests.py --tests2

# Run with specific optimization level
pytest -v -k "O1"

# Run specific test
pytest -v -k "00_assignment"
```

## File Structure

```
tests/tests2/
├── conftest.py              # Pytest configuration
├── test_suite.py            # Test definitions
├── README.md                # This file
├── *.c                      # C test source files
└── *.expect                 # Expected output files
```

## Test Format

Each test consists of:
1. **Source file** (`NN_test_name.c`) - C test program
2. **Expect file** (`NN_test_name.expect`) - Expected output

### Tagged Tests

Some tests have multiple variants defined in the `.expect` file:

```
[tag_name]
expected output line 1
expected output line 2
[returns 0]

[another_tag]
different output
[returns 1]
```

### Multi-file Tests

Tests with multiple source files (e.g., `104_inline.c` + `104+_inline.c`) are handled automatically.

## Running Tests

### Using pytest directly

```bash
# All tests
cd tests/tests2
pytest -v

# Only -O1 tests
pytest -v -k "O1"

# Specific test
pytest -v -k "00_assignment"

# Parallel execution
pytest -v -n auto
```

### Using the general runner

```bash
# From project root
python tests/run_tests.py --tests2 -v

# With parallel execution
python tests/run_tests.py --tests2 -n auto
```

## Requirements

- Python 3.8+
- pytest (`pip install pytest pytest-xdist`)
- armv8m-tcc compiler (built)
- QEMU ARM (`qemu-system-arm`)

## Adding New Tests

1. Create `NN_test_name.c` source file
2. Create `NN_test_name.expect` with expected output
3. Run `pytest -v -k "test_name"` to verify

For tagged tests, add `[tag_name]` sections to the `.expect` file.

# Unused-function scan

`scripts/find_unused_functions.sh` runs cppcheck's whole-program
`unusedFunction` check over the compiler sources and prints functions with no
caller anywhere in the scanned set.

```bash
scripts/find_unused_functions.sh              # tests/unit count as callers
scripts/find_unused_functions.sh --no-tests   # dead in the compiler proper
```

Requires cppcheck (`dnf install cppcheck`). Takes a few minutes single-threaded
(cppcheck's unusedFunction check does not support `-j`).

## How it avoids false positives

- Sources are collected with `find`, so nested dirs (`arch/arm/thumb/`) are
  included — a caller in a missed file makes its callee look dead.
- The preprocessor config is pinned to the `armv8m` target defines from the
  Makefile (`DEF-armv8m`); without pinning, cppcheck either tries every
  `#ifdef` combination (extremely slow with `--force`) or picks arbitrary
  configs (wrong results).
- `-DTCC_LOG_ALL=1` and `-DCONFIG_TCC_DEBUG=1` keep call sites inside
  `LOG_*` / `THOP_TRACE` / dump-only code visible.
- `tests/unit` sources count as callers by default, so a flagged function is
  removable without breaking the unit-test harness. Findings located in test
  files themselves are filtered out.

## Remaining caveats

- Functions called only through function pointers built by token-pasting
  macros can still be flagged; grep before deleting.
- The public libtcc API (`tcc_add_symbol`, `tcc_set_error_func`,
  `tcc_get_symbol`, ...) is unused internally by design.

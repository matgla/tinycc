# Phase 8 — Self-host bootstrap gate

This directory implements the self-host coverage layer from
`docs/plan_whole_tinycc_coverage.md`.  It contains two gates:

1. **Compile-only smoke gate** (`test_selfhost_compile.py`)  
   Cross-compiles the tinycc source files with `armv8m-tcc`.  This proves the
   compiler can ingest its own source tree and runs in the standalone
   `libs/tinycc` checkout without YasOS.

2. **FAT-drive round-trip gate** (`test_selfhost_fat.py`)  
   Copies a curated subset of `tests/tests2/` onto the YasOS guest FAT drive,
   compiles each one with the native `/usr/bin/tcc`, runs it, and compares the
   output and exit code against the cross-compiled reference.  This gate
   requires the YasOS repository and skips cleanly when it is absent.

## Running

```bash
# Compile-only gate (works in libs/tinycc checkout)
make cross
pytest tests/selfhost -v -m selfhost_compile

# Full self-host gate (requires YasOS checkout)
pytest tests/selfhost -v

# Via the top-level Makefile target
make test-selfhost
```

## Dependencies for the FAT gate

The FAT gate auto-detects the YasOS repository by walking up from the tinycc
root.  It expects:

- `../../scripts/qemu_fatdisk_run.py`
- `../../zig-out/bin/yasos_kernel`
- `../../rootfs/usr/bin/tcc` (or `../../libs/tinycc/bin/armv8m-tcc.elf`)

If any of these are missing, the FAT tests are skipped with an informative
message.  To build them:

```bash
cd ../../                 # YasOS repository root
./build_rootfs.sh -o rootfs.img
zig build                 # produces zig-out/bin/yasos_kernel
```

## Curated tests2 subset

The FAT gate uses a small, conservative list of `tests2` cases that only need
basic libc support (`printf`, `puts`, simple string/array operations).  Expand
the list in `test_selfhost_fat.py::SELFHOST_FAT_TESTS` as the native runtime
support grows.

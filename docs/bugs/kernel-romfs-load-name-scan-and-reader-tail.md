# ROMFS `FileHeader.load`: the `_reader` tail still spills `a1`/`data_offset`; the name scan is not unrolled

**Status:** open · **Severity:** performance · **Found:** 2026-10-12 (20:45 kernel_compare battery, per-PC attribution, disassembly)

## Summary

`FileHeader.load` ran 1,545,943 TCC instructions against 1,001,136 for LLVM over 11,262 calls
(137.3 against 88.9 per call). The round-2 reader-tail fix brings it to about 108 per call (estimate).

## LLM reporting name

kernel-romfs-load-name-scan-and-reader-tail

## Reproducer

`libs/tinycc/docs/bugs/repro/kernel-romfs-load-name-scan-and-reader-tail.c` (the compile
command is in the file header). In situ, the head of the tail of
`fs_romfs_file_header_FileHeader_load__16417` still reads both values back from the frame:

```
ldr r0,[sp,#12] ; asrs r4,r0,#31 ; ldrd r6,r7,[sp,#12] ; ldrd r2,r3,[sp,#16] ; mov r0,r3
strd r6,r4,[r5] ; strd r7,r0,[r5,#8]
```

LLVM: `strd fp,r0,[r4]; strd r9,r8,[r4,#8]`, with both values in registers.

## Root cause (what remains)

- **Whole-range spills of `a1` and `data_offset`.** `a1` is spilled at entry and read back in
  the tail, and `[sp,#12]` is loaded twice. `data_offset` (the u64 phi of the initial `#32` and
  the long-name path's value) lives in `[sp,#16..#20]`, stored on every path into the tail
  (`strd sl,fp,[sp,#16]` six times). Both values are live across the cold long-name calls.
  This is the same class as `kernel-template-replay-invariant-spill-reload.md`. The constant
  `#32` initial value is not rematerialised. The small `reader_tail()` in the repro shows the
  same RA shape without calls: `push {r3,r4,r6,r7}; mov r4,r2; mov r6,r3` copies the u64
  argument into callee-saved registers for no reason.
- **No unrolling of constant-trip search loops.** LLVM unrolls all 16 tests of the scan
  (3 per byte). No tcc unroller handles a constant trip count with a data-dependent early exit.

## Regression lock

`tests/ir_tests/bug_romfs_load_u64_tail.c`, `tests/ir_tests/bug_interleaved_word_copy_ldm.c`;
`test_codegen_asm.py::test_romfs_load_u64_tail_stays_in_registers`,
`test_romfs_load_name_scan_bottom_tested`, `test_interleaved_word_copy_pairs_or_blocks`
pin the parts already fixed. Nothing yet covers the spills or the unroll.

## Likely fix

All savings are estimates from the 2026-10-13 per-PC counts.
1. Rematerialise constant-valued spilled vregs, and split the `a1`/`data_offset` ranges
   around the cold calls. About 4-6 instructions per call (45-70k).
2. Unroll constant-bound search loops of 16 trips or fewer (LLVM's shape, larger code). About
   2 per byte (130k).

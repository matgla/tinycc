# Wyhash's 48-byte mixing loop retains pair repacking and spills

**Status:** open · **Severity:** performance · **Found:** 2026-10-11
(fresh attribution of the residual `zig-version` profile)

## Summary

`Wyhash.update` executes **115,904 TCC instructions versus 64,352 LLVM inclusive
instructions** in `zig-version`. The loop processes **48-byte blocks**, with
705 body iterations. TCC executes roughly 164 instructions per body iteration;
dividing LLVM's inclusive function count by 705 gives approximately 91, including
its function overhead. These are diagnostic counts, not identical loop regions.

Explicit SP memory operations are **28,301 (24.4%)** of the TCC function's
instructions. Surviving pair repacking, register moves, and a spilled zero
feeding UMAAL remain visible. Historical claims of 32-byte blocks or half of
dynamic instructions being stack traffic are not supported by this trace.

## LLM reporting name

kernel-wyhash-pair-repacking-spills

## Reproducer

Run `zig-version` on the paired kernels using
[the battery commands](kernel-bottlenecks-2026-10-12.md).
Inspect `hash_wyhash_Wyhash_update__5825` in the frozen TCC ELF. The hot loop
is `0x10010544..0x1001076c`; the body at `0x10010552` executes 705 times.
In the mix sequence, a zero is stored to `[sp,#28]` and reloaded into `fp`
for a UMAAL addend. The original investigation had no reduced reproducer.
`tests/ir_tests/bug_wyhash_pair_spills.c` now covers the multiply chain,
three-lane 48-byte rounds, carry boundaries, and byte offsets 0–7.

## Root cause

Candidate interaction between pair-producing UMAAL fusion, PACK64/extract
canonicalization, and register allocation. Inspect `source/opt/ra/umaal_fusion.c`,
`source/opt/flat/fusion/pack64.c`, and the allocator's surviving intervals.
Existing pack64 tautology handling does not establish elimination of all
PACK64-to-half extraction chains. The first missed transform is not isolated.

## Regression lock

`tests/ir_tests/test_wyhash_codegen.py` locks a stack-free single-addend
UMAAL and exactly two zero materializations in the reduced mum at
`-O1`, `-O2`, and `-Os`. The stack assertion fails on the saved pre-change
cross at all three levels. Runtime coverage is in
`bug_wyhash_pair_spills.c` / `.expect`, registered in `test_qemu.py`.
The prior unaligned wide-load correctness fixes remain distinct.

## Likely fix

Reduce the pair chain, preserve the required high/low halves, and simplify
redundant repacking before allocation; avoid keeping a spill solely to supply
a zero addend where a safe alternative reduces pressure. Validate carries,
aliasing/alignment, hash outputs, frame size, and dynamic counts. The fresh
function estimates are non-additive with outlined child costs.


## What remains

The single-addend UMAAL zero pair is fixed (`Wyhash.update` in `zig-version`
115,904 → 101,793 instructions; tests `bug_wyhash_pair_spills.c`,
`test_wyhash_codegen.py`). PACK64 result assembly and other pair copies
remain: removing their narrow snapshots can extend the wide pair's lifetime
and raise register pressure, so they are not proven redundant.

Fixed 2026-10-14 (local-combined `e199e576`): a 64-bit value's `SHR #32` into
a 32-bit destination now reads the high half directly
(`arm-thumb-alu.c:data_processing_mop_impl`), and the 8-byte frame growth the
UMAAL fix introduced is gone (`test_wyhash_codegen.py` bounds the
accumulator's instruction count and saved registers and forbids stack traffic
in `high_addend`). The residual pair copies and spill pressure above remain.

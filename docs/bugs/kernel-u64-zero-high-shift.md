# Dead high halves of 64-bit values still claim registers

**Status:** open · **Severity:** performance · **Found:** 2026-10-12 (20:45 kernel_compare battery)

## What remains

zero_half64 runs after register allocation. A pair whose high word is known
zero and never materialized keeps its allocated high register.
`plain_expand` in
[repro/kernel-u64-zero-high-shift.c](repro/kernel-u64-zero-high-shift.c)
is 8 instructions of work wrapped in `push {r4-r7}` / `pop`. r5 and r7 are
reserved for high words that are never written. gcc needs 7 instructions and
no frame. In the kernel's `getIndex` the frame already exists for the
multiply, so this costs nothing there. It matters for small leaf functions.

A fix needs the dead-half fact before allocation, so that such a vreg gets a
single register. The fact is only sound against the final instruction stream,
which is why zero_half64 runs last; see the pass header. Post-allocation
passes (cmp_imm_swap, backedge_phi_hoist, jump threading, coalescing) must
not then add a reader of the half. Either re-check after them, or let the
allocator release the high register once zero_half64 has confirmed it.

## Regression lock

- `tests/ir_tests/bug_u64_zero_high_shift.c` (runtime, all levels) and
  `tests/ir_tests/test_zero_high_shift_codegen.py` (asm shape; `expand`
  excludes push/pop from its count because of the item above)
- `vol_*` in both: a volatile u64 local keeps both words of its store

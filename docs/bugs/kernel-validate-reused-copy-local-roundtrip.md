# ELF validator residuals: validate's relocation loops, names_end and validate_hash

**Status:** open · **Severity:** performance · **Found:** 2026-10-12 (20:45 kernel_compare battery, per-PC attribution, disassembly)

## Summary

ELF validator family (`dynamic_loader/source/parser.zig`), kernel PCs only.
What remains, per function (tcc vs LLVM, from the 20:45 attribution; LLVM
inlines the data loop into `loader.Loader.process_header`):

**`validate` data loop** (9,814 iterations; was 30 executed instructions vs
LLVM 19, the 5-instruction record round-trip is gone):

| item | insns/iter | ≈ total |
|---|---:|---:|
| dead `span_end` payload `add r8,sl,#4`, plus a layout `b.n` | 2 | 20k |
| `count > (limit-start)/4` with count 1, emitted as `lsr; cmp #1` and not as `cmp #4`; separate `start>limit` test | 2 | 20k |
| `and ip,sl,#3; cmp ip,#0`, not `tst`/`ands` | 1 | 10k |
| `mov r2,fp` before `ubfx` | 1 | 10k |
| invariant `h->const_rodata_length` reloaded every iteration (gcc hoists it) | 1 | 10k |

**`names_end` (+44k vs LLVM).** Per name tcc runs 55 instructions vs LLVM
about 29, across 2,208 names:

- `span_end(at,1,prefix,end)` keeps the `udiv` and materializes its bool: `ite hi; movhi; movls; b; cbz`, 14 vs 4, about 22k. LLVM folds `1 > x/p` to `p > x`.
- The loop-invariant `lookup != null` test is re-evaluated, and `a6.ptr` is loaded twice, every iteration (LLVM unswitches): about 11k.
- `a1` is reloaded from `[sp,#28]` with `lr` free.
- Executed alignment `nop`s on fall-through paths: 4,416.

**`validate_hash` (+20k):**

- the `while (index != 0 and remaining != 0)` condition is materialized as `ite ne; movne; moveq; cbz` (3 extra × 4,354);
- 4,438 executed `nop`s.

## LLM reporting name

`kernel-validate-reused-copy-local-roundtrip`

## Reproducer

`libs/tinycc/docs/bugs/repro/kernel-validate-reused-copy-local-roundtrip.c` is
the data-relocation loop as kernel.c spells it
(`libs/tinycc/bin/armv8m-tcc -O2 -fno-pic -c ...`); compare with
`arm-none-eabi-gcc -O2 -mcpu=cortex-m33` (19-instruction loop, hoisted
`h->const_rodata_length`). In situ: `parser_Parser_validate__9133`,
`parser_names_end__9128`, `parser_validate_hash__9129` in the kernel ELF of
the battery above.

## Root cause

Not traced per item. The `cmp #4` and `span_end` bool items are a missing
fold of `n > x / c` (unsigned, constant c) into a compare of `x` against
`n * c` once inlined; the materialized-bool loop conditions are the same
class in `names_end` and `validate_hash`.

## Regression lock

- `tests/ir_tests/bug_ptr_local_fwd_multidef_copy.c` and
  `test_codegen_asm.py::test_ptr_local_fwd_multidef_copy_no_record_roundtrip`
  lock the fixed round-trip (no record slot store in the loops).
- Nothing covers the residuals.

## Likely fix

Separate small fixes; the largest single lever is the `n > x / c` fold
(about 42k across `validate` and `names_end`).

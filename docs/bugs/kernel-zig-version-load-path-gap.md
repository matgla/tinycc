# zig-version loads spend 1.53× LLVM's kernel instructions on the yaff validate/hash/reserve walk

**Status:** open · **Severity:** performance · **Found:** 2026-10-10
(kernel_compare battery `results-20261010-115749.json` + per-PC attribution of
the gated-icount CSVs; tcc kernel `07d047f2…`, llvm kernel `1f888a07…`, rootfs
`fd5b5b66…`)

## Summary

`zig version` runs 876,395 instructions under the tcc kernel vs 571,114 under
LLVM (1.53×). The window is one process launch of the on-device `zig` that
panics early, so the gap is almost entirely the **kernel-side load path**:
the yasld parser walking the 22 MB zig yaff image. Kernel-only counts
(PC < 0x60000000, the XIP userspace executes identically on both) are
774,685 vs 469,404 — a 305,281-instruction gap, decomposed:

| function (tcc symbol)            |  tcc insns | llvm insns* |   gap | share |
|----------------------------------|-----------:|------------:|------:|------:|
| `parser_Parser_validate__9131`   |    158,323 |      60,319 | +98,004 | 32% |
| `hash_wyhash_Wyhash_update__5825`|    121,544 |      64,352 | +57,192 | 19% |
| `ProcessMemoryPool_reserve_pages__4569` | 92,305 | 54,038 | +38,267 | 13% |
| `memset`                         |    114,962 |      94,369 | +20,593 |  7% |
| `mem_findScalarPos__anon_1363`   |     32,664 |      19,727 | +12,937 |  4% |
| `memcpy`                         |     74,820 |      64,361 | +10,459 |  3% |
| `parser_validate_hash__9127`     |     13,490 |       7,269 |  +6,221 |  2% |
| `mem_eql__anon_10757`            |      6,064 |       1,102 |  +4,962 |  2% |
| `loader_Loader_load_from_template` (own body) | 52,256 | 48,013 | +4,243 | 1% |

\* llvm numbers are **inclusive** (all DWARF inline frames containing the
function, `arm-none-eabi-addr2line -f -i`), because LLVM inlines
`names_end`/`validate_hash`/`span_end` into `validate` and wyhash into its
callers. Everything else (kheap/malloc stack, RankedMutex, uart) is at parity
or tcc-cheaper on this workload.

Single-call magnitudes: `validate` runs **once** and costs 158,323 insns;
`Wyhash.update` runs **twice** (name tables) at ~60,772/call;
`reserve_pages` 8 calls at ~11,538/call.

## Root cause

**1. Bool round-trip in tight validation loops (the dominant shape).**
The CBE spells every comparison as a materialized bool temp —
`t5 = t4 < UINT32_C(4); … if (t5) { … }` (kernel.c, `parser_Parser_validate`
body) — and tcc never folds SETCC→BRANCH back into a flags branch, not even
within one basic block. The relocation-check loops
(`for (parser.symbol_table_relocations.relocations) |rel|` … four loops,
3,072 iterations of the hottest) execute ~40 insns/iteration where LLVM's
inlined equivalent (`loader.Loader.process_header+0x800`) runs ~12:

```text
tcc  @1003acec  and.w ip, r3, #3        ; rel.to % 4
tcc  @1003acf0  cmp.w ip, #0
tcc  @1003acf4  ite  ne
tcc  @1003acf6  movne r3, #1            ; bool materialized…
tcc  @1003acf8  moveq r3, #0
tcc  @1003acfa  cmp  r3, #0             ; …then re-compared
tcc  @1003acfc  beq  …
llvm @100122ba  ands.w r2, r6, #3
llvm @100122be  bne.w <error>           ; branch straight off the flags
```

The same 4-instruction `ite/movne/moveq + cmp #0 + cbz` pattern appears twice
per validate iteration (`mov.w ip,#1; cmp ip,r8; ite hi` is the second
instance, the span-length check). Per iteration it also zeroes an 8-byte
**stack temp** that the inlined `span_end` error-union result is written
through (`add.w ip,sp,#80; mov lr,#0; str [ip]; str [ip,#4]`) — an
address-taken aggregate the optimizer keeps re-initializing.

**2. `Wyhash.update` residual (1.9×, 172 vs 91 insns per 32-byte block).**
Both kernels run 707 32-byte blocks. The tcc loop head round-trips its cursor
through memory every block:
`add.w r0, fp, #48; str r0,[sp,#40]; ldr r1,[sp,#40]; ldr r4,[sp,#68]; cmp;
bcs` — a same-block store→load pair `sl_forward` does not forward (same
family as the closed `switch_to_data` STORE-form gap). The variable-length
tail copies and the word-wise `__tcc_memmove` helper remain as noted in the
streaming-wyhash closure entry in `README.md`.

**3. `reserve_pages` per-page bit test (1.7×, ~16 insns/page × 2,536).**
Per page: reload bitmap base (`add r1,r9,#12; ldr r2,[r1,#12]`), `lsrs/ubfx/
movs+lsl` to build the mask, then the same 4-insn bool round-trip on
`and r1,r3,r0; cmp #0; ite ne; movne/moveq; cbz`. The word-wise `markRange`
fix (closed page-bitmap report) does not apply — `reserve_pages` scans
bit-by-bit in the Zig source, so the compiler-side lever is the bool fold
plus hoisting the invariant `ldr r2,[r9+24]` out of the loop.

`memset` (+20.6K over 18 huge page clears) is the known libc clearing policy,
not a new gap; `findScalarPos`/`eql`/`validate_hash` costs are the same
shapes tracked in `kernel-ls-bin-scan-family-gap.md`.

## Reproducer

Profile-derived; re-derive with the pinned battery (no kernel rebuild
needed):

```bash
python3 scripts/kernel_compare.py --skip-build --variants tcc,llvm   # 11:57 battery state
# attribution (inclusive llvm via DWARF, kernel-only PC<0x60000000):
ls .cache/kernel_compare/insns/{tcc,llvm}-zig-version.csv
arm-none-eabi-nm -S -n .cache/kernel_compare/tcc/bin/yasos_kernel | grep validate
arm-none-eabi-objdump -d -mthumb -S .cache/kernel_compare/tcc/bin/yasos_kernel
```

For a source-level repro, extract the four relocation loops verbatim from
kernel.c (`.zig-cache/o/<hash>/kernel.c`, `parser_Parser_validate__NNNN`) —
per the 2026-10-09d lesson, hand-minimized C compiles clean; only the real
CBE temp structure (bool temps, address-taken opt structs, sret chains)
reproduces the shapes. Needs `#include <zig.h>`,
`-I <zvm>/master/lib`, `#define ZIG_TARGET_MAX_INT_ALIGNMENT 8`, a
non-static driver, and stub callees.

## Likely fix

The umbrella lever is a compare→branch folding pass (or `sl_forward`
extension) that recognizes `BRANCH(CMP(SETCC(x)))` and `BRANCH(bool-slot)`
**within a basic block** and branches on the original flags, deleting the
`ite/movne/moveq` pair and the `cmp #0`. Demonstrated instances in this one
workload: validate ×2/iteration ×3,072, reserve_pages ×2,536, range_ok,
vcall map probe, `mem.eql` prologue ×3,398 (see the ls-bin report). A
secondary lever is sinking/deleting the per-iteration 8-byte opt-struct
re-zeroing in loops (validate) and forwarding same-block `str`/`ldr` cursor
pairs (wyhash loop head). Expected recovery if the bool fold lands: roughly
half of the validate+reserve gap, i.e. ~60–70K instructions on this workload
(~0.13× of the total ratio), plus shares of every other workload's scan
loops.

# Template-replay loops: scaled index not folded into the entry address, thunk loop at full pressure (load_from_template, ~52k over LLVM)

**Status:** open · **Severity:** performance · **Found:** 2026-10-12 (20:45 kernel_compare battery) · **Rewritten:** 2026-10-13

## Summary

`loader_Loader_load_from_template__8907` (yasld template replay: `View.apply` with `replay_range` ×2 and `replay_thunks` inlined, `dynamic_loader/source/template.zig`).

About 52k remains over LLVM (466,724 vs 414,412 inclusive). It is not register allocation:

| loop (kernel.c `a92895f5`, object offsets) | iters/battery | tcc insns/iter | LLVM | remaining gap |
|---|---:|---:|---:|---|
| `replay_range` data `0x560–0x58c` | 18,075 | 12 | 13 | none (tcc is shorter) |
| `replay_range` GOT `0x5b4–0x5e8` | 9,002 | 14 | 14 | the scaled-index fold below (1/iter) |
| `replay_thunks` `0x624–0x688` | 1,357 | 27 | ~20 (unrolled ×4) | stores not folded, 1 reload, no unroll |

## Remaining items

1. **Scaled index not folded into the entry address.** Each replay loop computes the entry address in two instructions, because two loads share it (`ldr [r9]`, `ldr [r9, #4]`):

   ```
   mov.w ip, r0, lsl #3
   add.w r9, r2, ip
   ```

   LLVM computes it in one: `add.w r9, r2, r0, lsl #3`. The SHL result has one use, the ADD, so the barrel-shift fusion (`source/opt/flat/fusion/barrel_shift.c`) should take it. Cost: one instruction per data and GOT iteration, about 27k.

2. **Thunk loop stores not folded.** The thunk slot stores are emitted as `add r9, r8, #4; str r2, [r9]` (×3) instead of `str r2, [r8, #4]`. The slot is `*align(1) [4]u32`. Cost: three instructions per iteration. Each `add` also takes a register at the loop's full pressure, which is what leaves one value reloaded:

   ```
   668: str.w r0, [r8]
   66c: add.w r9, r8, #4
   670: str.w r2, [r9]
   674: add.w r9, r8, #8
   678: str.w r6, [r9]
   67c: add.w r6, r8, #12
   680: str.w ip, [r6]
   ```

3. **Thunk loop reloads `thunks`.** The loop reloads the base every iteration (`ldr.w lr, [sp, #80]`). With 15 values live in 13 allocatable registers it cannot avoid one reload. Folding the stores (item 2) frees the register. LLVM unrolls the loop ×4 (about 10k).

## Reproducer

Reproduce it from the exact benchmarked C. The register-pressure-dependent parts need the whole 650-instruction function.

```
cd /home/mateusz/repos/yasos.zig
K=.zig-cache/o/a92895f5d98b120da02d78e7ec8a01eb/kernel.c
libs/tinycc/bin/armv8m-tcc -O2 -fno-pic -ffunction-sections -fdata-sections -DYASLIBC_REAL_BOOL \
  -mfloat-abi=hard -mfpu=fpv5-sp-d16 -fvisibility=hidden -minline-atomics -include apps/zig/zig_c_prelude.h \
  -I ~/.zvm/master/lib -c $K -o k.o
arm-none-eabi-objdump -d --no-show-raw-insn -j .text.loader_Loader_load_from_template__8907 k.o
```

Item 1 should reproduce in a small C loop that reads two adjacent words through `base + (i << 3)`.

## Regression lock

- `tests/ir_tests/bug_ra_live_through_loop_split.c` / `test_ra_live_through_loop_split` keeps the live-through split.
- `tests/ir_tests/bug_ra_loop_split_rescan_carriers.c` / `test_ra_loop_split_rescan_carriers` keeps the carrier splits.

Nothing locks items 1–3.

## Likely fix

1. Find why `barrel_shift` skips a SHL whose ADD result feeds two loads. Check whether it prefers folding into a load's register offset and gives up when there are two.
2. Fold `ADD base, #imm` → `STORE [addr]` into `STORE [base, #imm]` for word stores whose pointer is `align(1)`. Cortex-M33 allows unaligned STR.
3. After item 2, re-measure item 3 before considering unrolling.

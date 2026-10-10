# TCC -O2 vs Zig/LLVM kernel: rebenchmark and next bottlenecks — 2026-10-09 (evening)

A from-scratch rebuild + full battery after the afternoon's codegen fixes landed
(`f17818b9`, `8395ce4c`, `de62aba4`, `9efbcda3`), the kernel-side wyhash integer-map fix,
and the loader/VFS rework in the working tree. This run supersedes the 19:01 attempt
(whose failures were the device-zig `build-obj` fault — fixed by the `is_struct`
loop-split exclusion — and the console-stall hardfault, fixed separately).

## Inputs

- YasOS `a8cb6be6` + working tree, tinycc `9efbcda3` (+ doc-only commits).
- Fresh rootfs `ff3cff29…` (26,486,784 B, on-board zig included), cross rebuilt from the
  committed tree (the 19:56 cross in `bin/` had been built from then-uncommitted WIP and
  produced `R_ARM_ABS32`-in-text libtcc1 code that broke `libc.so`'s `-shared` link; no
  committed state reproduces it — dc20/ef77/f178/dde6/9ef scratch crosses all emit 0).
- TCC kernel `53f6f4ad…` (26,590 KiB), LLVM kernel `e381021c…` (29,117 KiB).
- Work dir `.cache/kernel_rebench_20261009d/` (battery `results-20261009-203533.json`,
  warm census `warm-results.json`, per-PC CSVs under `insns/`, attribution
  `attribution.json` / `attribution_llvm.json`).

## Battery (all 15 workloads: outputs identical, rc sane; zig-version 255 on both)

| Workload | TCC | LLVM | Ratio | vs 15:31 |
| --- | ---: | ---: | ---: | ---: |
| noop | 72,871 | 71,331 | 1.02× | |
| `/bin/true` launch | 296,851 | 237,229 | **1.25×** | 1.28× |
| uname | 329,766 | 259,722 | 1.27× | 1.29× |
| ls /bin | 3,768,887 | 2,849,871 | **1.32×** | 1.34× |
| sha256sum | 373,110 | 268,698 | **1.39×** | 1.39× |
| ps | 1,177,993 | 808,285 | **1.46×** | 1.53× |
| core-sort / core-grep | 20.04M / 7.32M | 18.28M / 7.07M | 1.10× / 1.03× | 1.11× / — |
| tcc hello -O0 / -O2 | 6.24M / 6.70M | 5.05M / 5.52M | 1.24× / 1.21× | 1.23× / — |
| tcc compile+run hello | 6,977,812 | 5,557,893 | 1.26× | 1.25× |
| tcc bench.c -O2 | 62.16M | 60.89M | 1.02× | |
| zig render→C→tcc | 521.9M | 512.0M | 1.02× | 1.04× (broken at 19:01, works again) |

Warm-launch census (6 boots × 50 launches): **tcc 133,106 vs llvm 99,323 = 1.34×**
(afternoon: 1.416×). The tcc arm dropped −7.5% on launch workloads vs the 15:31
baseline, llvm −4% (shared loader/VFS wins).

## Where the gap lives now

Warm per launch: **replay is at parity** (tcc `load_from_template` 32,074 vs llvm's
inlined `load_module` 32,399 — the afternoon's biggest item is gone), `memcpy` parity
(14,030/14,108), and the remaining warm delta is pool-family ~5.9k
(`reserve/mark_used/mark_free` 16.4k vs 10.5k), `memset` 2.1k (libc bytewise head/tail
policy), `mem_eql` ~5.1k, kheap lock/unlock ~4.3k.

The ratios are now made on the **cold/syscall-heavy paths**, in this order:

| Bottleneck | share of `ls-bin` gap (919k) | report |
| --- | ---: | --- |
| `mem.eqlBytes` byte assembly (94 vs ~7 insns/compare) | 34% | **Fixed in `858bd8c3`**; permanent regression: `tests/ir_tests/bug_eqlbytes_kernel_roundtrip.c` |
| RankedMutex lock/unlock out-of-line (~62 vs ~13 insns/op × 4,192 ops) | ~13% | [`rankedmutex-and-romfs-header-scan.md`](rankedmutex-and-romfs-header-scan.md) |
| `FileHeader.load` 2.2× + `findScalarPos` 3× (romfs scan family) | ~25% | [`romfs-header-scan-costs.md`](romfs-header-scan-costs.md) |
| u64 division layer cake (core-sort: 125 vs 57 insns/div × 4,560) | 17% of core-sort's gap | [`u64-division-layer-cake.md`](u64-division-layer-cake.md) |

Not re-filed: `ps`'s allocator family (`MallocAllocator` + kheap lock ≈ 2×) shares the
mutex/param mechanisms above; the sha256sum `Wyhash.update` residual (32k insns) is the
open submodule `wyhash-out-of-line-hash-and-init` report's remaining shape (string-keyed
maps — the integer-map fix landed kernel-side); `memset` stays a libc policy item.

## Notes for the next session

- The llvm kernel ELF is symtab-stripped in ReleaseFast (keeps DWARF): attribute its
  CSVs with `arm-none-eabi-addr2line -f`, not `nm` (`attribute_llvm.py` in the work dir).
- `zig build` re-renders `kernel.c` per config; today's tcc-kernel input is
  `.zig-cache/o/9787281303c172783a4b3d9b9bcdbb88/kernel.c` (10,086,389 B).
- Scratch crosses for A/B-ing tinycc revisions live in `/tmp/xbit/{dc20,ef77,f178,dde6,x9ef,dbg}`
  (worktrees + the build_rootfs configure line; `/tmp` may have been wiped since).

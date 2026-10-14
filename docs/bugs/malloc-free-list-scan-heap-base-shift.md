# Same malloc calls cost longer free-list scans when the heap base shifts

**Status:** open · **Severity:** performance · **Found:** 2026-10-11
(zig-obj-tcc battery regression; per-PC CSV diff; 2x2 binary/kernel A/B matrix
through identical copy-path commands)

## Summary

The 2026-10-11 afternoon battery showed the `zig-obj-tcc` workload at
**+4,012,450** instructions versus the morning baseline (per-workload ratio
1.004 -> 1.013 versus LLVM). The entire userspace delta is inside **libc.so
text**: the tcc step's libc.so cluster rose 20,367,954 -> 24,644,850
(**+4,276,896**); the zig step was flat and the kernel delta was -115,275.
The two libc.so files are **byte-identical** (same content, same SHA) — this
is more work, not new code.

**This delta is not a regression: it is inside the per-boot noise band of
this workload** (see Statistical noise below). Five repetitions of the
identical workload on each kernel give userspace means of 490,820,988 (old
kernel) versus 489,614,608 (new kernel) — the new kernel is 1.2M *lower* on
average, within ~1 SEM of the ~2.2-2.7M standard deviation. The battery
compared single samples: its old-kernel sample sat near the bottom of the
old-kernel band and its new-kernel sample near the top of the new-kernel
band, manufacturing a ~4M swing out of noise.

The hot range is malloc's free-list scan loop. `malloc` is exported at libc.so
code offset `0x1c49`; hot blocks map to `malloc+0xE7`, `+0x127`, `+0x147`,
`+0x167`, `+0x1E7`, `+0x207`. The malloc **entry count is identical in all
four binary/kernel matrix cells: 44,670 calls** (per-PC counts at the entry
block `0x60040060..0x6004006c`). Whole malloc-range totals: old@old 5.53M ->
new@new 14.55M. So the same libc code, the same number of calls, produces
longer per-call scans: a heap-state effect, not a compiler output effect.

A `-bench` A/B of both tcc binaries on one kernel shows an **identical
allocation profile** (big-allocs 430 calls / 2007 KiB, same alloc sites and
sizes) and a byte-identical `.o` (same SHA). The allocation stream is the
same; only the heap's free-list structure differs.

The 2x2 matrix (old/new tcc binary x old/new kernel, all four cells run the
same `cp /mnt/tcc_x /tmp/tc; /tmp/tc -c ...` copy-path command so the yasld
template cache is equally absent in every cell) separates two
multiplicative factors in the libc-cluster totals:

| cell | userspace | libc cluster | kernel |
|---|---|---|---|
| old binary @ old kernel | 119,404,085 | 17,629,569 | 12,113,483 |
| new binary @ new kernel | 128,086,970 | 26,482,245 | 11,977,653 |
| new binary @ old kernel | 125,246,390 | 23,632,854 | 12,149,419 |
| old binary @ new kernel | 123,471,842 | 21,700,050 | 11,968,685 |

- Binary factor (same kernel): +6,003,285 on the old kernel, +4,782,195 on
  the new kernel.
- Environment factor (same binary): +4,070,481 with the old binary,
  +2,849,391 with the new binary.

Both factors act on the same mechanism, and the new binary is *better* than
the old one in the new environment — the regression does not require the
tcc edits at all (old binary alone gains +4.07M on the new kernel).

**Caveat:** each matrix cell was a single sample, and single samples of this
workload carry a ~2-3M free-list noise (see Statistical noise). The cell
*magnitudes* above are noise-contaminated and should not be read as
factor sizes; what the matrix establishes robustly is the invariant part:
identical malloc call counts and identical allocation streams across cells.

## Statistical noise

Five repetitions of the exact battery `zig-obj-tcc` workload on each kernel
(2026-10-11, driver `/tmp/opencode/clt_stats.py`, `kernel_compare.run_workload`
per rep, installed `/usr/bin/tcc` path) are not reproducible in userspace:

| rep | old-kernel userspace | old libc cluster | old kernel | new-kernel userspace | new libc cluster | new kernel |
|---|---|---|---|---|---|---|
| 0 | 493,561,379 | 26,168,683 | 14,930,223 | 493,559,573 | 26,333,458 | 14,777,369 |
| 1 | 492,753,496 | 25,367,707 | 14,936,204 | 487,402,503 | 20,177,113 | 14,736,501 |
| 2 | 490,276,377 | 22,889,751 | 15,250,839 | 492,305,959 | 25,078,859 | 14,746,712 |
| 3 | 490,276,377 | 22,889,751 | 15,244,334 | 487,402,503 | 20,177,113 | 14,736,501 |
| 4 | 487,237,313 | 19,847,746 | 14,965,183 | 487,402,503 | 20,177,113 | 14,743,372 |

- old kernel: userspace mean 490,820,988, min 487,237,313, max 493,561,379,
  stdev 2,221,723; libc cluster mean 23,432,728, stdev 2,221,321; kernel
  mean 15,065,357, stdev 149,274.
- new kernel: userspace mean 489,614,608, min 487,402,503, max 493,559,573,
  stdev 2,738,114; libc cluster mean 22,388,731, stdev 2,737,569; kernel
  mean 14,748,091, stdev 15,166.

Between-rep per-PC diffs put **99.4% of the userspace spread inside the
malloc free-list scan loop** (`0x6004015c..0x600401b6`), plus a fixed
+/-94,139 instruction pair in two small userspace images (a branch pair
taken differently per boot, `0x6601e01c`/`0x6602e41c`) and small libc
newlocale-region shifts; kernel-region counts are stable (+/-35k on the old
kernel, +/-15k on the new one). The new kernel's runs cluster bimodally
(three runs identical at 20,177,113 libc, two above 25M).

Consequences for measurements:
- Any single-sample userspace comparison of this workload against a delta
  below ~4M is indeterminate; compare means over >=5 reps, and treat a
  difference as real only beyond ~2 x (stdev/sqrt(n)) ~ 2M here.
- The kernel-only comparison is tight (stdev <= 150k) and single samples are
  usable there.

## LLM reporting name

malloc-free-list-scan-heap-base-shift

## Reproducer

Battery regression (from the YasOS checkout root):

```
python3 scripts/kernel_compare.py --work .cache/kernel_compare_current_20261011_120800 \
  --baseline .cache/kernel_compare_current_20261011_071509/results-20261011-091617.json
```

Then diff `insns/tcc-zig-obj-tcc.csv` between the two runs: the +4.0M lands in
the libc.so text range (base `0x6003e410`), inside `malloc`.

Clean 2x2 attribution: four boots, each mounting a payload (`hello_zig.c` plus
`cp`-ed copies of both native tcc binaries) at `/mnt` and running the same
compile through `/tmp/tc` on the old and new kernels; sum per-PC counts over
the libc.so text range per cell (driver `/tmp/opencode/clt_mx.py`,
`kernel_compare.run_workload` per cell). Running a copy rather than
`/usr/bin/tcc` is required: the installed path hits the yasld template cache
(43 templates in `rootfs/lib/yasld.cache`) and adds ~11.8M unrelated work.

To show the allocation stream is identical, run both binaries with `-bench`
on the same input/kernel: the alloc-site table and totals match exactly and
the `.o` outputs are byte-identical.

## Root cause

`libc.so` malloc is a first-fit allocator whose per-call scan cost is sensitive
to the heap's free-block structure. What makes that structure vary is **not**
the tcc edits: the heap state itself varies from boot to boot even with the
same kernel and same inputs (the Statistical-noise section quantifies it), so
the battery's +4.0M pair-difference is boot noise, not a binary or kernel
effect. The image-size and provisioning shifts described below are plausible
*contributors* to the same spread, but single-sample cell magnitudes cannot
rank contributors against a ~2-3M noise band. What the evidence establishes
robustly is the mechanism: identical call counts, identical allocation
streams, and a userspace spread that lands almost entirely in the scan loop.

- **Heap-state sensitivity (established):** the allocation stream is
  identical (byte-identical `.o`, matching `-bench` alloc-site tables,
  identical 44,670-call entry counts across matrix cells), the libc code is
  identical, and the whole userspace spread lands in the malloc scan loop.
  Per-boot heap state (the initial free-block layout after loader and process
  startup) decides scan lengths.
- **Candidate contributors (not separable at this measurement quality):**
  the native tcc image grew +3,896 bytes (YAFF size field
  `0x00193030` -> `0x00193F30`), which shifts the heap base after the image;
  the new kernel's memory layout and process-heap provisioning paths
  (`ProcessMemoryPool.reserve_pages` +16.2k, `sys_mremap` +15.9k kernel
  regressions in the battery) change heap growth granularity/alignment.
  `block_copy_init`'s table-image fix does not change tcc's allocation sizes
  (the `-bench` profiles match); its only possible contribution is via
  aggregate image growth.

## Regression lock

None for these performance shapes (allocator behavior, not compiler output).

## Likely fix

- Make libc malloc's free list size-classed (bucketed) or add a top-chunk
  fast path so per-call cost stops depending on heap alignment; this also
  collapses the +/-2-3M per-boot noise band, which is the real measurement
  blocker for any userspace comparison on this workload.
- Kernel side: keep per-process heap provisioning granularity/alignment
  stable so allocator behavior does not track image size or kernel layout.
- Measurement protocol: compare >=5-rep means (stdev ~2.2-2.7M userspace for
  this workload); do not chase this in tcc codegen — the new binary is not
  measurably worse and kernel-only comparisons are already tight.
Provenance: the 2026-10-11 battery index (`kernel-bottlenecks-2026-10-11.md`, removed 2026-10-13; `git show 684a68ca:docs/bugs/kernel-bottlenecks-2026-10-11.md`).

## Status 2026-10-14

A libc change that caches an active pool's failed searches (measured libc
work -22%) sits uncommitted in the `libs/libc` working tree (`malloc.c`,
`tests/malloc_tests.c`). Successful first-fit scans still depend on the heap
layout, so the lever above stays open.

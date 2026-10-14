# TCC-vs-LLVM kernel inefficiencies — 2026-10-12 battery

**Status:** open performance investigation index (2026-10-13 fixes recorded below) · **Found:** 2026-10-12
(20:45 paired kernel_compare battery, per-PC attribution, LLVM inline frames,
disassembly, reduced reproducers through the current cross)

## Scope and provenance

- Run: `.cache/kernel_compare_current_20261012_2045/` in the enclosing YasOS
  checkout (`results-20261012-204437.json`, `insns/`, `bottlenecks/`).
- YasOS `16f00e9c` plus dirty `process_memory_pool.zig` (the pool-hint fix).
- TinyCC `175aaef3` plus working tree; the cross was built 20:44, after every
  dirty source.
- Kernel C: `.zig-cache/o/a92895f5d98b120da02d78e7ec8a01eb/kernel.c` (10-11
  22:24). Recompiling it reproduces the ELF bodies:
  ```
  libs/tinycc/bin/armv8m-tcc -O2 -fno-pic -ffunction-sections -fdata-sections \
    -DYASLIBC_REAL_BOOL -mfloat-abi=hard -mfpu=fpv5-sp-d16 -fvisibility=hidden \
    -minline-atomics -include apps/zig/zig_c_prelude.h -I ~/.zvm/master/lib -c kernel.c
  ```
  Omitting `-minline-atomics` (`build/kernel_tcc.zig`) turns every CAS into a
  `__atomic_compare_exchange_4` call; everything else is unaffected.

All 15 workloads match return codes and outputs. Kernel-only instructions
(PCs `0x1000_0000..0x2000_0000`): **26,957,124 TCC versus 20,435,550 LLVM
(1.319×, gap 6.52M)**, down from 1.463× at 10-11 15:38. These are guest
instruction counts, not board cycles; use `scripts/board_model.py` for cycles.

| Workload | TCC | LLVM | Ratio |
|---|---:|---:|---:|
| `zig-obj-tcc` | 13,739,627 | 10,372,855 | 1.325× |
| `core-sort` | 2,923,770 | 2,371,702 | 1.233× |
| `tcc-hello-run` | 1,800,228 | 1,334,089 | 1.349× |
| `ls-bin` | 1,781,244 | 1,375,412 | 1.295× |
| `tcc-bench-c-O2` | 1,615,609 | 1,191,676 | 1.356× |
| `tcc-hello-c-O0` / `-O2` | 1,488,246 / 1,481,054 | 1,107,151 / 1,110,473 | 1.34× |
| `ps` | 634,520 | 432,571 | 1.467× |
| `zig-version` | 600,335 | 465,993 | 1.288× |
| `core-grep` | 459,049 | 354,948 | 1.293× |
| `uname`, `true-launch`, `sha256sum`, `echo`, `noop` | 433,442 | 318,680 | 1.36× |

## Method and attribution corrections

Each family was compared per call: TCC physical instructions over TCC entry
counts, against LLVM's **inclusive** count (every PC whose inline stack
contains the source function). The difference is an upper bound on
attributable excess, not a proven saving, and projected savings across
reports overlap. Name pairing corrected several large rows from the raw
per-symbol ranking:

- **RankedMutex lock/unlock are at parity** (49.0 vs 52.4 and 73.9 vs 72.0
  per call). Five identical TCC mutex instantiations are folded to one address
  and were all counted under `RankedMutex(.mount)`.
- **`FileHeader.load`'s +268k since 10-11 15:38 is not a regression.** The new
  `inline:sret_loop` tier inlines `mem_findScalarPos` (previously 1.18M
  out of line) into it. The path as a whole got cheaper by about 330k
  (estimate); comparing kernel totals avoids the row shift.
- **`parser_names_end`'s +88k is the same effect** (the scan is now inlined;
  `names_end` plus the scan fell 345k → 242k). `load_from_template`'s +30k *is*
  a compiler change (identical kernel C), filed below.
- **The timestamp family is at parity**: TCC's inline reciprocal division in
  `time_realtime_us`/`time_now_timespec` (418k) versus LLVM's bodies plus
  `__udivmoddi4`/EABI wrappers (423k).
- **`MallocAllocator`, `get_free_fd`, `is_privileged`, `irq_svcall` stub:**
  parity or TCC ahead once LLVM's renamed and inlined bodies are paired.
- **`memcpy`/`memset`**: same C in both kernels. memcpy is at parity inclusive;
  memset's +182k is LLVM unrolling the 128-byte loop 4× further (a policy
  choice, not filed).
- **`gen_vcall` thunks** carry whole inlined VFS method bodies; matched by
  call counts the net is +48k of VFS work, not dispatch glue.

## Current state (after the 2026-10-14 fixes)

**2026-10-14 round 4** (`merge/lc-2026-10-14-7`: const_local_table var-dest
aliasing, indexed struct-arg derefs, dead volatile trails, branch threading
beside `asm goto`, CAS leftovers, component iterator; base
`fix-base-2026-10-14` measured 24,118,198 in the same session): kernel-only
**24,273,837 TCC versus 20,435,550 LLVM (1.188×)**, all 15 outputs and return
codes matching. The +155k against the base is inside the battery's load noise
(the movers are `reserve_pages` and `memset`, whose code is unchanged); kernel
`.text` grew 392 bytes. Runs: `.cache/kc4_base4`, `.cache/kc4_merge7_ci`.

**2026-10-13 round 3** (`merge/lc-2026-10-13c`: inline CAS loop exits,
bitmap helpers inlined, free-run probe, path iterator; base
`fix-base-2026-10-13c` measured 24,363,401 in the same session): kernel-only
**24,100,190 TCC versus 20,435,550 LLVM (1.179×)**, all 15 outputs and return
codes matching; QEMU self-host stage-3 fixpoint passes. Runs: `.cache/kc3_base`,
`.cache/kc3_merge`.

`local-combined-2026-10-06` @684a68ca, same battery protocol and kernel C:
kernel-only **24,594,683 TCC versus 20,435,550 LLVM (1.204×)** after the second round (2026-10-13 evening, `merge/lc-2026-10-13b`; the first round gave 24,840,075 / 1.216×), all 15
outputs and return codes matching; QEMU self-host stage-3 fixpoint passes.
Baseline runs for further comparisons (enclosing checkout): `.cache/kc_fixbase_20261012`
(before, 26,924,347) and `.cache/kc_merge_20261013` (after). What the fixed
reports were is in git history (`git log -- docs/bugs`).

## Reports (2026-10-12, open after 2026-10-13)

Correctness first, then performance by the original projected saving
(estimates from the 10-12 run; the 2026-10-13 fixes took part of several).

| Report | Evidence (TCC vs LLVM incl, 10-12) | State |
|---|---|---|
| `ptr_local_fwd` ignores `asm goto` edges | **miscompile** with `asm goto`, pre-existing | fixed 2026-10-14 (asm goto labels are CFG edges; report deleted) |
| [uaccess access_ok out of line + bool tail](kernel-uaccess-access-ok-outlined-bool-tail.md) | 33.7 vs 20.6 /call, +309k callee, ≈+94k callers | partly fixed (write variant inlines) |
| [mem.eql one outlined generic body](kernel-mem-eql-outlined-generic-equality.md) | 31.0 vs ~9.3 /call over 26,837 calls, ≤+582k | partly fixed (body; call boundary open) |
| [ROMFS FileHeader return copies + deinit](kernel-romfs-fileheader-return-copies.md) | lookup side 1.21M vs 766k; 8.7 header copies per lookup | partly fixed (`return try f()`) |
| [ROMFS load name scan + reader tail](kernel-romfs-load-name-scan-and-reader-tail.md) | 137.3 vs 88.9 /call, +545k | partly fixed (scan) |
| free-run by-value field load not hoisted | `reserve_pages` +156k; 13 vs 10 /probe | fixed (masks hoist, bit-test shift, branch to epilogue, orphaned jump) |
| [bitmap helpers: `setRangeValue` double RMW, `?T` returns](kernel-bitmap-helpers-out-of-line-overhead.md) | TmpMemoryPool family +226k; mremap `isSet` +153k | partly fixed (`isSet`, `next_bit`, `setRangeValue` inline) |
| [path iterator: `get_file_header` `cbnz`, `last`, `FileHeader.deinit`](kernel-componentiterator-marshalling-blocks-inlining.md) | 308k vs 198k, +110k | partly fixed (sizing; `next` scans) |
| [u64 zero-high shift](kernel-u64-zero-high-shift.md) | u16 map `getIndex` 123.8 vs 78.6 /call | partly fixed |
| [validate reused copy-local round-trip](kernel-validate-reused-copy-local-roundtrip.md) | relocation loop 30 vs 19 /iter, +137k | open (fix held) |
| [noreturn call keeps fall-through edge](kernel-noreturn-call-cfg-fallthrough.md) | `kheap_unlock` 13.5 vs 9.0 /call | partly fixed (LR save open) |
| inline CAS re-test of a kept value | `clrex` 15,126 vs 34; `dmb` 14,381 vs 17 | fixed (5.0 insns per CAS, LLVM's; kept value decided per edge, store status `cbz`); left: [dead `expected` initial move](cas-expected-dead-initial-move.md) |
| [template replay](kernel-template-replay-invariant-spill-reload.md) | ≈52k over LLVM after the split fix | open (rewritten) |

Still open from earlier: [Wyhash pair repacking](kernel-wyhash-pair-repacking-spills.md)
(also the 21 register `mov`s per u16 map lookup) and the userspace
[malloc free-list scan](malloc-free-list-scan-heap-base-shift.md).

## Cross-cutting themes

1. **Inliner size accounting.** `access_ok`, `mem.eql`, `ComponentIterator.last`,
   `FileHeader.deinit` and `appendSliceAssumeCapacity` stay out of line (the
   bitmap helpers inline since round 3). Their op counts (24/32/12
   limits in `backend/generators/function.c`) are inflated by Zig C-backend
   plumbing that folds away after inlining: by-value aggregate copies, sret
   stores, `0xaaaaaaaa` undefined fills, bool tails and NOP slots. Counting only
   real ops would admit several of them.
2. **Aggregate round-trips through the stack.** Return copies (`FileHeader`),
   multi-def copy locals (`validate`), split by-value parameters, and 64-bit
   fields stored wide and read narrow (`load` reader tail).
3. **LICM gaps for loads.** No dominance-based load hoisting; the field-pointer
   hoist now covers by-value copies and respects zero-trip guards.
4. **No shrink-wrapping.** Early exits pay full prologues (`mem.eql`,
   `kheap_unlock`); the remaining dead-frame cases are in the bitmap report.

## Open questions

- `get_free_fd`'s own loop (39.7 vs 22.5 per iteration) and the `gen_vcall`
  slice getter (7 vs 3 instructions, ≈6.7k) are measured but not filed.

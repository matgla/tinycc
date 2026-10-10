# Remaining TCC kernel bottlenecks — current October 9 benchmark

**Status:** open performance investigations · **Found:** 2026-10-09, saved A/B profiles and disassembly

## What the remaining gap means

Warm process launch uses **133,620 TCC versus 99,163 LLVM instructions**,
averaged across six boots and 300 measured launches per kernel: **1.347×**.
The larger compile workloads are closer end to end because they execute mostly
the identical userspace binaries. Their kernel-flash work is still about
**1.81×** LLVM's. There is meaningful kernel work left even where the total
ratio is 1.02–1.03×.

Both kernels are `ReleaseFast`; the TCC build uses `-O2`. Rootfs and userspace
are fixed across variants. These are guest instruction counts, not board
cycles. All 14 supported workloads passed with matching checked outputs;
`ps` checks status only. No correctness defect was found in this analysis.

## Priorities

| Priority | Remaining mechanism | Current evidence | Report |
| --- | --- | --- | --- |
| P1, compile workloads | Scalar byte-copy loop overhead | Zig chain: `memcpy` 15,875,579 vs 9,065,057; 13,302,945 TCC instructions in the byte fallback | [Byte copy](kernel-memcpy-byte-loop-overhead.md) |
| P1, compiler quality | Streaming Wyhash retains eight-byte copies and assembly | Frozen small compile: streaming Wyhash 297,464 vs 38,407; equality byte-buffer issue fixed in `858bd8c3` | [Streaming hash](kernel-streaming-wyhash-byte-assembly.md) |
| P1, warm launches | Thunk replay | FIXED 2026-10-10: explicit 16-byte contract and direct pair replay; loop 8,648 → 2,538 instructions, scalar SP accesses 1,692 → 94; whole TCC warm gate −7.36% against a fresh before build | [Fix and validation](../../../../docs/loader_thunk_replay_fix_2026-10-10.md) |
| P1, large binary launches | Image verification hash: streaming Wyhash plus byte-loop memmove helper | `zig-version` launch: pipeline 631,367 vs 64,352, 79% of that workload's 795,096 gap (2026-10-09 23:38 battery) | [Streaming hash](kernel-streaming-wyhash-byte-assembly.md) |
| P2, allocation | Page-at-a-time bitmap mutation and count updates | FIXED 2026-10-09: word-at-a-time `markRange`, mark+free 8,219 → 1,114 instructions/launch (−86%); report deleted with the fix | — |
| P2, file-heavy workloads | Repeated ROMFS header reconstruction through interfaces and by-value arguments | Kernel side FIXED 2026-10-09 (mapped XIP header reads, no lock/seek/vtable, ls-bin −14.9% tcc / −14.0% llvm); the remaining compiler half is the argument shuffle and `findScalarPos` needle reload | [ROMFS header scans](romfs-header-scan-costs.md) |

Priorities are workload-specific. The copying gap is the largest measured
single-function difference in the Zig chain; it is nearly absent in warm
launches. Thunk replay is the clearer warm-launch target. These rows are not
additive predicted savings: helpers can call one another, compiler inlining
changes function boundaries, and a profile identifies cost rather than proving
that all of it can be eliminated.

## What changed since the earlier reports

The data replay loop now keeps its carriers in registers: **11,802 instructions
and zero direct SP accesses**, versus the earlier 14,330 and 2,528 SP loads.
The GOT loop is also free of direct SP accesses, at 9,738 instructions. Do not
reopen the old data-loop allocation report based on its old counts.

`Parser.create` is now **1,065 vs 546** exclusive instructions per warm launch,
well below the earlier 6,672-vs-2,595 result. General integer-key hashing has
already been replaced by the specialized context. The hash report here concerns
the remaining **streaming** byte-input path, not that fixed integer-key issue.

Explicit scalar SP loads/stores across a warm launch are now **6,878.52 vs
4,180.49**, or **1.65×**; total explicit SP transfer bytes are **71,057.64 vs
43,301.32**, or **1.64×**. These count direct SP accesses, including necessary
homes and ABI traffic; they are neither a spill count nor peak stack usage.
Accesses through SP-derived pointers are outside the scalar census. The byte
helper report documents such derived-pointer traffic separately.

`memset` remains secondary: **9,610.57 vs 7,539.82** instructions per warm
launch. Warm `memcpy` is already at parity: **14,029.62 vs 14,107.84**.
There is no evidence here for another blanket rewrite of all memory helpers.

## Evidence and reproduction

The [benchmark report](../../../../docs/kernel_benchmark_current_2026-10-09.md) records build
identity and methodology. Artifacts are in
`.cache/kernel_benchmark_20261009_current/`; the analysis uses its frozen ELFs,
not a rebuild of a moving working tree.

- TCC ELF SHA256: `53f6f4ad71bf2ffeb4e752fdff2064645c7d97a50c20a28342168172a73a8122`.
- LLVM ELF SHA256: `e381021c9e1611c75904af5af4fa409ad2582d776aaea0d7782c52fdc819794f`.
- `analysis/warm-summary.json`: six-boot average exclusive function counts and SP census.
- `analysis/{tcc,llvm}-{tcc-hello-c-O2,zig-obj-tcc}.json`: compile-workload kernel census.
- `analysis/regions.json`: measured instruction ranges, with exclusive end addresses.
- `analysis/{tcc,llvm}.s`: disassembly matching the frozen kernels.
- `analysis/kernel.c` and `analysis/armv8m-tcc`: saved generated kernel C and
  the cross used for reproduction; `analysis/evidence-manifest.json` fingerprints
  the analysis files. Cross and kernel hashes still matched the benchmark
  manifest after this analysis.
- `analysis/byte_helpers_repro.{c,o,s}` and `byte-copy.{o,s}`: freshly compiled reproductions.
- `analysis/eql-reduced.s` and `pool-reduced.s`: old C reductions optimize successfully; they do not cover all remaining full-kernel shapes.

For example, reproduce one warm census with the existing analyzer:

```bash
.qemu_smoke_venv/bin/python scripts/tcc_stack_profile.py \
  .cache/kernel_benchmark_20261009_current/tcc/bin/yasos_kernel \
  .cache/kernel_benchmark_20261009_current/insns/tcc-warm-1.csv \
  --repetitions 50 --top 30
```

Repeat for all six samples and both variants before comparing averages. A
single-boot compile profile uses `--repetitions 1 --start 0x10000000 --end
0x10100000`. Counts inside a symbol exclude its callees; compare whole gates
after changes, since LLVM often inlines work that TCC leaves in helpers.

No kernel/compiler optimization was implemented in this analysis. Pass-level
causes not demonstrated by the evidence are explicitly left as investigations
in the individual reports. Validate future changes with the current workload
battery, the relevant semantic regressions and the board timing model before
claiming a board-speed improvement.

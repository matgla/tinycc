# TCC -O2 vs Zig/LLVM kernel: rebenchmark and bottleneck reports — 2026-10-09 (afternoon)

A repeat of the morning recheck (`docs/kernel_performance_recheck_2026-10-09.md`)
against the unchanged tree, followed by a pass-level root-causing of the top
bottlenecks. The compiler inputs are **byte-identical** to the 11:06 snapshot
(verified against its `manifest.json` source hashes; only a docs file and one
ir_test differ), so the pinned ELFs were reused and the numbers reproduce —
the new content here is the root-cause work and the six defect reports filed
from it:

- ra-evicts-loop-invariant-carriers — data-replay loop reloads (biggest single win) **fixed 2026-10-09 in `8395ce4c`** (`ra:loop_split` entry-copy ordering; pass now default-on, verified by a 401-seed `diff_olevels` sweep; report closed, git history keeps it)
- thunk-replay loop, invariant divisor / layout reloads — [`loop-invariant-params-reloaded-per-iteration.md`](loop-invariant-params-reloaded-per-iteration.md) **fixed 2026-10-09** (late `ra:licm` + `stack_param_promote`; report closed, git history keeps it)
- CAS operand stack round trip on every `__atomic_compare_exchange` — [`atomic-cas-desired-round-trips-through-stack.md`](atomic-cas-desired-round-trips-through-stack.md) **fixed in `a18f690d`** (`atomic_ptr_arg_inline`: `&local` stays unmarked; report closed, git history keeps it)
- `mem.eqlBytes` 6,090 insns/launch — [`byte-assembled-word-compare-round-trips-through-stack.md`](byte-assembled-word-compare-round-trips-through-stack.md) **fixed in `f17818b9`** (store-load forwarding re-run after the RA unroll cascade; report closed, git history keeps it)
- [`pool-mark-per-page-invariant-reloads.md`](pool-mark-per-page-invariant-reloads.md) — allocator bitmap helpers, 16,102 insns/launch (10.4%)
- Fixed-size wyhash wrappers — **fixed kernel-side 2026-10-09** with a bit-identical scalar integer-map context; general hash/init calls 34.94 → 2 per warm launch (strings remain), −5.14% whole-launch guest instructions, +384 bytes text. Regression: YasOS `dynamic_loader/source/integer_hash_map.zig` and `dynamic_loader/tests/test_integer_hash_codegen.py`; report removed.

## Inputs

- YasOS `a8cb6be6`, TinyCC `dc20e3c5` + working tree == the 11:06 recheck
  snapshot (manifest hash check: no compiler source changed).
- TCC kernel sha256 `4f52704b…`, LLVM kernel `a7584506…`, rootfs `101eece5…`,
  private cross `a326462c…` — all identical to the morning artifacts; reused,
  not rebuilt.
- Work dir: `.cache/kernel_rebench_20261009b/` (battery results
  `results-20261009-115406.json`, warm census, `summary.json`).

## A/B workload battery (one fresh boot per workload, both CPUs)

All 15 workloads rc=0 with matching outputs on both kernels (`zig version`
returns the known-unsupported 255 on both); zero baseline failures. Numbers
match the morning run within scheduling variation.

| Workload | TCC | LLVM | Ratio |
| --- | ---: | ---: | ---: |
| noop (harness) | 73,220 | 72,455 | 1.01× |
| `/bin/true` launch | 326,778 | 255,511 | 1.28× |
| tcc hello -O0 / -O2 | 6,431,498 / 6,884,585 | 5,211,757 / 5,672,236 | 1.23× / 1.21× |
| tcc bench.c -O2 | 60,891,647 | 59,583,374 | 1.02× |
| tcc compile+run hello | 7,578,030 | 6,042,413 | 1.25× |
| sort / grep corpus | 21,517,241 / 7,503,131 | 19,405,321 / 7,192,355 | 1.11× / 1.04× |
| zig render→C→tcc | 559,297,202 | 539,740,719 | 1.04× |

Against the stored 07:38 baseline the TCC arm is 13-32% better on the
launch/syscall-heavy workloads (the `ra:exit_sink` + read-only-wrapper + bump
commits' effect landing in the pinned build), while LLVM moved ≤2%.

## Warm-launch census (6 alternating boots × 50 measured launches each)

| Per launch | TCC | LLVM | Ratio |
| --- | ---: | ---: | ---: |
| Both-CPU instructions | 154,432 | 109,078 | 1.416× |
| Scalar SP loads + stores | 10,003 | 3,690 | 2.71× |
| SP transfer bytes (all forms) | 85,048 | 45,365 | 1.88× |

Compile matrix: all 8 objects (hello/scopes/inline/bitops × O0/O2)
byte-identical across the two kernels.

## Where the 45k-instruction gap lives (TCC kernel, per launch)

| Function | insns | share | defect report |
| --- | ---: | ---: | --- |
| `Loader.load_from_template` (replay) | 34,252 | 22.2% | data loop → RA eviction report; thunk loop → param-homing report |
| `memcpy` | 14,177 | 9.2% | parity with LLVM (14,100) |
| `memset` | 9,826 | 6.4% | head/tail byte alignment vs LLVM's unaligned words (minor; see below) |
| sentinel scanning | 9,115 | 5.9% | call-count parity with LLVM (267 vs 264) |
| pool reserve/mark-used/mark-free | 16,102 | 10.4% | per-page invariant reloads — [pool report](pool-mark-per-page-invariant-reloads.md); lock path also pays the [CAS round trip](atomic-cas-desired-round-trips-through-stack.md) |
| `Parser.create` | 6,672 | 4.3% | RA-spill family (see report 1's note); 993 vs 201 SP accesses |
| `mem_eql` (eqlBytes inlined) | 6,090 | 3.9% | byte-assembly report |
| wyhash init+hash | 6,560 | 4.2% | 35.4 hashes + 35.4 state-copying inits in this snapshot; fixed kernel-side for integer keys on 2026-10-09 |
| kheap lock/unlock | 5,659 | 3.7% | CAS report + call frequency |

The three replay loops alone: data 14,330 (2,528 SP loads), GOT 9,708 (0),
thunk 8,370 (1,488). Post-opt IR of the function keeps all loop values in
virtual registers — the data/GOT-loop difference is made entirely in register
allocation, and the morning report's open question ("alias reasoning or
register allocation?") is answered: **register allocation**.

## Remaining items not filed as defects

- **memset** (9,826 vs 7,819): the byte head/tail is a deliberate libc
  policy — `libs/libc/string.c` keeps mismatched alignment bytewise because
  "tcc's codegen is not audited for" unaligned LDR/STR, and the 64-byte
  `strd` main loop is at parity (cf. `memcpy` 14,177 vs 14,100). A probe
  (`repro/`-style `aligned(1)` member access) shows tcc **already** emits
  single unaligned `ldr`/`str` for direct align(1) accesses, identical to
  gcc — so the policy's audit may be closer than the comment suggests, but
  the residual 2,007-instruction delta has no single named mechanism and
  needs per-site profiling before a defect can be filed. Small upside
  (1.6% of launch); revisit with the board model.
- **Parser.create** (6,672 vs 2,595; 993 vs 201 SP accesses): same
  RA-spill mechanism as the eviction report (post-opt IR nearly clean);
  tracked there rather than as a separate file until a deterministic
  reproducer exists.

## Reproduction

```bash
# battery (ELFs pinned by hash; see work dir)
.qemu_smoke_venv/bin/python scripts/kernel_compare.py --skip-build \
  --work .cache/kernel_rebench_20261009b --jobs 8 \
  --baseline .cache/kernel_compare/baseline.json
# warm census + matrix + attribution (summarize.py's manifest step needs the
# morning manifest; summary.json is written before it fails — harmless)
cd .cache/kernel_rebench_20261009b && ../../.qemu_smoke_venv/bin/python warm.py
# per-defect repros: docs/bugs/repro/*_2026-10-09.c with the pinned cross
.cache/kernel_recheck_20261009/tinycc/armv8m-tcc -c -O2 -minline-atomics ...
```

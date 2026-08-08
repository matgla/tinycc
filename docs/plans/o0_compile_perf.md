# -O0 Compile-Time Plan

**Status**: Phases 0-3 LANDED (§6, §7, §8). Phase 4 **REJECTED on
measurement** (§9) — it is a real 20% win that miscompiles, and the knob was
removed rather than left alive. Phase 5 not started (needs device data).
Phase 1 includes step 3(b), semi-NCA dominators, taken on measurement.
Phase 2 landed **host-only**: the device half was refuted (tcc does not
inline, so it is +112 KB `.text` for zero fewer calls — §7).
**Created**: 2026-08-07 (baseline measured pre-restructure at `11552e45`; paths
below use the post-restructure `source/` layout, same file contents)
**Scope**: the -O0 pipeline — tccgen-time gen passes, `tcc_ir_ssa_regalloc`, codegen; plus one algorithmic fix in `source/ir/cfg.c` that helps every -O level
**Goal**: cut typical -O0 compile-body time by ~20-30%, and fix a superlinear
CFG-analysis blowup that makes some machine-generated TUs ~100× slower than
their size warrants (and un-compilable on the RP2350 for memory reasons).

-O0 is the default when no `-O` is given (`libtcc.c` zero-inits `optimize`),
so this is the path every plain `tcc foo.c` on the device takes. It is the
level that explicitly trades code quality for compile speed — the rehearsal
skip at `source/ir/codegen.c:2719` already made that trade for codegen — but the rest
of the pipeline has not been priced at -O0 yet.

Sibling doc: `opt_pass_dedup_and_perf.md` covers the optimizer's own cost at
-O1/-O2. This plan deliberately does not overlap it: everything here is either
-O0-specific or (§4.1) a shared-infrastructure fix measured through its -O0
impact.

---

## 1. Measured baseline

Host x86_64, **non-ASAN** cross (`./configure --disable-asan && make cross`),
tree at `enableVfp@11552e45` (dirty). Corpus: first 300 files of
`tests/gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/compile/`
(286 compile), the same corpus as `opt_pass_dedup_and_perf.md`.

```bash
ls tests/.../gcc.c-torture/compile/*.c | head -300 > corpus.txt
while read f; do ./armv8m-tcc -O0 -c "$f" -o /dev/null; done < corpus.txt
```

### 1.1 Wall clock (300 invocations, warm cache, 3 reps within ±3%)

| Level | Wall | Per file |
|-------|------|----------|
| `-O0` | 0.51 s | 1.70 ms |
| `-O1` | 0.98 s | 3.27 ms |
| `-O2` | 1.04 s | 3.47 ms |
| empty.c ×300 (process floor) | 0.14 s | 0.47 ms |

So the -O0 **compile body** is ~0.37 s ≈ 1.2 ms/file, and -O0 is only ~2×
faster than -O2. For a compiler whose -O0 contract is "fast, dumb code",
that ratio is low — the sections below show where the other half goes.

### 1.2 PASS_TIME aggregate at -O0 (whole corpus)

```bash
while read f; do TCC_PASS_TIMING=1 ./armv8m-tcc -O0 -c "$f" -o /dev/null; done < corpus.txt \
  | awk '/^PASS_TIME/ {s[$2]+=$3} END {for (p in s) print s[p], p}' | sort -rn
```

| Timed block | Self µs | Calls | Note |
|---|---|---|---|
| `ra2:cfg_ssa` | 164,356 | 364 | CFG + dominators + frontiers + promote + SSA construct/rename |
| `cg:emit` | 10,032 | 364 | |
| `ra2:ssaopt` | 4,332 | 354 | DCE-only at -O0 |
| `ra2:intervals` | 3,210 | 354 | |
| `ra:stack_layout` | 2,791 | 364 | |
| `ra2:finish` / `ra2:phis_folds` / `ra2:scan` | ~6,200 | | |
| `cg:dry` | 1,515 | 66 | pass-0 discovery; rehearsal already skipped at -O0 |

**`ra2:cfg_ssa` is 84% of all timed pass time at -O0 — and 153 ms of its
164 ms is a single file**, `20001226-1.c`. Excluding that file, the whole
regalloc block is ~27 ms ≈ 14% of the typical compile body; the untimed
majority (~75%) is preprocessing, parse+IR build, output write, and
allocator traffic.

### 1.3 The pathological file (and why it is not just a curiosity)

`20001226-1.c` macro-expands to one function with 8,192 `if (…) goto`
statements: ~4.8k basic blocks, and two labels (`gt:`/`lt:`) with thousands of
predecessors each. Callgrind at -O0 (1.85 **G** instructions total, vs
~10 M for a normal 22 KB TU):

| Function | Ir share |
|---|---|
| `tcc_ir_cfg_compute_dom_frontiers` | **38.0%** |
| `tcc_ir_cfg_compute_dominators` | **25.5%** |
| everything else | 36.5% |

Two independent quadratic behaviours, both keyed on the same shape — a
many-predecessor join block over a deep dominator-tree chain:

* **Frontier runner walk** (`source/ir/cfg.c` (runner walk)): for each pred of a join
  block `b`, a runner climbs the idom chain to `idom(b)`. With 8k preds
  spread along an O(n)-deep chain, walks re-cover the same suffix over and
  over: O(n²) steps. The `df_seen` dedup bitset already *detects* the repeat
  (`cfg_add_df` returns early) but the walk keeps climbing anyway.
* **CHK dominator iteration** (`source/ir/cfg.c` (old CHK fixpoint)): once the join's
  `new_idom` converges near the entry, every remaining `cfg_intersect`
  against a deep predecessor climbs that predecessor's whole chain: O(n²)
  per fixpoint iteration.
* **`df_seen` was O(n²/8) bytes** (`nb * ceil(nb/8)`). At this file's ~4.8k
  blocks that is a measured **2.9 MB zeroed allocation per function**
  (host max-RSS delta confirms it); the curve is quadratic, so a 4×-bigger
  function costs 46 MB — past what the RP2350 (8 MB PSRAM) can allocate at
  any -O level. On PSRAM, zeroing costs the bytes cleared (55 MB/s), so
  even the sub-OOM sizes cost tens of ms per function on the device.

The synthetic file is extreme, but the shape is ordinary: `goto fail`-style
error handling, lexer/parser tables, protobuf-generated code, and switch
ladders all produce many-pred join blocks. The cost curve is quadratic well
before it is visible, and this is shared infrastructure — `-O1/-O2` pay the
same blowup (`ra2:cfg_ssa` is 165 ms there too, same file).

Dominators are additionally computed a **second** time per function at -O0
by `ra_refine_live_regs_accurate` (`source/ir/regalloc.c:3840`) — which only needs
the RPO ordering the computation happens to populate, not dominators at all.

### 1.4 Typical-TU profile (callgrind, 22 KB torture file, -O0)

| Bucket | Ir share | Functions |
|---|---|---|
| IR operand accessors | **~11.8%** | `irop_get_vreg` 4.9%, `tcc_ir_op_get_dest` 2.6%, `_src1` 2.5%, `_src2` 1.8% |
| Codegen emit | ~9% | `tcc_ir_codegen_generate` 3.0%, `thop_emit` 2.2%, `ot` 1.5%, `machine_op_from_ir` 1.3% |
| Preprocessor/lexer | ~6.6% | `next_nomacro` (+`'2`) 4.8%, `next` 1.8% |
| memset (zeroed allocs) | ~4.8% | |
| Interval build | ~4.7% | `ra_build_intervals` |
| SSA construct+rename | ~3.2% | |
| Live-reg refinement | ~2.9% | `ra_refine_live_regs_accurate` |
| IR build | ~3.4% | `svalue_to_iroperand` 2.2%, `tcc_ir_put` 1.1% |
| malloc/free | ~2.5% | |
| imm-cache invalidation | ~1.5% | `tcc_gen_machine_imm_cache_invalidate_live` |
| DU-chain builds at -O0 | ~1.6% | `ir_opt_du_build_mode` (from ungated `pack64*`, §4.4) |

The accessors are the single largest bucket: they live in `tccir_operand.c`,
a separate TU, so every operand decode in every phase is an outlined call.

### 1.5 Per-invocation floor (empty.c, host 0.47 ms)

`tccpp_new` 13.4%, dynamic-linker relocation ~12% (host-specific), memset
6.9%, predefine parsing (`parse_define`+`sym_push`+`tok_alloc`) ~6%,
malloc ~3%. On the device the floor is the already-optimized ~25.8 ms
(init-stamp work: programmatic predef protos, XIP bracketing); no further
floor work is proposed here — but §4.6 notes the floor is paid **per
invocation**, which build scripts can amortize.

### 1.6 Host-vs-target caveat

All numbers above are host instruction counts. On the RP2350 the ranking
shifts *toward* the memory-bound items (§1.3, §1.4 memset/malloc): PSRAM
zeroing costs bytes, XIP misses cost cache lines. Host profiling has lied
about target pass ranking before (the vrp full-array-clear incident), so
Phase 0 below re-ranks everything on target with `-bench`/PASS_TIME before
any constant-factor work is committed. The algorithmic findings (§1.3) are
asymptotic and transfer regardless.

---

## 2. What -O0 currently pays for, that -O0 does not need

Walking `tcc_ir_ssa_regalloc` (`source/ir/regalloc.c:4969`) at `optimize == 0`:

1. `ssa:mem_init` — deliberate, lowering-adjacent, keep (comment at 4976).
2. CFG build + **dominators + dominance frontiers** (5153-5160) —
   unconditional, even though at -O0 their only consumer is…
3. **variable promotion + SSA construct + rename** (5162-5176) — whose only
   -O0 payoff is DCE-on-phis (`ra2:ssaopt`, comment at 5209: it runs "to
   remove dead phi definitions that could confuse phi resolution") plus
   whatever register-quality benefit promotion gives -O0 code.
4. Phi resolution + folds, interval build, linear scan, stack layout — the
   actual allocator; needed.
5. `ra_refine_live_regs_accurate` — a full CFG + dominator rebuild + bit-matrix
   dataflow to tighten caller-saved-register reuse. Pure code-quality
   refinement, unconditional at -O0.
6. In `gen_function` (before regalloc), `source/opt/function_pipeline.c` runs
   a tail of **ungated** passes at -O0: `compact_nops` ×3, `cmp_expr_fold`,
   `local_addrof_const_fold`, `memmove_to_indexed_stores` ×2, the `pack64`
   family (4 passes, each building DU chains), `shl32_or_chain`,
   `pack64_tautology`, `cmp_narrow_64`, `assign_fuse`, `select` (lines
   77-326). Some of these are required lowering (e.g. SELECT lowering,
   64-bit packing before codegen can encode the ops); others look like pure
   optimizations that predate the -O gating discipline.

There is already a complete non-SSA fallback in the allocator: when no
variable is promotable, `ssa == NULL` and intervals are built from flat IR
(5168-5173). So "skip SSA at -O0" is not new machinery — it is choosing the
existing fallback unconditionally.

---

## 3. Ranked findings

| # | Finding | Typical -O0 body | Worst case | Device amplification |
|---|---------|------------------|-----------|----------------------|
| F1 | Frontier runner doesn't stop on already-seen; `df_seen` is O(n²/8) bytes | ~1% | **38% + OOM** | PSRAM zeroing + quadratic (2.9 MB on the §1.3 file) alloc growth |
| F2 | CHK intersect quadratic on many-pred joins; dominators computed 2×/function at -O0 (once only for RPO) | ~2% | **25%** | same |
| F3 | Cross-TU operand accessors outlined | **~12%** (host Ir; ~7-8% host time) | ~5% | none — tcc does not inline, see §7 |
| F4 | Full SSA machinery for DCE-only benefit at -O0 | ~10-14% | large | per-function array zeroing in PSRAM |
| F5 | Ungated gen-time opt passes at -O0 | ~3-5% | small | + shrinks the -O0 miscompile surface |
| F6 | Per-function zeroed-array churn (intervals, matrices, cfg) | ~5-8% | — | **bytes-not-calls**: PSRAM 55 MB/s |
| F7 | Per-invocation floor in multi-file builds | — | — | 25.8 ms × N files |

On F5's safety note: a disproportionate share of past -O0 miscompiles came
precisely from gen-time passes that ran at -O0 (`cmp_expr_fold`'s
param-entry-def fold, the pack64 `.btype` hazard, `var_tmp_fwd`). Gating
true optimizations out of -O0 is a correctness-surface reduction as much as
a speed win.

---

## 4. Plan

### Phase 0 — target-side baseline + harness (no compiler changes)

* Fix `scripts/opt_profile.py`: the PASS_TIMING aggregation runs at a
  hard-coded -O2 regardless of `--levels` (observed while measuring this
  plan). Make it honor the requested level so -O0 tables are collectable.
* Re-run §1.1/§1.2 on the device via `-bench` (PASS_TIME is wired into
  -bench on target) over the on-device smoke corpus; record the -O0 table
  next to the host one in this doc. Include one many-join-block TU scaled
  to fit device memory (e.g. `C1024` instead of `C4096` in the §1.3
  pattern) to price F1/F2 on target.
* Add a compile-time regression check: corpus -O0 wall + PASS_TIME snapshot
  under `metrics/`, so later phases and unrelated work have a number to
  diff against.

**Verify**: numbers reproduce twice within noise; no tree changes.

### Phase 1 — kill the superlinear CFG work (F1, F2)  [helps all -O levels]

1. **Frontier walk memoization** (`source/ir/cfg.c`): the outer loop visits join
   blocks `b` in increasing order, so per runner block the target `b` is
   monotone — replace the `df_seen` bit-matrix with a per-block
   `last_df_added` **int array** (O(n) memory, O(1) check), and **break the
   walk** when the runner already has `b` recorded: its ancestors up to
   `idom(b)` were filled by the walk that recorded it. Total work becomes
   O(Σ|DF|), and the quadratic matrix becomes one int per block (~19 KB
   on the §1.3 file).
2. **`ra_refine_live_regs_accurate`**: call `cfg_compute_rpo` (exposed, or a
   thin wrapper) instead of `tcc_ir_cfg_compute_dominators` — it consumes
   only `rpo_order`/`rpo_count` (`source/ir/regalloc.c:3956` comment documents the
   same pattern in the coalescer). Halves the dominator cost everywhere the
   refinement runs.
3. **Dominators proper**: after 1+2, re-measure `20001226-1.c`. If CHK is
   still the top cost (expected: ~470 M Ir), either (a) accept and record —
   worst case is now bounded by one CHK fixpoint, or (b) implement semi-NCA
   (LLVM's choice, ~150 lines, O(n·α(n))). Decide on the measurement, not in
   advance; (b) is the only item in this plan that rewrites an algorithm
   rather than trimming one.

**Verify**: dominator trees and frontier sets are semantically identical —
assert by diffing `-dump-ir-passes` golden IR over `tests/ir_tests` at
-O0/-O1/-O2 (must be byte-identical; this phase is output-neutral). Full
host suite (`make test` against the *test* configure, expect 13927/0),
device smoke, and the §1.3 file's compile time before/after (expect ≥5×).
Watch DF *ordering*: the memoized walk must append in the same order, or
phi operand order could shift — golden IR will catch it.

### Phase 2 — inline the hot operand accessors (F3)  [LANDED, host-only — §7]

Move `irop_get_vreg`, `tcc_ir_op_get_dest/src1/src2` into a header as
`static inline`, keeping `tccir_operand.c` as the out-of-line home. The
IROperand encoding is hazard-prone to *write* (the `.btype` overwrite
incident) — this phase only relocates *readers*, no encoding changes.

Outcome: landed **for the host cross only**, gated on `__TINYC__`. The device
half of the idea was measured and refuted — see §7 for the numbers and the
mechanism.

### Phase 3 — audit and gate the -O0 gen-pass tail (F5)

For each ungated pass in §2.6: classify **lowering-required** (codegen
cannot encode the IR without it: `select`, `assign_fuse`, the base `pack64`
lowering, `compact_nops`) vs **optimization** (`pack64_tautology`,
`shl32_or_chain`, `cmp_expr_fold`, `local_addrof_const_fold`,
`memmove_to_indexed_stores`, `cmp_narrow_64`, others found in the audit).
Gate the optimizations behind `optimize >= 1` (or their existing `opt_*`
flags). The classification method is empirical, not archaeological: build
the corpus + ir_tests at -O0 with the pass force-disabled
(`TCC_DISABLE_PASS`) and see whether codegen faults or merely emits more
instructions.

**Verify**: -O0 golden IR *changes* here by design (fewer transforms) — the
gates are the ir_tests device suite at -O0 (must stay green), the gcc
torture *execute* suite at -O0, and object-size spot checks (accept ≤ ~2%
-O0 size growth; -O0 size is not a contract). Host corpus wall at -O1/-O2
must be unchanged.

### Phase 4 — decide the -O0 SSA question (F4)  [experiment first]

Behind an env knob (`TCC_O0_NO_SSA=1`), make -O0 take the existing
non-promotable fallback unconditionally: skip promotion, SSA
construct/rename, DCE, phi resolution — and with Phase 1 landed, skip
dominator+frontier computation entirely when the fallback is taken (they
have no other -O0 consumer; confirm `ra_build_intervals`' flat path only
needs the CFG itself). Then measure both sides of the trade on target:

* compile time (expect the remaining ~10% of typical body, much more on
  join-heavy TUs), and
* -O0 *output* cost: object size + smoke-suite runtime delta (unpromoted
  locals mean more stack traffic).

Accept if compile-time win ≥ ~8% typical body and -O0 runtime cost is
within what the rehearsal-skip precedent priced (-O0 chose compile speed
over quality; a few % runtime is in-contract, 2× is not). Otherwise keep
SSA at -O0 and bank Phases 1-3. Either way, delete or promote the knob —
don't leave a third half-supported -O0 variant alive.

**Verify**: ir_tests + torture execute at -O0 with the knob on (the
fallback path is currently only exercised by all-address-taken functions;
running the whole suite through it is itself a correctness sweep — past
bugs lived exactly in under-exercised allocator paths).

### Phase 5 — allocation churn, only with target data (F6)

If Phase 0's device table ranks memset/alloc high after Phases 1-4: reuse
per-function scratch arenas across functions in `TCCIRState` (grow-only,
sized to high-water), and size bit-matrices to live counts rather than
maxima. Do **not** resurrect SRAM prefer-fast tiering — it was implemented
and deliberately reverted (slow-default penalizes scratch-heavy workloads);
any revival needs that design discussion, not this plan.

### Non-goals / rejected levers

* **Floor work** — already taken to 25.8 ms on device; the remaining lever
  is *amortization*: `tcc -c f1.c f2.c …` compiles N files in one process
  today, so build scripts (self-host driver, rootfs app builds) can batch
  sibling TUs. Worth wiring where trivial; not compiler work (F7).
* **Skipping `cg:dry` (pass 0)** — it is discovery (scratch pushes, LR,
  frame), not modeling; the modeling walk (rehearsal) is already skipped at
  -O0 with the trade documented at `source/ir/codegen.c:2723-2731`. Merging pass 0
  into the real pass is a codegen rewrite with miscompile risk far above
  its ~1.5 ms/corpus value.
* **Preprocessor/lexer micro-opts** (`next_nomacro` 6.6%) — real but flat,
  high-risk-per-percent, and partially covered by past work (token lookup
  cache, lazy builtins). Revisit only if Phase 0's device table promotes it.
* **Caching/PCH schemes** — out of scope; changes semantics and storage
  story on device.

---

## 5. Expected outcome

| Workload | Now | After 1-3 | After 4 (if accepted) |
|---|---|---|---|
| Typical TU, host body | 1.2 ms | ~1.0 ms (−15-20%) | ~0.9 ms (−25-30%) |
| `20001226-1.c` host | ~370 ms | ≤ 90 ms (frontier+RPO fixes; ~40 ms if semi-NCA) | ~25 ms |
| Same-shape TU on device | OOM / tens of seconds | compiles; seconds | ~1 s |
| -O1/-O2 | — | inherit F1-F3 wins | unchanged |

The single highest-value item is Phase 1: it is small (tens of lines),
output-neutral, removes an on-device OOM, and improves every optimization
level's worst case.

---

## 6. Landed results (2026-08-07, Phase 0 local + Phase 1)

Landed on `enableVfp` after the `source/` restructure (`b05fb734` + this work):

* `scripts/opt_profile.py`: `--table-level` now defaults to the last `--levels`
  entry instead of silently profiling -O2.
* `source/ir/cfg.c` — frontier runner walk memoized: per-runner `last_df` int
  array replaces the `nb×(nb/8)` `df_seen` matrix, and the walk breaks when
  the runner already carries the join (its suffix is already recorded).
* `source/ir/cfg.c` — CHK fixpoint replaced by Lengauer-Tarjan semidominators
  (iterative path-compressing eval/link) + semi-NCA idom construction: step
  3(b), taken because after the frontier fix the fixpoint was still 98 ms of
  the worst case's 130 ms. `cfg_intersect` deleted. `tcc_ir_cfg_compute_rpo`
  exposed publicly (frees a stale `rpo_order` on recompute).
* `source/ir/regalloc.c` — `ra_refine_live_regs_accurate` calls
  `tcc_ir_cfg_compute_rpo` instead of the full dominator computation.

### Measured (host x86_64, non-ASAN, same-machine A/B at `b05fb734`)

| Metric | Base | Fixed | Δ |
|---|---|---|---|
| `20001226-1.c` -O0 wall | 0.18 s | **0.03 s** | 6× |
| `20001226-1.c` -O2 wall | 0.45 s | **0.21 s** | 2.1× |
| `20001226-1.c` `ra2:cfg_ssa` self | 151.2 ms | **0.98 ms** | 154× |
| `20001226-1.c` -O0 max RSS | 19.7 MB | 16.0 MB | −3.7 MB |
| 300-file corpus -O0 wall | 0.32 s | **0.17 s** | −47% |
| corpus `ra2:cfg_ssa` total, -O0 | 153.1 ms | 2.8 ms | 55× |
| corpus `ra2:cfg_ssa` total, -O2 | ~165 ms | 2.1 ms | ~79× |

### Verification

* **Byte-identical output**: 966 object pairs (300-file torture corpus +
  `tests/ir_tests`, each at -O0/-O1/-O2) compiled by base and fixed compilers
  — zero byte differences, zero return-code differences. The dominator tree
  is unique, so semi-NCA is held to reproducing CHK's exact output, and does.
* Full host `make test` suite on the ASAN configure: **green** — unit suites
  (2744 tests / 11683 asserts), aeabi, warn/optflag/dsl checks, frontend,
  linker, debug-info, runtime-library, self-host gate, and `ir_tests`
  **13908 passed / 0 failed** (273 skipped, environment-dependent).

With `ra2:cfg_ssa` reduced to noise, the top remaining typical-body items are
the Phase 2 accessor outlining (~12%) and the Phase 3/4 questions, unchanged.

---

## 7. Phase 2 results — host-only inlining, device half refuted

### The device half does not work, and cannot without an inliner change

The four accessors were made `static inline` in the headers and measured
against the unchanged tree, compiling the **device** compiler's own 344 TUs
with the cross tcc (`-O1 -mcpu=cortex-m33 -mthumb -ffunction-sections`,
the self-host gate's flags):

| Device build (tcc compiling tcc) | Out-of-line | Header `static inline` |
|---|---|---|
| total `.text` over 344 TUs | 1,367,921 B | 1,480,305 B (**+112,384, +8.2%**) |
| call-site relocations to the 4 accessors in `source/ir/codegen.c` | 136 | **136 (unchanged)** |
| accessor symbols defined in that object | 0 (shared copies) | 4 local copies |

**tcc inlines none of these calls even with the definition in the same TU.**
It emits one local out-of-line copy per TU and still calls it 136 times, so
the device pays 112 KB of `.text` for exactly zero fewer calls. That is 14×
over this phase's ≤8 KB budget with none of the win, and it independently
re-derives the reason these were de-inlined in the first place (the note at
the head of the out-of-line block in `source/ir/tccir_operand.c`: "179 copies
of `tcc_ir_op_get_src1` alone").

A real device win here needs tcc's **inliner** to handle small same-TU
callees, not the header move. That is a separate piece of work (it would also
pay off across the whole self-hosted build); until then, F3 is a host-side
lever only, and §1.4's ~12% Ir share should not be read as 12% of device time.

### What landed

`#ifndef __TINYC__` sets `TCC_IROP_INLINE_ACCESSORS` (`source/ir/tccir_operand.h`),
which switches the four readers between `static inline` definitions in the
headers (`irop_get_vreg` in `tccir_operand.h`; `tcc_ir_op_get_dest/src1/src2`
in `tccir.h`) and the existing out-of-line definitions in
`source/ir/tccir_operand.c`. gcc/clang builds — the host cross and every
developer build — inline them; the self-hosted armv8m compiler keeps the
shared copies, bit-for-bit as before.

| Metric | Base | Landed | Δ |
|---|---|---|---|
| device `.text`, 344 TUs | 1,367,921 B | **1,367,921 B** | **0** |
| host cross `.text` | 2,083,326 B | 2,586,182 B | +502,856 (host tool, irrelevant) |
| host: `source/ir/codegen.c` ×10 at -O0 | 0.27 s | **0.25 s** | −7.4% |
| host: `source/ir/codegen.c` ×10 at -O2 | 8.01 s | **7.33 s** | −8.4% |
| host: 300-file torture corpus -O0 | 0.17 s | 0.17 s | none (startup-bound) |

The corpus shows nothing because those TUs are ~1-3 KB: after Phase 1 the
whole 300-file run is 0.57 ms/file against a ~0.47 ms process floor, so
almost none of it is compile body. Body-heavy TUs (tcc's own sources, i.e.
the self-host and rootfs workloads) are where this shows up — and where it
matters.

**Verification**: 5,943 object pairs (the full 2,003-file torture compile
corpus + `tests/ir_tests`, each at -O0/-O1/-O2) byte-identical between base
and landed compilers; device `.text` identical to the byte; full host
`make test` green.

### Method note for future phases

Measure host and device separately. The host cross is gcc-built and inlines
normally; the device compiler is tcc-built and does not. A change justified
by host profiling can be neutral-to-harmful on device, and §1.4's Ir shares
are host shares. `scratchpad/devsize.sh`-style measurement (compile every TU
with the cross tcc, sum `size`'s text column) is the cheap device-side check
— it needs no device and no rootfs build.

---

## 8. Phase 3 results — gate the -O0 pass tail

Nine passes ran ungated at -O0 in `source/opt/function_pipeline.c`. All nine
first got a `tcc_ir_opt_pass_disabled("<name>")` knob (the codebase's existing
bisect idiom), which made one build serve the whole audit. Each was then
disabled in turn against the **-O0 suites**: 1,804 gcc-torture *execute*
tests and 2,628 compile/other tests, all at -O0.

| Pass | -O0 suites without it | -O0 `.text` over 286 objects | Verdict |
|---|---|---|---|
| `memmove_to_indexed_stores` (×2 sites) | green | 259,376 (=) | gated `-O1+` |
| `pack64` | green | 259,376 (=) | gated `-O1+` |
| `pack64_from_stack_stores` | green | 259,376 (=) | gated `-O1+` |
| `shl32_or_chain` | green | 259,376 (=) | gated `-O1+` |
| `pack64_tautology` | green | 259,392 (+16 B) | gated `-O1+` |
| `cmp_narrow_64` | green | 259,376 (=) | gated `-O1+` |
| `assign_fuse` | green | 259,376 (=) | gated `-O1+` |
| `select` (if-conversion) | green | **292,418 (+33,042 B, +12.7%)** | **kept at -O0** |
| `pack64_implicit` | **1 failure** (`gcc.dg/bitfld-3`) | — | **kept at -O0** |

Two of the nine earn their place at -O0 and were left on:

* **`select`** is the whole size story. Gating all eight "safe" passes cost
  +12.7% of -O0 `.text` for −4% compile time; per-pass attribution showed
  `select` was 33,042 of the 33,058 bytes. If-conversion is real work — it
  stays. This is why the phase is per-pass and not a blanket gate.
* **`pack64_implicit` is load-bearing for correctness**: without it
  `gcc.dg/bitfld-3` computes a wrong answer at -O0. An optimization that is
  required for correct output means the un-optimized path has a latent bug —
  the `(hi SHL 32) OR lo` 64-bit bitfield lowering. **Worth its own
  investigation**; until then the pass stays ungated and the bug stays masked.

### Landed effect (host, non-ASAN, same-machine A/B)

| Metric | Before | After | Δ |
|---|---|---|---|
| `source/ir/codegen.c` ×30 at -O0 | 0.76 s | **0.72 s** | **−5.0%** |
| -O0 `.text`, 286-object corpus | 259,376 B | 259,392 B | +16 B (+0.006%) |
| -O1/-O2 output | — | — | **byte-identical, 3,962 pairs** |
| 300-file corpus -O0 wall | 0.17 s | 0.17 s | none (startup-bound) |

Full host `make test` green (`ir_tests` 13,908 passed / 0 failed).

---

## 9. Phase 4 — REJECTED: SSA is not skippable at -O0

The premise in §2.3 — that SSA's only -O0 payoff is DCE-on-phis — is **wrong**,
and this phase is the measurement that disproves it.

Behind a temporary `TCC_O0_NO_SSA` knob, -O0 took the existing
no-promotable-variable fallback unconditionally. The performance case was
excellent, better than the plan predicted:

| Metric | SSA (current) | Fallback | Δ |
|---|---|---|---|
| `source/ir/codegen.c` ×30 at -O0 | 0.745 s | **0.593 s** | **−20.4%** |
| -O0 `.text`, 286-object corpus | 259,392 B | 259,182 B | **−210 B (smaller)** |

So it is faster *and* very slightly smaller — no quality trade to weigh at all.
But it **miscompiles**: 4 of the 1,804 -O0 gcc-torture execute tests fail
(`990404-1`, `pr125291`, `pr34415`, `pending-4`). Two follow-up variants
isolated the cause:

1. keep dominators + frontiers, skip promotion + SSA → **same 4 failures**
2. keep dominators + frontiers **and** multi-def temp promotion, skip only
   SSA construct/rename → **same 4 failures**

So it is **SSA renaming itself** that is load-bearing, not the analyses around
it: renaming gives every definition its own vreg, which is precisely what makes
the allocator's one-interval-per-vreg model sound. A value defined on two paths
without renaming collapses into a single interval spanning both, and the
allocator may hand its register out in between. The existing fallback is sound
only in the case it was written for — nothing promotable, so every such value
already lives in memory.

**Outcome**: the knob was **removed** (the plan's own instruction: don't leave
a third half-supported -O0 variant alive), and the constraint is recorded as a
comment at the `tcc_ir_ssa_construct` call site so the next reader does not
re-derive it. The 20% is real but needs a different interval model — a
liveness representation that handles multi-def values without renaming — not
a skip. That is a much larger piece of work than this plan's remit.

### What is left

Phase 5 (allocation churn) is unchanged and still needs the device-side
ranking from Phase 0 before it is worth starting. The two new leads this work
produced are both bugs rather than perf items: the `pack64_implicit`-masked
64-bit bitfield miscompile (§8), and tcc's missing same-TU inlining (§7),
which would pay off across the whole self-hosted build.

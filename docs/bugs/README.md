# Open bug reports

One file per known, unfixed tinycc bug: `docs/bugs/<short-slug>.md`. A bug
found but not fixed in the session that found it goes here; a session that
comes across a report here fixes it.

## Kernel compiler inefficiencies — 2026-10-09

The [current bottleneck analysis](kernel-bottlenecks-2026-10-09.md) uses the
latest frozen TCC/LLVM benchmark, six warm boots per variant, weighted
instruction profiles and disassembly. These are performance investigations;
the reports distinguish demonstrated costs from unresolved compiler causes.

- [Memcpy byte-loop overhead](kernel-memcpy-byte-loop-overhead.md) — largest
  measured single-function gap in the Zig compile chain: 6.81M instructions.
- [Streaming Wyhash byte assembly](kernel-streaming-wyhash-byte-assembly.md)
  — FIXED 2026-10-10: byte-tiled align(1) 8-byte copies (the readInt
  unaligned-load spelling) now chunk inline, store-forward and fuse into one
  wide source load; kernel.c −60 B .text with 20 → 9 `__aeabi_memmove` sites.
  The variable-length tail copies and the word-wise `__tcc_memmove` helper
  remain as their own lever.
- [ROMFS header-scan costs](romfs-header-scan-costs.md) — the compiler half of
  the closed header-load report: tcc's per-call argument shuffle and the
  `findScalarPos` needle reload. The kernel-side fix (mapped XIP header reads,
  no lock/seek/vtable call) landed 2026-10-09 — ls-bin −14.9% tcc / −14.0%
  llvm — and the page-bitmap report was deleted with its `markRange` fix
  (mark/free 8,219 → 1,114 instructions per warm launch).

## Kernel compiler inefficiencies — 2026-10-10

From the 11:57 kernel_compare battery (both kernels rebuilt off the same
rootfs, all 15 workloads output-identical), per-PC attribution with llvm
inclusive DWARF frames. The shared root cause in the hot loops is a
SETCC→BRANCH fold miss: the CBE materializes every comparison into a bool
temp (`t5 = t4 < K; if (t5)`) and tcc keeps it as
`cmp; ite; movne/moveq; cmp #0; cbz` instead of branching on the flags.

- [zig-version load-path gap](kernel-zig-version-load-path-gap.md) — 1.53×
  (kernel-only 1.65×): `Parser.validate` 158K vs 60K for one call (reloc
  loops ~40 vs ~12 insns/iteration, plus per-iteration opt-struct re-zeroing),
  `Wyhash.update` 1.9×/block with a loop-head cursor store→load round-trip,
  `reserve_pages` per-page bit test.
- [ls-bin scan-family gap](kernel-ls-bin-scan-family-gap.md) — now 1.27× LLVM
  (3,094,353 vs 2,432,861 instructions). **Partially fixed 2026-10-10:**
  needle promotion, bool-window/merge folding, immutable slice-home store
  elision, narrow address-taken local forwarding and borrowing-callee NRVO.
  `mem.eql` 48 → 39 instructions/call, `FileHeader.init` 86 → 30 (160-byte
  copy removed), interface-map byte round-trip removed; ls-bin about −2.6–3.0% and
  ps −2.73% versus the first-fix baseline. Outlining, the eql offset-array
  loop and the other attributed cost families still leave a substantial gap.

## Kernel compiler correctness — 2026-10-10

- [Wyhash wide slice reads pair into LDRD/STRD](kernel-wyhash-unaligned-ldrd.md) —
  the only kernel workload the current cross now fails: the device `zig` chain
  hardfaults (`CFSR=0x01000000`) on `LDRD` off `slice.ptr + 8k` inside
  `Wyhash.hash`. The alignment guards exist (`arm-thumb-mem.c:896`/`1072`
  respect `IROP_AUX_UNDERALIGN`); the aggregate-copy lowering in
  `source/frontend/gen/store/{struct_copy,vstore}.c` never sets the mark.

Two general codegen levers were found while probing the `memcpy` fallback
(2026-10-09, small C repros, no kernel profile needed):

- [Latch `cmp` after a flag-setting `subs`](loop-latch-cmp-after-subs.md) —
  every `for (; n >= K; n -= K)` latch re-tests a value the decrement already
  tested; one wasted instruction per iteration in the copy/clear loops.
- [Re-rolling an unrolled copy body](reroll-unrolled-copy-penalty.md) — a
  post-increment copy body re-rolled into a counted inner loop costs 24
  instructions per 16 bytes where the straight-line body needs 11.

Commands in these reports run from the enclosing YasOS checkout root.

The generic thunk replay report was closed on 2026-10-10: direct pair replay
and a checked 16-byte layout reduce the loop from 8,648 to 2,538 instructions
per warm launch. [Fix and validation](../../../../docs/loader_thunk_replay_fix_2026-10-10.md).

Format:

```
# <one-line statement of the defect>

**Status:** open · **Severity:** correctness (miscompile) | performance | ... · **Found:** YYYY-MM-DD (how)

## Summary
## LLM reporting name <if LLM generated>
## Reproducer        (C source + -O level + the wrong output or code)
## Root cause        (file:function, what check is missing)
## Regression lock   (unit test pinning the current behaviour, if any)
## Likely fix
```

`**Tags:** gap` marks a report found by auditing the `IR_LEGACY_GAP` markers
(`grep -rn IR_LEGACY_GAP source`): a check an optimizer helper never made.  It
may have no reproducer yet; the severity line says whether it is confirmed.
Fixing one usually means deleting its gap marker.

Reports auto-filed by the fuzzer are named `fuzz_<profile>_seed<N>.md`, with
the seed's generated source frozen at `repro/fuzz_<profile>_seed<N>.c`.
`tests/fuzz/triage_olevels.sh` files one per triaged divergent seed via
`tests/fuzz/bug_report.py` — a no-op when a report with that filename already
exists, so re-sweeps never duplicate (delete the report in the fixing commit
first, per the rule below). They arrive from the template above with the
triage table filled in and Root cause/Likely fix still open; treat them like
any hand-written report.

When fixed: flip any `_bug` unit-test lock the report names, add an IR test,
and delete the report in the fixing commit (git history keeps it).

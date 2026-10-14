# Open bug reports

One file per known, unfixed tinycc bug: `docs/bugs/<short-slug>.md`. A bug
found but not fixed in the session that found it goes here; a session that
comes across a report here fixes it.

## Open reports — kernel compiler inefficiencies (2026-10-12 battery)

The [battery index](kernel-bottlenecks-2026-10-12.md) holds the method, the
per-call pairing and the current numbers: after the 2026-10-14 fixes (round 4,
`merge/lc-2026-10-14-7`) the tcc kernel executes **24,273,837 kernel-only
instructions versus 20,435,550 for LLVM (1.188×)**, all 15 workload outputs
matching (round 3 measured 24,100,190 / 1.179×; the same-session base measured
24,118,198, so the difference is battery noise).

Correctness:

- [Hard-float: a struct argument's LDM overwrites a double argument staged in r0:r1](hardfp-struct-arg-clobbers-staged-double-arg.md)
  — the call-argument parallel move does not treat the staged pair as a
  source (`-mfloat-abi=hard`, every level).
- [A packed `_Complex float` member is read with `LDRD` and faults](packed-complex-member-ldrd-unaligned.md)
  — the 64-bit scalar paths check `underalign_hint`, the complex pair does
  not; every level.
- [Inlining a nested function that calls its enclosing nested function never ends](nested-fn-inline-recursion-unbounded.md)
  — the nested-inline gate in `call.c` skips the depth cap and the cycle check
  for nested callees (`-O2`/`-Os` compile hang).
- [A nested function used as a `cleanup` handler is called without the static chain](nested-fn-cleanup-call-no-static-chain.md)
  — `try_call_scope_cleanup` emits the call without `SET_CHAIN` (every level).
- [A nested function cannot call through its parent's function-pointer local](nested-fn-captured-function-pointer-type.md)
  — the captured `CType`'s function `Sym` is gone by the time the nested function is compiled (compile error).
- [A trampoline reads the frame of its parent's latest activation](nested-fn-trampoline-chain-slot-not-reentrant.md)
  — one static chain slot per nested function; a recursive parent overwrites it (every level).
- [A call to a nested function before its `auto`-declared definition binds to a global](nested-fn-auto-forward-decl-call-binds-global.md)
  — the `auto` prototype is an `external_sym`; the definition makes a new Sym (every level).
- [A nested function naming more than 32 parent locals/statics/typedefs/enum constants loses the rest](nested-fn-capture-limit-silently-drops.md)
  — fixed `MAX_CAPTURED_VARS` arrays drop entries silently; `'v40' undeclared` (every level).

Performance (several partly fixed 2026-10-13; each report lists only what is
left):
[access_ok read variant / `_irq_svcall` site](kernel-uaccess-access-ok-outlined-bool-tail.md),
[mem.eql call boundary](kernel-mem-eql-outlined-generic-equality.md),
[ROMFS header return copies](kernel-romfs-fileheader-return-copies.md),
[ROMFS load reader tail](kernel-romfs-load-name-scan-and-reader-tail.md),
[bitmap `setRangeValue` double RMW with `value = true`](kernel-bitmap-helpers-out-of-line-overhead.md),
[path iterator `get_file_header` registers / `last`](kernel-componentiterator-marshalling-blocks-inlining.md),
[u64 known-zero high word](kernel-u64-zero-high-shift.md),
[validate copy-local round-trip](kernel-validate-reused-copy-local-roundtrip.md)
(a fix exists on branch `fix/validate-copy-local` but needs a memory cap),
[noreturn guard LR save](kernel-noreturn-call-cfg-fallthrough.md),
[template replay](kernel-template-replay-invariant-spill-reload.md),
[Wyhash pair repacking](kernel-wyhash-pair-repacking-spills.md)
(extract widths and 8-byte frame growth fixed; residual spill pressure remains),
[inline CAS's dead `expected` initial move](cas-expected-dead-initial-move.md).

Not a compiler defect, kept as a runtime lever:
[malloc free-list scan under heap-layout shift](malloc-free-list-scan-heap-base-shift.md)
(userspace libc; active-pool failed searches cached, measured libc work -22%;
successful first-fit scans still depend on heap layout).

## Format

```
# <one-line statement of the defect>

**Status:** open · **Severity:** correctness (miscompile) | performance | ... · **Found:** YYYY-MM-DD (how) · **Reported by:** <human | LLM name and version, e.g. claude-opus-5-5, qwen3-coder-30b>

## Summary
## Reproducer        (C source + -O level + the wrong output or code)
## Root cause        (file:function, what check is missing)
## Regression lock   (unit test pinning the current behaviour, if any)
## Likely fix
```

`**Reported by:**` names who found the bug. For an LLM-written report, use the
model name and version as exact as you know it (`claude-sonnet-5-5`,
`qwen3-coder-30b`), so a pattern of bad or good reports can be traced to a
model; use `human` for hand-written ones. Fuzzer-filed reports use `fuzzer`.

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

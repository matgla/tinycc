# ls-bin scan-family gap: explicit copy/spill misses fixed; outlining and scan costs remain

**Status:** partially fixed (2026-10-10) · **Severity:** performance · **Found:** 2026-10-10
(kernel_compare battery `results-20261010-115749.json` + per-PC attribution;
tcc kernel `07d047f2…`, llvm kernel `1f888a07…`, rootfs `fd5b5b66…`)

## Update 2026-10-10 — remaining copy, bool and byte-local misses fixed

The cross now removes the concrete residues called out after the first
fixes. This does **not** close the entire LLVM instruction gap; the remaining
costs are listed below rather than treating the original attribution as a list
of independent, fully removable overheads.

- **Slice entry stores:** `param_home_fwd` removes an immutable parameter-home
  store after forwarding every eligible word read. Escaped addresses, writes,
  volatile reads, partial reads and wider reads retain the store. Complex
  accesses count both components; otherwise a complex-float read can silently
  lose the next parameter word (`fp_hard_complex_exec` softfp regression).
- **Boolean merge residues:** the window fuse also recognizes `SELECT cond,
  1, 0` and its inverse. `bool_diamond_branch` runs again after phi resolution,
  when its single-use tests are explicit. Its use scan includes every
  four-operand instruction, including SELECT, so another value use prevents
  threading. The actual eql prologue now branches directly without the
  empty-check materializations or the byval `strd` spill.
- **Address-taken byte probe:** `ptr_local_fwd` follows single-definition
  pointer bindings and direct TEMP LEAs. Narrow reads of a word-backed local
  become explicit UBFX/SBFX, preserving truncation and signedness. Volatile
  accesses remain memory accesses. Dead pointer bindings disappear, and the
  interface-map probe's byte store/reload round-trip disappears with them.
  `refresh_stale_var_addrtaken` also recognizes embedded address operands:
  forwarding the last explicit LEA must not promote a local whose address is
  still used by an inlined cleanup.
- **Header return copy:** completed static, non-weak callees record a
  conservative `param_nocapture` summary. The existing NRVO family analysis
  can then merge a local passed to a proven borrowing initializer into the
  return buffer. Private frame words are tracked through CFG joins with full
  writes replacing the old value; this distinguishes FileHeader.load's inline
  name pointer from the heap pointer that later replaces it. Unknown calls,
  returned/stored pointers, exposed frame objects and analysis limits remain
  escapes. These summaries relax callee-side object merges only: a borrowed
  argument can still alias the same call's sret destination, so caller-side
  sret de-aliasing remains conservative. FileHeader.init's 160-byte copy is
  absent from the final kernel.

Measured against the saved compiler and 13:03 baseline, with unchanged kernel
sources and rootfs (`55e29a95…`), final tcc kernel `2b7eaa39…`, battery
`results-20261010-150537.json`:

| ls-bin cost centre | before | after | instructions/call before → after |
|---|---:|---:|---:|
| `mem.eql` (3,398 calls) | 163,195 | 132,613 | 48.03 → 39.03 |
| `FileHeader.init` (368 calls) | 31,648 | 11,040 | 86 → 30 |
| interface-map probe (310 calls) | 49,178 | 45,236 | 158.64 → 145.92 |
| `uaccess.range_ok` (1,228 calls) | 44,208 | 42,980 | 36 → 35 |
| `findScalarPos` (4,056 calls; needle promotion already applied) | 149,451 | 149,451 | 36.85 → 36.85 |

Whole `ls /bin`: **3,188,310 → 3,094,353 instructions (−2.95%)**;
`ps`: **1,053,731 → 1,024,934 (−2.73%)**. All 15 battery workloads retain
exit status and output hashes; no count warning. Host compilation of the
pinned generated kernel C takes 4.065 → 4.070 seconds (three-run medians,
about +0.14%, within measurement noise). A paired repeat with identical
kernel hashes (`results-20261010-151027.json`) gives 3,105,667 tcc versus
2,441,672 LLVM instructions for ls-bin, a −2.59% tcc improvement over the
saved baseline. Timer activity varies whole-workload totals; the observed
gain is approximately 2.6–3.0%, and both runs pass all output/status checks.

Regression locks: `bug_kernel_eql_slice`, `906_narrow_local_probe`,
`907_borrowed_header_nrvo`, and three assembly-shape checks. Validation:
4,583 runtime tests pass (one skipped), 167 assembly/parameter-home checks
pass, frontend checks and all unit suites pass, and the unity manifest is
current. A 15,000-seed differential pre-scan (5,000 each of int/ptr/struct_byval;
-O0/-O1/-O2 plus GCC -O0) has no new divergences. Pointer seeds 1683/2728
still diverge, with identical signatures under the saved original compiler.
This is the batched pre-scan, not an exhaustive context-sensitive fuzz proof.
The newly cross-built native compiler runs on QEMU and emits byte-identical
objects for all 11 selected optimizer/regalloc/backend TUs. This is a targeted
host/device comparison, not a full self-host fixpoint run.

### Remaining work

LLVM's same-rootfs baseline is 2,432,861 instructions: tcc remains about
**1.27×**, a roughly 662–664K-instruction whole-workload difference. The explicit
misses above are fixed; these larger remaining compiler costs are still open:

- `mem.eql` is outlined. Its hot path still pays a six-register save/restore
  and a 40-byte frame used by the cold four-offset scan. The 4–16-byte tier
  still constructs that offset array and loops over it. LLVM inlines the
  equality check and specializes it at the caller. Removing the spill alone
  cannot make 39 instructions/call match the original LLVM ~10/call.
- `findScalarPos` now loads the stack-passed needle once, but retains an
  outlined optional/sret interface, a frame and general scan setup. Its
  original per-byte reload is fixed; the remaining attribution is a separate
  inlining/return-value cost.
- Header and interface-map helpers still pay call boundaries, argument
  shuffles and general hashing/probing costs. FileHeader.init is 30 rather
  than LLVM's original 16 instructions/call even after the copy disappears.
- u64 division, formatting, syscall dispatch and heap locking account for
  other portions of the original whole-workload gap. They are not evidence
  that the copy/spill transforms above failed.

The original measurements and disassemblies below are historical evidence,
not the current generated code.

## Update 2026-10-10 — first compiler fixes landed

Three transforms in the cross (ir_tests `905_setif_window_fuse.c` +
`test_narrow_stack_param_needle_not_reloaded` pin them):

1. **`setif_window_fuse`** (`source/opt/flat/scalar/branch.c`, runs inside
   `setif_fuse`/`ra:setif_fuse`): `CMP; SETIF; <flag-neutral window +
   single-use copies>; TEST_ZERO; JUMPIF` → `CMP; window; JUMPIF`.  The old
   fuse demanded NOP-adjacency; the CBE's bounds-check quartets carry the
   struct-copy STOREs between the SETIF and its test, and phi resolution
   leaves 2-hop copy chains (`T1710 <- SETIF; T1711 <- T1710; T1245 <- T1711;
   TEST_ZERO`).  Sound because the backend already keeps any window between
   a CMP and its branch flag-free positionally (`codegen_flags_live` /
   `flags_safe()` choose flagless encodings), and the walk refuses any jump
   target inside the window.  Per-vreg mention counts are precomputed lazily
   (a candidate that walks pays once; a dead store into a carrier inside the
   window is refused, closing the one hole a plain-def relaxation had).
2. **`setif_branch_remat` accepts two-register compares** (was: one register
   + one immediate): `CMP a,b; SETIF; …t tested at several branches` now
   re-materializes the compare at each test, so multi-use bools with
   register-register compares fold too (the T129 shape below).
3. **`ra:stack_param_promote` accepts narrow widths** (u8/u16 stack-passed
   params whose every use reads the same width): the findScalarPos needle
   (u8 5th argument) loads once at entry and lives in a register through the
   loop — `ldrb buf[i]; cmp; bne` per byte, gcc's shape
   (`repro/loop_stack_param_reload_2026-10-09.c` now compiles clean).

Measured (battery `results-20261010-130346.json` vs the 11:57 baseline,
kernel-only): `uaccess.range_ok` 58,756 → 44,208 (−25%, the outlined
bool round-trip folded); whole workloads: **ps −3.37%, echo −2.93%**;
findScalarPos's loop is reload-free.  eqlBytes' scan quartets fold too
(12→3 materializations on the extracted function), but `mem.eql`'s per-call
count is unchanged — its surviving round-trips are the *merge-phi diamonds*
(`t10 = (len==0) ? true : (ptr==ptr)` tested after the merge, where control
from the `true` arm lands directly at the test, so no window fuse may fire)
and the `strd` byval slice spill.  Cost: ~+2% host compile time on the
kernel.c TU (window walks + extra remats; +0.36% on the on-device
tcc-bench-c-O2 workload).

Open after the first update (addressed by the second update above): the eql
merge-phi diamonds and byval spill (needed bool-phi threading at merges and
non-escaping byval copy elision), the
`FileHeader.init` 160-byte copy LLVM elides, the vcall opt-struct residency.
u64 division remains a separate report.

## Summary

`ls /bin` runs 3,190,954 instructions under tcc vs 2,443,472 under LLVM
(1.31×). Kernel-only (PC < 0x60000000): 2,124,076 vs 1,376,594 — a
747,482-instruction gap. Attribution (llvm inclusive via DWARF inline
frames; RankedMutex, the malloc stack, uart and memcpy are at parity and
excluded):

| cost centre                       |      tcc |      llvm |    gap | share |
|-----------------------------------|---------:|----------:|-------:|------:|
| `mem_eql__anon_10757__7302` (3,398 calls, 48.0/call)  | 163,195 | 34,739 | +128,456 | 17% |
| `mem_findScalarPos__anon_1363` (4,056 calls, 36.9/call) | 149,829 | 59,232 | +90,597 | 12% |
| romfs header family: `FileHeader_load` 113/call, `.init` 86/call, `get_file_header` 104/call, `create_file_header_with_offset` 95/call, `.deinit` | 393,525 | 273,752 | +119,773 | 16% |
| vcall process-interface map probe (`hash_map_Custom(u16,ProcessInterface)…` 310 calls, 159/call) | 49,486 | ~0 standalone | +49,486 | 7% |
| syscall dispatch family (`irq_svcall` + `process_syscall_*`, 620 syscalls, 74 vs ~53/syscall) | 45,832 | 32,686 | +13,146 | 2% |
| `uaccess_range_ok` (1,228 calls, 36 vs 16.5/call) + `strnlen_user` | 58,756 | 35,676 | +23,080 | 3% |
| `__tcc_aeabi_uldivmod_helper` (184 calls, 105/call) | 19,320 | ~891 | +19,320 | 3% |
| `vfmt_render`/`vfmt_emitValue` (208/31 vs 196/15 per call) | 96,660 | 80,908 | +15,752 | 2% |
| kheap lock/unlock (2,722 pairs, 33.5 vs 26.8/pair) | 91,187 | 72,920 | +18,267 | 2% |

(The `+42K irq_svcall` seen in naive per-symbol diffs is an artifact — the
tcc `irq_svcall` nm entry has no size and swallows the `process_syscall_*`
helpers; LLVM's number is inclusive. RankedMutex(_mount) lock+unlock is now
at parity: 68,313 tcc vs 69,169 llvm inclusive — the old out-of-line-pair
cost premise no longer holds.)

## Root cause

**1. `mem.eql` prologue: 12+ instructions before the first byte compare.**
Every call (3,398×, mostly 1–16-byte directory names) executes:

```text
strd r2, r3, [sp, #24]    ; both slice params spilled to the frame (byval copy)
cmp   r0, #0; ite eq; moveq r1,#1; movne r1,#0; cbz r1   ; (b.len==0) → bool round-trip
cmp   r2, r2 …
cmp   r4, r1              ; a.len==b.len (third re-compare)
cmp   r0, #16; bhi …      ; std eqlBytes size ladder
cmp   r0, #4;  bcs …
```

The `strd` is the CBE's address-taken param copy (`t0 = a0; t1 = &t0;` in
kernel.c) that never needs to happen for a leaf compare; the two empty-check
bools are the SETCC→BRANCH fold miss (see
`kernel-zig-version-load-path-gap.md`). LLVM's inlined eql does the same
size ladder with no spill and turns the ≤3-byte tier into three branchless
first/middle/last compares. 48 vs ~10.2 insns/call.

**2. `findScalarPos` — the needle reload is still there.** 22.5K loop-body
executions; per byte:

```text
ldrb.w r5, [r8, r2]     ; slice[i]
ldrb.w r6, [sp, #32]    ; the NEEDLE byte reloaded from the stack EVERY byte
cmp r5, r6; bne next
```

plus an sret'd optional written through `r4` on the found path inside the
loop. This is the surviving shape of the compiler half of
[`romfs-header-scan-costs.md`](romfs-header-scan-costs.md) (its repro
`repro/loop_stack_param_reload_2026-10-09.c`): the loop-invariant needle
byte lives in an incoming-arg stack slot tcc does not promote into a
register.

**3. romfs header family.** `FileHeader.init` at 86 insns/call is a
13-trip `ldmia/stmia` copy of a 160-byte header struct (368 calls; the copy
loop alone is ~11.4K insns) — LLVM inlines init at call sites and elides
the unused-field copies (16 insns/call). `FileHeader.load` (113 vs 79.4
incl.) and `get_file_header` (104 vs 88) carry the per-call argument shuffle
already documented in `romfs-header-scan-costs.md`; that report's numbers
predate the 2026-10-09/10 kernel-side mapped-reader fix, so its remaining
compiler-half costs are these.

**4. vcall interface-map probe (159 insns/call × 310).** The hot path
stores a 1-byte entry into a stack slot, re-loads it, then decodes the
optional discriminant:
`ldrb r7,[r6]; mov r9,#0; strb [sp,#28]; str r7,[sp,#28]; add r7,sp,#28;
ldrb r8,[r7]; mov r7,r8,asr #7; cmp r7,#1` — the opt-struct memory
residency family (same root as the `__atomic` opt-struct fix): a
`?*Entry` probe round-tripped through an address-taken stack temp instead
of a register + tag branch.

**5. `uaccess.range_ok` (36 vs 16.5/call)** — push/call/bool-materialize/pop
around a 2-region bound check; LLVM inlines it into `access_ok` and
branches on the compare flags directly. Same bool-fold family plus outline
cost of a 25-instruction helper called 1,228 times.

Not new: `__tcc_aeabi_uldivmod_helper` is the open u64-division layer-cake
report; `vfmt` at 1.2× is a mild residual.

## Reproducer

Same pinning as the zig-version report:

```bash
python3 scripts/kernel_compare.py --skip-build --variants tcc,llvm
ls .cache/kernel_compare/insns/{tcc,llvm}-ls-bin.csv
arm-none-eabi-objdump -d .cache/kernel_compare/tcc/bin/yasos_kernel \
  | sed -n '/<mem_eql__anon_10757__7302>:/,/^$/p'
```

Entry-PC counts in the CSVs give exact call counts (e.g. PC 0x10007904 ×
3,398). Source-level extraction anchors: kernel.c `mem_eql__anon_NNNN`,
`mem_findScalarPos__anon_NNNN`, `fs_romfs_file_header_FileHeader_init__NNNN`
(verbatim extraction rules as in the 2026-10-09d session notes).

## Likely fix

- SETCC→BRANCH folding (umbrella, shared with the zig-version report)
  covers the eql prologue, range_ok and parts of init/load — the highest
  call-count levers in this workload (3,398 + 1,228 + 1,534 + 368 sites).
- Loop-invariant promotion of incoming-arg bytes (`findScalarPos` needle)
  closes with the existing `romfs-header-scan-costs.md` compiler half.
- Dead-store/elision of non-escaping byval param copies (`t0 = a0; t1=&t0`)
  would remove the eql spill and shares machinery with the arg-shuffle item
  in that report.
- The vcall probe needs opt-struct scalar replacement in loops (register +
  tag), the same transform the `__atomic` fix applied to call arguments.

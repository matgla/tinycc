# romfs header scans: FileHeader.load 2.2×, findScalarPos 3× per byte

**Status:** open · **Severity:** perf (≈40% of the tcc/llvm gap on `ls` when taken with
the mutex pair it calls; ~2.5% of a process launch) · **Found:** 2026-10-09 (evening
rebench)

## Summary

Directory-shaped workloads walk the romfs header chain once per entry:
`get_file_header` → `FileHeader.load` (seek + 32-byte interface read + `rev` fields) with
`findScalarPos` scanning sentinels per step. Per-call costs in the tcc kernel are 2–3×
LLVM's, on 1,533 `FileHeader.load` calls (`ls-bin`):

| Function | tcc | llvm | per call tcc | per call llvm |
| --- | ---: | ---: | ---: | ---: |
| `FileHeader.load` | 233,190 | 107,379 | ~152 | ~70 |
| `findScalarPos` | 149,834 | 50,716 | ~3× | 1× |
| `get_file_header` (+inlined cache) | 174,447 | ~29,416 (LookupCache.find) + inlined | — | — |

## Mechanisms

`FileHeader.load` (tcc `0x10026bf8`, `0x2dc` bytes):

1. **236-byte frame of argument shuffling**: the prologue copies four sret/stack
   arguments through ten `ldr/str` pairs before doing anything (`ldr r0,[sp,#320]` →
   `str r0,[sp,#36]`, …). LLVM passes the same aggregate mix in registers / reorders.
2. **A RankedMutex lock+unlock pair per header read** — see
   [`rankedmutex-and-romfs-header-scan.md`](rankedmutex-and-romfs-header-scan.md);
   at ~62 insns per op this is ~124 of the ~152-insn per-call cost's overhead budget.
3. The 32-byte read goes through an interface vtable call (`blx r7`), with the vtable
   pointer re-loaded from the stack argument area at each use.

`findScalarPos` (tcc `0x100017b4`, `0x54` bytes): the needle byte is the function's 5th
argument, so it lives in the incoming stack-arg area — and the scan loop reloads it from
`[sp,#32]` **every iteration** (`ldrb r5,[r8,r2]; ldrb r6,[sp,#32]; cmp`). The
loop-invariant stack-arg is not promoted into a register; LLVM keeps it in a register
for the whole loop. This is the same family the 2026-10-09b rebench filed as
`loop-invariant-params-reloaded-per-iteration` (closed by late `ra:licm` +
`stack_param_promote`) — the surviving shape is a load from the *caller-arg area*, which
that promotion evidently does not treat as a promotable local.

## Fix directions

- tinycc: `stack_param_promote`/`ra:licm` should hoist loads from the incoming argument
  area when the address is not captured (the `[sp,#arg]` read here is provably
  invariant);
- tinycc: aggregate-argument shuffle in `FileHeader.load`-shaped prologues (many
  sret/stack args) is worth a pass-level look — ten dead copies is not register
  pressure, it is absence of copy elimination across the prologue;
- kernel: `FileHeader.load` could take the mutex once per directory scan rather than
  once per header (the mutex pair is pure overhead multiplied by 1,533 on `ls`).

## Reproducers

- **findScalarPos needle reload**:
  [`repro/loop_stack_param_reload_2026-10-09.c`](repro/loop_stack_param_reload_2026-10-09.c)
  — a five-argument scan function whose needle is the stack-passed 5th parameter.
  Verified 2026-10-09: tcc's loop is `ldrb r5,[r1,r4]; ldrb.w r8,[sp,#48]; cmp r5,r8`
  (needle re-loaded from the incoming argument area every byte); gcc hoists it into a
  register and uses post-increment addressing (`ldrb.w r3,[r1,#1]!; cmp r3,r0`). The
  file's `find_scalar_reg` (4-arg form, needle in a register) compiles cleanly under
  tcc — it is specifically the argument-area address that is not promoted.
- **FileHeader.load prologue shuffle / mutex pair**: no standalone reproducer yet — a
  minimal by-value-struct callee compiles clean; the ten dead argument copies need the
  full sret + interface-argument mass of the kernel function. Disassemble the tcc kernel
  at the addresses above (`0x10026bf8`, `0x10008b18`); for the llvm side attribute PCs
  through DWARF (`arm-none-eabi-addr2line -e llvm/bin/yasos_kernel -f`) — the llvm ELF
  is stripped of symtab but keeps full debug info. The mutex pair's own reproducer is
  [`rankedmutex_lock_fastpath_2026-10-09.c`](repro/rankedmutex_lock_fastpath_2026-10-09.c).

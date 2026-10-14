# `std.mem.eql(u8, ...)` stays one out-of-line generic body: every call pays the call boundary, 48% of calls are a length mismatch

**Status:** open · **Severity:** performance · **Found:** 2026-10-12 (20:45 kernel_compare battery, per-PC attribution, disassembly)

## Summary

`mem_eql__anon_10761__7304` (Zig `std.mem.eql(u8, a, b)` with `sliceAsBytes` and `eqlBytes`
inlined into it) is one out-of-line body called from 71 sites in kernel.c. LLVM inlines it at
every call site. At the 20:45 battery it ran 832,913 tcc instructions in 26,837 calls
(31.0/call) against 250,712 LLVM inclusive (~9.3/call).

The 2026-10-13 fix (branch `fix/mem-eql-generic`) cleaned up the callee itself:

- the incoming slices stay in their argument registers (`ra:co_hint`: a graph-coalesced
  class takes its hinted partner's register), so the four entry `mov`s are gone;
- the late unroll's `T <- #0` index and `0 | x` accumulator fold (`const_prop_tmp` runs again
  after `sl_forward`; identity fold of `#0 OR x`, `x ADD #0`, ...), so no frame is left
  (`sub sp,#8` gone) and the 4-16 path needs no r8/r9;
- each arm returns its 0/1 directly in r0 (`ra:return_sink`, `source/opt/ra/return_sink.c`):
  `C <- x; JMP L` into a block that only returns C becomes `RETURNVALUE x`, so the bool
  carrier, the join's `mov r0,rC` and the `uxtb` are gone;
- the push shrank from `stmdb {r4-r9}` to `push {r4-r7}`;
- VRP reads `CMP #c,x` (swapped compares) and `x != 0` / `x >u c` non-zero facts, so
  `mem_trim`'s inlined `findScalarPos` zero-trip guard is tested once per loop instead of twice.

Measured (2026-10-13 battery vs the 2026-10-12 fix base, all outputs same): `mem_eql`
832,913 -> 629,692 instructions (23.5/call), `mem_trim` 119,540 -> 108,638, kernel-only
-267k (-0.99%). The length-mismatch path is now
`push; cbnz; cbnz; cmp; beq; nop; movs r0,#0; b; pop; bx lr` (10 instructions, the `nop` is a
branch-target pad) against LLVM's 6-instruction in-situ exit.

## What remains

1. **Not inlined.** The optimized IR of `mem_eql` has ~100 real ops and a CFG cycle (the
   `len > 16` word loop). `backend/generators/function.c` only marks a body
   `func_small_leaf` / `func_auto_inline` if it fits `SMALL_LEAF_INLINE_OPS` (24) or the
   small-predicate limit (32); the post-opt revoke clears `func_auto_inline` for loop bodies,
   and `func_const_arg_loop` needs a register-only body. Constant-length sites (`"/"`, 4- and
   5-byte literals in `modules_file_resolver`) call the generic body too.
2. **No partial (guard-prefix) inlining.** Nothing under `source/` can inline the
   `a.len != b.len -> false` / `len == 0 || a.ptr == b.ptr -> true` prefix and call the body
   only for equal non-trivial lengths. That prefix is pure (it only reads the arguments), so
   re-running it in the callee on the slow path is safe. Estimated ~220-250k (12,988 mismatch
   calls x ~19 instructions).
3. **No shrink-wrapping.** `push {r4-r7}` / `pop` run on the mismatch and same-pointer exits,
   which use no callee-saved register (~2 x 13,113 instructions).
4. **SETIF arms still copy into r0.** The 1-3 and 4-16 paths compute their 0/1 into r4 and
   `mov r0,r4`: `a.ptr` holds r0 until the `len > 16` path at the end of the body, and the
   linear scan has no lifetime holes to give r0 to the arms.
5. **CBZ lost on the `len == 0` test.** Its target is now the `RETURNVALUE #1` arm, and
   `tcc_gen_machine_cbz_forward_ok` (`arm-thumb-call.c`) rejects it: `min_offset` =
   12 - 8 - 6 (`rehearsal_max_shrink_between` counts the following `CMP; JUMPIF` as a 4-byte
   shrink plus a pad) < 0, so `cmp r1,#0; beq` is emitted instead of `cbz r1`.
6. **Call-site aggregate round trip.** The by-value slice argument (CBE struct temporary `t11`
   in `mem_startsWith`) is stored and reloaded through the stack at both hot sites
   (`find_child`: `strd r0,r1,[sp]` ... `ldrd r2,r3,[sp]`; `find_entry`: `ldrd r2,r3,[sp]`
   every iteration), ~2-3 instructions per call.
7. **`mem_trim__anon_22429__11731`** (57.6/call vs LLVM 27.5) is not inlined either, so the
   constant `"/"` strip set cannot collapse the inner `indexOfScalar` loop, and the
   stack-passed half of `values_to_strip` is reloaded every outer iteration
   (`ldrd r5,r6,[sp,#36]`).
8. **Side effect seen in the battery:** the smaller bodies let `fs_ifile_FileName_get_name`
   inline into `RomFs.get_file_header` (-8,770 there, +9,494 in the caller). Its walk loop now
   stores the next name slice `strd r5,r7,[sp,#72]` at the latch, and the loop head
   overwrites the same 8 bytes (`strd sl,r8,[sp,#72]`) before any read: a store dead across
   the back-edge that DSE does not remove (9,265 executions).

## LLM reporting name

kernel-mem-eql-outlined-generic-equality

## Reproducer

`libs/tinycc/docs/bugs/repro/kernel-mem-eql-outlined-generic-equality.c` copies
`mem_eql__anon_10761__7304`, `mem_sliceAsBytes__anon_43297__16598`, `mem_eqlBytes__1647` and
`mem_startsWith__anon_19772__10848` verbatim from kernel.c, with two reduced callers,
`find_child` (find_locked) and `find_entry` (RomFs walk).

```
libs/tinycc/bin/armv8m-tcc -O2 -fno-pic -ffunction-sections -fdata-sections -DYASLIBC_REAL_BOOL \
  -mfloat-abi=hard -mfpu=fpv5-sp-d16 -fvisibility=hidden -include apps/zig/zig_c_prelude.h \
  -I ~/.zvm/master/lib -c libs/tinycc/docs/bugs/repro/kernel-mem-eql-outlined-generic-equality.c -o r.o
```

Both callers still `bl mem_eql`. `clang --target=thumbv8m.main-none-eabi -mcpu=cortex-m33 -O2`
on the same file shrink-wraps: it tests length and pointers before `push {r4,r5,r6,lr}`.

## Regression lock

- `tests/ir_tests/bug_mem_eql_entry_shape.c` (+ `.expect`, all four -O levels): mem.eql over
  lengths 0-40 and all alignments, the two callers, mem.trim, unrolled accumulators, identity
  folds and goto-out return carriers (narrow, re-read, volatile, address-taken).
- `tests/ir_tests/test_codegen_asm.py::test_mem_eql_entry_shape`: no frame, no entry copies,
  no `uxtb`, at most 4 saved registers, 5 `orr`s on the unrolled paths, one zero test per
  mem.trim loop, goto-out constants returned in r0, a volatile carrier still read back.
- `tests/ir_tests/bug_kernel_eql_slice.c` (correctness of this shape, lengths 0-64).

No test locks the call boundary itself: that needs items 1-2.

## Likely fix

- **Guard-prefix inlining** of `mem_eql` (item 2): inline the pure entry prefix, call the
  body for the rest. A narrower variant: inline when the IR is "small acyclic prologue + one
  loop-bearing tail", with the tail outlined as a separate static function.
- **Full inlining at hot/constant-length sites** (item 1): an LLVM-like budget for a ~100-op
  body when `b.len` is a constant or the site is in a loop.
- **Shrink-wrap** callee-saved pushes past early exits that use none (item 3).

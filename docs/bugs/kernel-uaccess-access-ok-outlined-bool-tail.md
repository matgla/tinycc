# uaccess.access_ok: the read variant stays out of line; loop-body and bool-tail shapes

**Status:** open · **Severity:** performance · **Found:** 2026-10-12 (20:45 kernel_compare battery, per-PC attribution, disassembly)

## What remains

1. **The read variant `__anon_51583` (9 sites, 0x7e bytes) stays out of line.** Its body is two
   inlined `permitted` loops, 59 ops, which is more than the 32-op predicate tier
   (`function.c:ir_body_is_small_predicate`). It is the only pure 0/1 predicate in the kernel between 32
   and 64 ops. An experiment that raised the tier to 64 (kc2_fix_access-ok-read_cap64 vs
   _r2, all outputs the same) inlined it into `_irq_svcall` and into `uaccess_check__anon_52332`.
   That `check` was a 7-op wrapper. With the predicate inside it, it has 58 ops and goes out of line, so its callers
   still make one call. The family moved from 850k to 823k (−27.5k), plus `sys_mremap` −14.6k, for +128 B of
   `.text`. That is too small to justify a policy change on its own. What the fix needs is bottom-up classification, which
   sizes a wrapper by its own ops plus the class of the callee it expanded (see
   `kernel-componentiterator-marshalling-blocks-inlining.md`). Without that, inlining the
   predicate only moves the call into `check`.
2. **The loop body is still worse than LLVM's:**
   - `lsls r5,r4,#3; adds r6,r3,r5` for each iteration, where a shifted-register address would do.
   - A `nop` pad after `bls` that is reached by fall-through and executed on every
     non-matching iteration.
   - The dead poison `mvn.w r1,#0x55555555` on the overflow path (also in `_irq_svcall`'s
     inlined copy).
3. **Bool tails at the inlined `caller_is_untrusted` and at the read-variant call sites.**
   - In `_irq_svcall`: `cmp r2,#0; ite eq; moveq r1,#1; movne r1,#0; mov r0,r1; mov r6,r0;
     uxtb r6,r6; cmp r6,#0; beq`. The other join edges set `r0 = 0` directly.
   - The callers of `__anon_51583` (`sys_write`, `sys_nanosleep`, `sys_prlimit`, ...) re-zero-extend
     the bool result with `mov r1,r0; uxtb r1,r1; cbz r1` before testing it.
4. **Repro `check` (`t0 = !t0; if (t0) return 23; return 0;` returning `uint16_t`).** The constant
   edges still go through `movs r0,#1|#0; cmp; ite; cmp; ite`. They are not threaded to the
   two returns. The kernel's `uaccess_check` returns an error union and is threaded.
5. A side effect seen in `ProcessMemoryPool_reserve_pages`: the inlined `isSet` test in the
   page-scan loop became `cmp r5,#0; beq` instead of `cbz`, which costs +1 instruction per
   iteration. The IR is the same apart from the threaded `found = false` edge, which now
   skips its re-test. The fall-through block is shorter by 2 bytes, and
   `tcc_gen_machine_cbz_forward_ok` now refuses the fusion. This was inferred from the IR and
   not instrumented.

## Battery note (2026-10-13, fix2/access-ok-read)

The battery is kc2_fix_access-ok-read and _r2 against kc_fixbase_20261013, with all outputs the same.
- **Without zig-obj-tcc**, kernel-only is −130,752 (−1.08%). The `_irq_svcall` family is −58k over all
  workloads (`_irq_svcall` +279k, `caller_is_untrusted` −182k, `__anon_51128` −156k), and the vfmt family is −10k.
- **zig-obj-tcc** gains +296k kernel-only in both runs. The gain sits in `ProcessMemoryPool_reserve_pages` (+228k) and `memset`. Both have
  byte-identical code, and its userspace count moves by +7.15M on the same binaries. The
  cause is the kernel image layout, not code. Across the fix-base and the 2026-10-13 fix runs,
  `reserve_pages` in that workload ranges from 726k to 1,073k with no change to its code, and the
  10-13 base sits at the low end.

## LLM reporting name

kernel-uaccess-access-ok-outlined-bool-tail

## Reproducer

`repro/kernel-uaccess-access-ok-outlined-bool-tail.c` (copied from the generated kernel C):

```sh
libs/tinycc/bin/armv8m-tcc -O2 -fno-pic -ffunction-sections -fdata-sections -DYASLIBC_REAL_BOOL \
  -mfloat-abi=hard -mfpu=fpv5-sp-d16 -fvisibility=hidden -include apps/zig/zig_c_prelude.h \
  -I ~/.zvm/master/lib -c libs/tinycc/docs/bugs/repro/kernel-uaccess-access-ok-outlined-bool-tail.c -o r.o
```

`dispatch` is now fully inlined. `check` shows item 4. For items 1 to 3, see the kernel object
(`uaccess_access_ok__anon_51583__17623`, its callers, and `_irq_svcall`).

## Regression lock

- `tests/ir_tests/bug_access_ok_bool_tail.c` covers constant-only return slots and predicate-loop inlining.
- `bug_uaccess_setcc_latch.c` covers the `range_ok` latch.
- `bug_ra_branch_thread_copy_at_join_setter.c` covers `branch_thread` join copies.
- `bug_struct_tag_paren_defers_body.c` and `test_codegen_asm.py::test_struct_tag_declarator_keeps_body_deferred`
  cover a body with `struct T (*fp)(...)` staying deferred, so later callees inline into it.

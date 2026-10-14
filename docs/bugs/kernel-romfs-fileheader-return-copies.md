# ROMFS header lookup: `get_file_header` still copies the 160-byte `FileHeader` across blocks and through *P0

**Status:** open · **Severity:** performance · **Found:** 2026-10-12 (20:45 kernel_compare battery, per-PC attribution, disassembly)

`get_file_header` is at 771,741 instructions (2026-10-13 round 2 battery; LLVM inclusive is 765,617).
33 copy loops are left in it.

## What remains

1. **Copies whose source is built in another block.** The inlined `first_file_header`
   (367 calls) is the main case: `X.payload = tmp.payload; X.is_null = 0`, then
   `if (X.error)` in a join block, then `t9 = X.payload`. Those are 4,771 and 5,138 `stmia`
   iterations, about 40k instructions. The relay merge (`cfw_relay` in
   `source/opt/ssa/memory/copy_fwd.c`) only looks back inside the copy's own block. To handle
   this case it would need every def of S that reaches the copy, on all paths, and it would
   need to know that the error path never reaches the copy.
2. **Copies through the struct-return buffer.**
   - After the recursive `get_file_header` / `create_file_header` call (545 times), `t57 →
     *P0 → t9` is two 160-byte copies. `sret_nrvo` merged `t21` into *P0 because another
     live range copies it into the result, so `t9.payload = t21` reads *P0, and copy_fwd does
     not forward through that. A "do not merge a relay local" rule in `nrvo_merge` was tried.
     It made the 545 path a single copy, but a copy elsewhere grew by the same amount, so the
     net change was +167 and it was dropped. `ConstructCountingInterface(IFile).clone`
     (+3.2k, `t3 = *a0; t2 = t3; …; t3 = t2; ret.payload = t3`) has the same cause, and that
     rule did not reach it either.
   - At `walk`'s return into `get_file_header`, `*P0 → t9 → *P0` is a round trip (312–313
     calls, about 4k `stmia` iterations each way). `t9`'s address goes to `offset_of`, so
     the second copy stays.
3. **Cancelling moves in the sret call's argument setup.** `create_file_header` still begins
   with `mov r0,r1; mov r1,r2; mov r2,r1; mov r1,r0` (840 calls, about 3k).

## LLM reporting name

kernel-romfs-fileheader-return-copies

## Regression lock

- `tests/ir_tests/bug_copy_fwd_relay_padding.c` locks the padding-aware cover and the relay
  merge (runtime at all levels). The asm lock is
  `test_codegen_asm.py::test_optional_copies_between_locals_forwarded`: at most 5 copy loops in
  `walk`, down from 7.
- `tests/ir_tests/bug_sret_nrvo_partial_forward.c` locks the `return try` forwarding.
  `542_sret_nrvo.c`, `641_sret_nrvo_merge.c`, `642_sret_claim_shapes.c` and
  `907_borrowed_header_nrvo.c` cover the other NRVO shapes.
- Nothing locks the cross-block copies.  `FileHeader.deinit` (formerly item 3) now expands
  its test at each site: `bug_guard_head_partial_inline.c`,
  `test_codegen_asm.py::test_guard_head_tests_before_the_call`.

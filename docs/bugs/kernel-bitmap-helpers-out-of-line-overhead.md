# Kernel bitmap helpers: inlined `setRangeValue` still does two read-modify-writes when `value = true`

**Status:** open · **Severity:** performance · **Found:** 2026-10-12 (20:45 kernel_compare battery, per-PC attribution, disassembly) · **Reported by:** claude-opus-5-5

## Summary

Since `fix4/bitmap-helpers-leftovers` (2026-10-14), the following are fixed:

- **Register returns.** At `-O1` and above, a static function's 8-byte
  aggregate (`?usize`, `E!*T`, `[]u8`) comes back in r0/r1. The callee builds
  it from its members, and the caller stores it back member by member. The
  choice is made only after the whole TU is parsed, so it sees every
  declaration, attribute, alias and top-level asm statement. A name spelled
  out in the asm of a body saved for the end of the TU keeps that function
  AAPCS, since that asm runs after the choice. A pointer, a table entry or an
  asm reference reaches the function through an AAPCS adapter `f.aapcs`. At
  `-O0` every body is generated while parsing, so nothing uses r0/r1.
- **Narrow members of a returned aggregate.** The caller stores the r0/r1
  pair member by member. A halfword or byte member's store of a wider value
  was promoted by `sra` as if it already fit, because the frontend types the
  stored value with the store's width. `sra` now asks the value's
  definition, and masks a value that is narrow only by its type. This also
  fixes the same miscompile on the base for plain `q.s = word` code.
- **`value = false` second pass.** In the inlined `setRangeValue`, the second
  pass stored back a word it had just reloaded. That store is removed
  (`ssa:ptr_silent_store`).
- **`peekNext`.** It expands in `get_file_header` again: the
  aggregate-return loop class is no longer charged to the nested-inline
  budget by its tokens.

One item remains. With `value = true` (`claim_range` in `alloc` and
`resize`), the inlined `setRangeValue` still reloads `self.masks` and the
word after the first store, then does a second `orr` and `str`. LLVM emits a
single `orr` and `str` instead.

One smaller cost came in with the r0/r1 return. `time_now_timespec` lands
its `tv_nsec` multiply in r0:r1 and then moves the result into place:
`umull r0,r1,..; mov r1,r0; mov r0,r4`. That is 2 extra instructions on each
of 2,950 calls (+5,900). An allocation hint that steers PACK64's sources
toward the return pair would remove the moves.

## Reproducer

TmpPageAllocator `alloc` in the 2026-10-14 battery kernel
(`.cache/kc4_bitmap-helpers-leftovers-rp3`):

```
ldr.w ip, [r2, #20]        ; self.masks, reloaded: the store may alias it
ldr.w r9, [ip, r4]         ; the word just stored
and.w sl, r8, r5
orr.w r5, r9, sl
str.w r5, [ip, r4]
```

`fill_range` in `tests/ir_tests/bug_ptr_silent_store.c` has the same shape
(`ldr r3,[r0,#4]; ldr r0,[r3,r6]; orr; str` after the first `str`).

## Root cause

`ssa:load_cse` (`opt/ssa/memory/load_cse.c`) kills a tracked load of
`self->masks` at any store through a pointer it cannot place, including the
store to `masks[i]`. The operands already carry the C11 6.5p7 marks
(`IROP_AUX_ALIAS_PTR`, set by `ir/gen/put.c`): the store writes a non-pointer
scalar, and the load reads a pointer object. Only LICM's field-pointer hoist
reads those marks.

## Regression lock

None for the remaining item. The fixed parts are locked by these tests:

- `bug_regpair_static_aggregate_return` (runtime, soft and hard float);
- `bug_regpair_static_by_name`, `bug_regpair_static_late_alias` (-O0) and
  `bug_regpair_extern_inline` (two TUs, `-fgnu89-inline`): reaching the
  function by asm name, after an inlined call, through a later alias, and
  through a GNU extern inline body that is later replaced;
- `bug_regpair_asm_global_in_body` (two TUs): `.global`/`.weak` in the asm of
  a deferred body after a direct call;
- `bug_regpair_narrow_member_const` and `bug_sra_narrow_store_wide_value`:
  narrow members stored from a wider or constant value;
- `test_regpair_static_return_*` and `test_regpair_late_declarations_keep_aapcs`
  in `test_codegen_asm.py`;
- `bug_ptr_silent_store` with `test_ptr_silent_store_drops_unchanged_word`;
- `bug_inline_sret_loop_nested_budget` with
  `test_sret_loop_leaf_not_charged_to_nested_budget`;
- `test_opt_pure_via_sret.c` (unit test).

## Likely fix

When both marks are present, `load_cse` should keep a tracked pointer load
across such a store, as `licm.c` does. The word reload then forwards from the
first store, and `ptr_store_dse` drops the first store: about 3 instructions
per `alloc`/`resize` (1,857 calls in the battery). Decide first whether tcc
may assume strict aliasing everywhere it compiles: the kernel, libc, and Zig
output.

# Zig path iterator family: `get_file_header`'s header walk runs out of low registers, `ComponentIterator.last` stays out of line

**Status:** open · **Severity:** performance · **Found:** 2026-10-12 (20:45
kernel_compare battery)

## What remains

Battery 2026-10-14 (base `fix-base-2026-10-14`, `.cache/kc4_base`; after
`fix4/componentiterator-leftovers`, `.cache/kc4_componentiterator-leftovers`; all 15
outputs and return codes the same).  `FileHeader.deinit` is fixed: its call sites now
expand its test (`inline:guard_head`) and the out-of-line body is never entered (67,896 ->
0); `get_file_header` grew by the moved test, 812,437 -> 822,475.  The `eor.w r1, r0, #1;
mov r0, r1` in `not_root` is no longer in the kernel.  Left:

- **`get_file_header` register pressure (822,475 over 662 calls).**  The header-walk
  loop's name slice is a coalesced class `T900`/`T902` over IR positions [426, 602], 112
  and 92 uses, crossing calls, narrow uses 7 and 5.  When the linear scan reaches it
  (`TCC_RA_TRACE_FUNC`, `-DTCC_LOG_LS=1`), `int_free` is `0x100e`: r4-r11 all belong to
  call-crossing classes allocated earlier, among them the searched-for name `T2661`/`T2662`
  [302, 1073] in r5/r7 with one narrow use each.  `T900` evicts a cold class from r9, so
  the hottest block, `ldrd r9, fp, [sp, #236]; cmp.w r9, #0; bne` (9,265 runs), cannot use
  `cbnz`.  That is +9,265 against a low register, and the class order, not a short
  temporary, decides it: a fix needs the scan to let a dense class displace a sparse one
  already holding r4-r7 (or a priority/splitting allocator), which `ra:hi_callee_pref`
  (decided per interval at its own start) does not do.  Same cause, 367 runs: the second
  scan's start index is stored to `[sp, #56]` and reloaded three times
  (`ldr r3`/`ldr r4`/`ldr.w fp, [sp, #56]`) at the scan's entry.
- **`ComponentIterator.last`** (43,072 over 323 calls; `TCC_INL_DBG` `slots=296 ops=52
  cost=35`): a loop leaf writing `*self`, 1,633 token ints, two call sites
  (`basenamePosix`, `dirname`, each on a local iterator).  The few-callers class takes it
  only under `FEW_CALLERS_INLINE_TOKENS` (512); with the cap at 4096 (and `decl.c` keeping
  a few-callers body's tokens past `post_opt_inline_cap`) it expands into both callers and
  the family drops 16.4k (`last` -43,072, `basenamePosix` +13,758, `dirname` +12,958), but
  40-odd other few-callers leaves come in too and the changed functions sum to +4.5k
  (`gen_vcall` of `UartFile.write_locked` +1.4k net, `load_from_template` +10.9k against
  `template_bases` -4.8k, `system_call_write_result` +14.2k).  A rule keyed to what pays
  here -- the argument is the address of a caller local the expansion scalar-replaces --
  is a call-site property the definition-time class does not see.

## Other shapes in this family (unchanged)

- `MountPoints.find_locked` (~203k, LLVM 173,273 incl.): `child.path` passed to the
  out-of-line `mem_eql` round-trips through the frame; same in gcc -O2 for the reduced shape.
  The header walk above shows it too: `strd r5, r7, [sp, #112]; ...; ldrd r2, r3, [sp, #112]`
  before every `mem_eql` (10,176 runs).  `mem_trim` (LLVM inlines and specializes `"/"`),
  see `split-byval-struct-param-loop-reloads.md` in git history.
- `ArrayList(u8).ensureUnusedCapacity` 66,331 (LLVM 31,604 incl.) and
  `appendSliceAssumeCapacity` 29,176: out-of-line call wrappers from `fs.path.resolve`.
- `vfmt_render` ~162k vs 125,097; `DeriveFromChain(RamFsDirectory)` 22,111 vs 12,857 (call
  overhead + a re-test of a known-zero flag).

## Reproducer

`docs/bugs/repro/kernel-componentiterator-inline-2026-10-12.c` (the original shape;
`count_components`/`last_len` are call-free leaves).  The register pressure needs a caller
as large as `get_file_header`: compile kernel.c (see `kernel-bottlenecks-2026-10-12.md`)
and read the `ldrd ..., [sp, #236]` block after the `mem_eql` loop in
`fs_romfs_romfs_RomFs__struct_10661_get_file_header`.

## Regression lock

`tests/ir_tests/bug_componentiterator_inline.c`, `bug_loop_leaf_wrapper_inline.c`,
`bug_ra_coalesce_contained_iv_copy.c`, `bug_known_ext_bool_select.c`,
`bug_guard_head_partial_inline.c` (runtime, all levels);
`test_codegen_asm.py::test_componentiterator_by_value_methods_inline`,
`test_componentiterator_retains_size_policy`, `test_loop_leaf_wrapper_inlines`,
`test_ra_coalesce_contained_iv_copy`, `test_known_bool_select_is_a_copy`,
`test_guard_head_tests_before_the_call`, `test_guard_head_rejects_unsafe_shapes`,
`test_guard_head_retains_policy`.  The remaining parts have no lock.

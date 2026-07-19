# Known bugs

When adding a new entry, include the affected files, the current behavior, and
the regression test that pins it. Remove the entry once the bug is fixed and the
test asserts the corrected behavior. Detailed reports live in
[`docs/bugs/`](bugs/).

## Open

- [volatile local reads folded to a constant](bugs/volatile-local-folded-to-constant.md)
  — const propagation (SSA `var_imm_prop`/`var_const_fold` + legacy `const_prop`)
  drops the mandated volatile loads for a single-def `volatile` local; the
  `blocked` set omits the `is_volatile` guard that DSE already has.
  Correctness/miscompile, pre-existing.

- Dereferencing an integer-constant address yields the address, not the loaded
  value: `*(volatile unsigned *)0xE000E018` compiles to `ldr r0,[pc,#..]` (the
  literal) with no `ldr r0,[r0]`. Affects `-O0` through `-O2`, volatile and
  non-volatile alike; a deref through a pointer *variable* is correct, so only
  the constant-address form is broken. Reproduces identically on `mob`, so it is
  pre-existing and not optimizer-related. Blocks the bare-metal MMIO pattern; no
  test in the corpus dereferences a literal address, which is why it went
  unnoticed. Correctness/miscompile, pre-existing, unpinned.

- `tcc_ir_gen_cvt_ftof`/`tcc_ir_gen_cvt_itof`/`tcc_ir_gen_cvt_ftoi` read the
  conversion source from *below* the top of stack: `tcc_ir_gen_f()`
  (ir/gen/float.c:200) calls `tcc_ir_put(ir, op, &vtop[-1], &vtop[0], &dest)`
  for the unary CVT_* ops (whose irop_config has no src2), so the emitted
  instruction's src1 is the stale slot `vtop[-1]`, the `&vtop[0]` argument is
  silently ignored, and the result is written into `vtop[0]` with nothing
  popped. Under the natural one-value cast convention (the value to convert
  sits at `vtop`, exactly what `gen_cast()`'s inline expansion assumes when it
  passes `vtop` as src1, tccgen.c:9945/9961) the instruction converts whatever
  stale value lies beneath. Additionally the itof/ftof branches derive
  `dest.type` from `vtop[0].type` (ir/gen/float.c:176,188), which under that
  convention is the *source* type, so `tcc_ir_gen_cvt_itof` would mark an int
  source's dest incorrectly. Latent — none of the three wrappers has any
  caller in the product (the frontend reaches float.c only via
  `tcc_ir_gen_f()`, and gen_cast inlines its own conversion `tcc_ir_put`).
  Not yet fixed. Regression lock:
  `tests/unit/arm/armv8m/test_gen_float.c`,
  `test_cvt_itof_single_value_stack_reads_stale_slot_below` pins the current
  buggy behavior — flip its assertions once float.c passes `&vtop[0]`.

- [load_cse: pointer-deref store source tracked as the stored value](bugs/load-cse-lval-store-src-tracked-as-value.md)
  — `ssa_opt_load_cse`'s two `sstore_track_vr` call sites
  (`source/opt/ssa/memory/load_cse.c`:1031 and :1076) record a TEMP store
  source without the `!src.is_lval` guard their three sibling trackers have
  (:302, :1104, :1181), so the fused mem-copy form `StackLoc <- *p` /
  `*q <- *p` tracks the pointer `p` as the slot's value; a later LOAD of the
  slot is rewritten to `ASSIGN p` (the address, not the pointee).
  Correctness/miscompile. Regression locks:
  `tests/unit/arm/armv8m/test_ssa_opt_load_cse.c`
  `test_stack_store_lval_vreg_src_tracked_bug` and
  `test_temp_indir_store_lval_src_tracked_bug` pin the current buggy
  behavior — flip their assertions once fixed. Not yet fixed.

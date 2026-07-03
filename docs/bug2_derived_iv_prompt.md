# Next-session prompt: fix bug #2 (`transform_derived_iv` derived-IV strength reduction)

> **RESOLVED 2026-07-02 — kept for historical reference.** The pass is fixed
> and re-enabled; see `docs/bugs.md` #2 for the full write-up. The actual root
> cause was NOT in the transform (its IR output was correct): the miscompile
> came from `tcc_ir_opt_cmp_stack_addr_fold`'s stack-address resolver crossing
> control-flow merge points (deleting the single-trip varargs9 loop's only
> exit test). Fixes: merge-sound `ir_resolve_stack_addr_value_ex`
> (ir/opt_constprop.c), a taint-based escape analysis replacing `feeds_mem`
> over the FULL loop body (`sr_div_value_stays_in_regs`), and removal of the
> unreachable/unsound shared-pointer path. All acceptance criteria below were
> met (torture 11201, primary 1908, ut 2342, golden 21, fuzz 0–2000 clean;
> regression test `tests/ir_tests/258_derived_iv_strength_reduction.c`).

Paste the block below into a fresh session. It is self-contained; everything a
new agent needs to pick this up cold is here. It reflects what was learned in
the 2026-07-02 miscompile-hunting sessions (see `docs/bugs.md` #2 and the
in-code comment at the top of `transform_derived_iv` in `ir/opt_loop_utils.c`).

---

## Task

Re-enable and correctly fix **derived-IV strength reduction**
(`transform_derived_iv` in `ir/opt_loop_utils.c`), which is currently disabled
by an unconditional `return 0;` at the top of the function. It must be
re-enabled **without introducing any miscompile**. This is a *redesign* task,
not a point fix — treat it with full miscompile-hunting rigor
(`docs/debugging_fuzz_divergences.md`). Do **not** ship it unless the full
regression + a fuzz sweep are clean.

Bug #11 sibling context: the analogous pass #7 (`tcc_ir_hoist_pure_calls`) was
fixed and re-enabled in the same era — that one had specific, self-contained
defects. #2 is harder: its index bookkeeping is fragile and it has a history of
"linker heap corruption," so budget for restructuring, not patching.

## Where it lives

- Function: `int transform_derived_iv(...)` in `ir/opt_loop_utils.c` (~130 lines
  of rewrite logic below the disabling `return 0;`).
- Disabled by: a plain `return 0;` immediately after the out-param
  initialization at the top of the function. (Do NOT use a `(void*)1` sentinel
  to "disable differently" — it trips GCC `-Werror=array-bounds` on the later
  `*out_ptr_vreg = ...` writes; that's why it's a plain early return.)
- Caller: `iv_strength_reduction_core()` (invoked from `ir/opt_loop.c:190,206,220`),
  which does the `APPLY_SHIFT` index bookkeeping around the transform's
  `out_idx_shift` / `out_postnop_origpos` / `out_stride_pos` return values.
- Detection of derived IVs (regular ADD-based and INDEXED forms) is in the same
  file above `transform_derived_iv` (grep `Found DIV`, `Found INDEXED-DIV`,
  `Found MLA-DIV`).

## Confirmed reproduction

`gcc.c-torture/execute/va-arg-24.c` **miscompiles at -O1** (QEMU exit code 1;
O0 and O2 pass). Steps:

1. Remove the disabling `return 0;` (and its comment) at the top of
   `transform_derived_iv`.
2. `make cross`
3. `cd tests/ir_tests && python -m pytest test_gcc_torture_ir.py -k "va-arg-24" -q`
   → `va-arg-24-O1` FAILS ("Test exited with code 1"); O0/O2 pass.

The failing loop (macro-expanded, per varargs function):
```c
for (i = x + 1; i <= 10; i++)
    n[i] = va_arg (ap, int);   /* n[] is a local int[11]; ap is the va_list */
verify (..., n);               /* checks n[i] == i for all i */
```

## What was already root-caused (2026-07-02)

Method: compile va-arg-24.c at -O1 with `-dump-ir`, once with the pass enabled
and once disabled, and `diff` the `=== IR AFTER OPTIMIZATIONS ===` sections
(the pass runs between "AFTER LOOP ROTATION" and "AFTER OPTIMIZATIONS").

Findings:

1. **The transform fires on the array-element address.** `&n[i]` is
   strength-reduced into a pointer IV: init `ptr = &n[x+1]`, stride `+4`, loop
   guard `ptr <U &n[0]+40`, and the store becomes `*ptr = <va_arg value>`. The
   transformed IR *looks* structurally correct at a glance (right start address,
   right stride, right trip count), yet the compiled program computes wrong
   values — so the fault is in a **downstream interaction** (copy-prop / DCE
   merging the address temp into the pointer and dropping a deref or the stride,
   and/or the register-allocation / va_list interaction), exactly as the
   in-code comment above the disable warns.

2. **The `feeds_mem` guard is incomplete.** It is meant to skip DIVs whose
   address feeds a memory access (the backend already forms efficient indexed
   `LDR/STR rN,[rb,rm,LSL#k]` addressing, so nothing is lost by skipping). But
   va-arg-24's DIV is a **non-indexed address-temp ADD** — `div->use_idx` points
   at `T = base + (i<<2)` (op `TCCIR_OP_ADD`, with `shl_idx` = the feeding SHL,
   `stride=4`), NOT at a `STORE_INDEXED`. The `feeds_mem` scan checks whether the
   ADD's dest (`ud_vr`) is the lval dest/src of a STORE/LOAD in the loop body
   (`sr_vreg_is_ud_or_offset`), but va-arg-24's connection between the ADD result
   and the actual store escapes that scan, so it does not bail.

3. **Shared-path / general-path asymmetry (already fixed defensively).** The
   general path bails on `STORE_INDEXED`/`LOAD_INDEXED` uses via `feeds_mem`; the
   shared-pointer fast path (`shared_ptr_vreg >= 0`) rewrote
   `STORE_INDEXED->STORE` / `LOAD_INDEXED->LOAD` with **no** such check. A guard
   was added so both paths are consistently conservative — it is present but
   **dormant** (the function still returns 0 early). Keep it.

## Suggested approaches (pick one, or better)

- **(A) Make the memory-feeding detection sound.** Redesign `feeds_mem` (or add a
  use-def pass) so a DIV is skipped whenever its computed address value reaches
  ANY dereference/store/load in the loop — including the non-indexed
  address-temp ADD case va-arg-24 exercises. This preserves the pass for genuine
  non-memory derived IVs (address used only in further pointer arithmetic) while
  guaranteeing correctness for memory-feeding ones. Lowest-risk direction.
- **(B) Restrict scope.** Only transform DIVs whose address is provably never
  dereferenced (used purely in more pointer arithmetic that is itself not a
  memory address). Simpler to prove correct; may leave value on the table.
- **(C) Fix the downstream interaction.** If (A)/(B) show the transformed IR is
  actually valid and the fault is later (copy-prop merging the address temp into
  the pointer and dropping a deref/stride), fix that pass instead. Higher effort;
  confirm with a `bisect_opt.py` run which knob actually corrupts the value.

Whichever you choose, also re-check the fragile index bookkeeping
(`use_idx` / `shl_idx` / `new_use_idx` / `out_stride_pos` / `out_postnop_origpos`
and the caller's `APPLY_SHIFT`) — the "heap corruption" history points at
off-by-one shifts when multiple DIVs / calls interleave.

## Tools

- `-dump-ir` flag (build already has `CONFIG_TCC_DEBUG`): dumps IR
  before-opt / after-loop-rotation / after-opt per function. IR-diff
  enabled-vs-disabled is the fastest way to see the exact rewrite.
- `make cross CFLAGS+='-DTCC_LOG_IV_SR=1'` for `LOG_IV_SR` tracing of the pass
  (or add a temporary unconditional `fprintf(stderr, ...)` — more reliable if
  the CFLAGS override drops other flags).
- `scripts/bisect_opt.py` — QEMU-confirmed culprit knob + the exact IR line
  where a value is misfolded (see `docs/debugging_fuzz_divergences.md`).
- `scripts/diff_olevels.py --count N --start M` — O0/O1/O2 self-consistency
  fuzz sweep. NOTE: pre-existing divergences at seeds 193, 222, 477, 555, 591
  (and 1136, 1259, 1371, 1378, 1522, 1820 in 800–2000) are **backend
  literal-pool / regalloc compile-failures that fail at -O0** — unrelated to the
  optimizer. Filter them by checking whether `-O0` compiled; only an O0-compiles-
  but-O1/O2-diverges result implicates an optimizer change.

## Acceptance criteria (all must hold with the pass ENABLED)

1. `va-arg-24` passes at O0/O1/O2:
   `cd tests/ir_tests && python -m pytest test_gcc_torture_ir.py -k "va-arg-24" -q`
2. Full gcc-torture IR execute suite: `python -m pytest test_gcc_torture_ir.py -q -n auto` — 0 failures (baseline 11201 pass).
3. Primary IR suite: `python -m pytest test_qemu.py -q -n auto` — 0 failures (baseline 1904 pass).
4. Host unit tests: `make ut` — 0 failures. Re-enable/rewrite the two disabled
   tests in `tests/unit/arm/armv8m/test_opt_loop_utils.c`
   (`test_transform_derived_iv_always_returns_zero`,
   `test_transform_derived_iv_shared_path_also_disabled`) to assert the new
   behaviour, and add a positive test that a non-memory derived IV IS reduced.
5. `scripts/diff_olevels.py --count 2000 --start 0` — no NEW divergences beyond
   the pre-existing O0 backend failures listed above.
6. Add a project IR regression test under `tests/ir_tests/` (register in
   `test_qemu.py` `TEST_FILES`) that reduces the va-arg-24 array-store-in-loop
   pattern and would produce a wrong checksum if the DIV were mis-transformed.
   (Avoid `static __attribute__((pure))` + `--gc-sections`: that pattern hits an
   unrelated pre-existing "undefined symbol" linker bug.)
7. Update `docs/bugs.md` #2 to FIXED with the validation numbers, and replace the
   disabling comment in `ir/opt_loop_utils.c`.

## If it can't be made correct

If (A)–(C) don't yield a provably-correct re-enable within scope, leave it
DISABLED (the current safe state) and record the additional findings in
`docs/bugs.md` #2 and the in-code comment — do not ship a partial fix. A
disabled missing-optimization is strictly better than a miscompile.

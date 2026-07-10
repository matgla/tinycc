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

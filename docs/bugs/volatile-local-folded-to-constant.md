# volatile local reads folded to a constant

**Status:** open · **Severity:** correctness (miscompile) · **Found:** 2026-07-09
· **Pre-existing:** yes (reproduces with legacy `const_prop` on and off)

## Summary

Constant propagation folds reads of a `volatile`-qualified **local** variable to
the constant it was initialised with, eliminating the volatile memory accesses
that C requires (C11 6.7.3p7 / 5.1.2.3: every access to a volatile object is a
side effect that must not be optimised away).

## Reproducer

```c
int ext(int);
int f(int c)
{
  volatile int x = 5;
  if (c)
    return ext(x);   /* must LOAD x from memory */
  return x;          /* must LOAD x from memory */
}
```

`armv8m-tcc -O1 -dump-ir -c` (IR after optimization):

```
0000: TEST_ZERO R0(P0)
0001: JMP to 5  if "=="
0002: PARAM0[call_0] #5      <-- x read folded to #5, no load
0003: CALL GlobalSym(1188) --> R0(T0)
0004: RETURNVALUE R0(T0)
0005: RETURNVALUE #5         <-- x read folded to #5, no load
```

Both the store `x = 5` and the two reads should survive as real memory accesses;
instead the reads become immediates and the slot store is dead-stored away.

## Root cause

VAR constant propagation treats a single-def local as a propagatable constant
without excluding `volatile`-qualified slots. The per-VAR "blocked" set only
excludes address-taken (and complex) locals, not volatile ones:

- SSA `ssa_opt_var_imm_prop` — [ir/opt/ssa_opt_cprop.c](../../ir/opt/ssa_opt_cprop.c)
  builds `blocked[]` from `variables_live_intervals[v].addrtaken` only
  (~line 1912); no `is_volatile` check. Same gap applies to the `ASSIGN #imm`
  def acceptance in `ssa_opt_var_const_fold`.
- Legacy `ssa`/flat `tcc_ir_opt_const_prop` —
  [ir/opt_constprop.c](../../ir/opt_constprop.c) marks a def non-constant on
  `interval->addrtaken || interval->is_complex` (~line 2032) but not
  `interval->is_volatile`, then propagates the immediate (~line 2043 accepts
  `ASSIGN|STORE` immediate defs).

The interval already carries the needed flag: `IRLiveInterval.is_volatile`
([tccir.h](../../tccir.h) line ~317). The precedent guard exists in
[ir/opt/ssa_opt_dce.c](../../ir/opt/ssa_opt_dce.c) line ~1854
(`if (!iv || iv->addrtaken || iv->is_volatile)`), which correctly excludes
volatile slots from dead-store elimination — the const-prop passes are missing
the symmetric check.

**Scope is broader than the SSA passes.** With `const_prop` force-disabled, the
reproducer still folds to `#5` after individually disabling *each* of
`ssa:sccp`, `ssa:cprop`, `ssa:var_const_fold`, `ssa:var_imm_prop`
(`TCC_DISABLE_PASS=<name>`), so the fold is also performed upstream — a flat
pre-SSA pass (`const_var_prop` / `value_tracking` / `copy_prop`) or IR
generation itself. A complete fix must guard `is_volatile` at every VAR-const
producer, not only the SSA ones. (The SSA `var_imm_prop` / `var_const_fold`
guards are already in place on `legacyOptRemoval` — they stop those paths from
folding volatile slots but do not by themselves fix the observable case.)

## Proposed fix

Exclude volatile VAR slots from constant propagation, mirroring the DSE guard:

- SSA: when building `blocked[]` in `ssa_opt_var_imm_prop` (and the def gate in
  `ssa_opt_var_const_fold`), also block when
  `variables_live_intervals[v].is_volatile`.
- Legacy: in `tcc_ir_opt_const_prop`, treat `interval->is_volatile` like
  `addrtaken`/`is_complex` (set `is_constant = 0`).

Audit sibling VAR-const consumers (`sccp`, `var_const_fold`, `value_tracking`,
`known_bits`) for the same omission.

## Regression test

Add an IR test asserting the two reads of `x` remain memory loads (not `#5`
immediates) at `-O1`/`-O2` — e.g. the reproducer above, checking the disasm/IR
retains loads from the `x` slot.

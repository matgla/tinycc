# 01 — `const_prop_tmp` does not fold IMOD/UMOD/DIV/UDIV/PDIV

**Status:** FIXED in this branch ([ir/opt_constprop.c:4340-4378](../ir/opt_constprop.c#L4340-L4378))
**Severity:** Medium — blocks bigger cascades, not a miscompile.

## Symptom

`const_prop_tmp`'s two-immediate fold table in [ir/opt_constprop.c:4294-4353](../ir/opt_constprop.c#L4294-L4353) covers
`ADD`/`SUB`/`AND`/`OR`/`XOR`/`SHL`/`SHR`/`SAR`/`ROR`/`MUL`/`UMULL`/`UBFX`
but **not** integer division/remainder. After propagation, an op like
`T11 <-- #-13 IMOD #61` stays in the IR with both operands as immediates
and never folds to `T11 <-- ASSIGN #-13`.

## Repro

`tests/gcctestsuite/.../gcc.c-torture/execute/bitfld-1.c` at `-O2`. The
"AFTER LOOP ROTATION" dump shows:

```
0008: T11 <-- #-13 IMOD #61
0009: CMP T11,#-13
0010: JMP to 13  if "=="
0011: FUNCPARAMVOID  FUNCPARAMVOID #131072
0012: CALL GlobalSym(1137) CALL #131072   ; abort()
```

`T11` should fold to `#-13`, `CMP` to a tautology, JMP to unconditional,
and `abort()` to dead code that DCE removes.

## Why it matters

Beyond the static fold itself, this stalls **all downstream cascades**:
the `CALL abort()` between a stack STORE and a later stack read keeps
`sl_forward` from forwarding the stored value (it conservatively assumes
a call may clobber memory). Without the fold, the call stays, and the
read-after-store chain never collapses.

## Fix

Extend the fold switch with:

```c
case TCCIR_OP_DIV:
case TCCIR_OP_PDIV:
case TCCIR_OP_UDIV:
case TCCIR_OP_IMOD:
case TCCIR_OP_UMOD:
```

each handling `v2 == 0` (and `INT64_MIN / -1` for the signed variants) by
setting `ok = 0` so the fold is skipped on UB inputs.

## Related

- [[02]] — without `known_bits`, the operands of these IMOD/UMODs would never *become* both-immediate in the first place. Both bugs together gate the bitfld-1 cascade.
- [[04]] — even after this fold fires, the downstream cleanup needs the pipeline to keep iterating.

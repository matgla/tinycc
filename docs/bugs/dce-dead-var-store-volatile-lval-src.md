# dce_dead_var_stores drops a store whose fused lval source is a volatile read

**Status:** open · **Severity:** correctness (miscompile) · **Found:** 2026-07-19
(unit tests for `source/opt/ssa/dce/`)

## Summary

`dce_dead_var_stores` eliminates a `STORE` to a dead VAR slot without checking
whether the stored **value** is itself a volatile memory read. For the fused
memory-to-memory copy form `STORE lval(V0slot) <- lval(V1)`, the store
instruction *embodies* the read of `V1`; when `V1` is volatile that read is a
mandated side effect (C11 5.1.2.3) and must survive even though `V0` is never
read.

## Root cause

The second scan loop in `dce_dead_var_stores`
([source/opt/ssa/dce/dead_var_stores.c](../../source/opt/ssa/dce/dead_var_stores.c)
lines ~164-187) NOPs any `STORE` whose dest is a non-volatile, non-address-taken,
unused VAR slot. The volatile pre-mark at lines 62-64 covers only the
**destination** slot's interval:

```c
for (int p = 0; p < num_vars && p < ir->variables_live_intervals_size; p++)
  if (ir->variables_live_intervals[p].is_volatile)
    var_used[p / 8] |= (1 << (p % 8));
```

Nothing inspects the source operands of the store being killed. The two
sibling passes have the guard this one is missing:

- `vl_removable` in [var_liveness.c](../../source/opt/ssa/dce/var_liveness.c)
  lines ~84-101 — rejects removal when any src's live interval `is_volatile`;
- `dce_temp_worklist` in [temp_worklist.c](../../source/opt/ssa/dce/temp_worklist.c)
  lines ~42-50 — keeps a dead def whose dest slot is volatile.

`dce_var_liveness` would keep the store (via `vl_removable`), but
`dce_dead_var_stores` runs earlier in `ssa_opt_dce` and kills it first.

## Observable symptom

Unit-level IR shape (confirmed in the UT11 harness):

```c
/* intervals: V1.is_volatile = 1, V0 never read */
STORE lval(local V0) <- lval(V1)   /* fused copy: embodies a read of V1 */
```

`ssa_opt_dce` at -O1 NOPs the store, deleting the volatile access to `V1`.

**Caveat:** confirmed at pass level with a hand-built fixture; an end-to-end
C reproducer through `armv8m-tcc` has not been established yet. The fused
lval-source store form is real (see the ptr fuzz seed 7226 note in
[load-cse-lval-store-src-tracked-as-value.md](load-cse-lval-store-src-tracked-as-value.md));
whether the frontend/fusion passes emit it with a volatile-qualified source
needs verification when fixing.

## Regression lock

`tests/unit/arm/armv8m/test_ssa_opt_dce.c`:

- `test_dce_dead_var_store_volatile_lval_src_bug` pins the elimination.

It asserts the CURRENT (buggy) result; flip its assertions to "store kept,
changed == 0" once fixed. Not yet fixed.

## Likely fix

In the elimination loop of `dce_dead_var_stores`, before NOP'ing a store,
reject it when any source operand is an lval whose vreg's live interval is
volatile — mirroring `vl_removable`:

```c
if (q->op == TCCIR_OP_STORE && store_has_volatile_lval_src(ir, q))
  continue;
```

The sibling audit is worth doing at the same time: `dce_dead_stackloc_stores`
kills unread anonymous-slot stores and likewise ignores volatile sources, and
`dce_dead_overwrite_stores` / `dce_dead_global_stores` kill *earlier* stores —
a killed earlier store whose value was a volatile read has the same problem.

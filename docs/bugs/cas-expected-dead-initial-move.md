# An inline CAS's `expected` local keeps its dead initial move

**Status:** open · **Severity:** performance · **Found:** 2026-10-14 (reading the -minline-atomics CAS shapes after the twin-compare threading) · **Reported by:** claude-opus-5-5

## Summary

`uint32_t e = 0; if (!cas(l, &e, t)) use(e);` (`-O1`/`-O2`/`-Os`,
`-minline-atomics`): the CAS reads its expected value as the constant 0, and the
failure path writes the old value back into `e` before `use(e)` reads it. On
every path `e = 0` is overwritten or never read, but the move stays:

```
br_trylock (tests/ir_tests/bug_atomic_cas_result_branch.c, -O2 -minline-atomics):
  movs   r2, #0          ; e = 0: dead
  ldaex  r2, [r0]
  cbnz   r2, fail
  strex  r3, r1, [r0]
  cbz    r3, ok
  ...
fail:
  clrex
  rsb    r0, r2, #0      ; -(int)e, the written-back old value
```

The same shape: `void tc(uint32_t *l, uint32_t t) { uint32_t e = 0; if (cas(l, &e, t)) return; g(e); }`
(`R2(T7) <-- #0` at IR 0, `T7 <- T4` on the failure path, `PARAM0 T7`).
One instruction per such CAS. The kernel's spin locks do not have the shape
(their expected value is not read after a failure).

## Reproducer

`br_trylock` in `tests/ir_tests/bug_atomic_cas_result_branch.c`, `-O2
-minline-atomics`: the `movs r2, #0` before the loop.

## Root cause

`source/ir/dead_def.c` `tcc_ir_dead_def` drops a TEMP definition when nothing
else names the TEMP, or when the TEMP is redefined later *in the same straight
run of code* (`dd_overwritten`). `T7` is named again by the failure path's
write-back and use, which sit past the CAS's branch: a different run, so the
`#0` survives although no path reads it.

## Regression lock

None.

## Likely fix

Let `dd_overwritten` follow branches (a liveness walk like
`ra:branch_thread`'s `bt_live_at`): a pure definition whose value no path reads
before the next whole definition goes.

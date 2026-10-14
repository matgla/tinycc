# A nested function's trampoline reads the frame of the latest activation of its parent

**Status:** open · **Severity:** correctness (miscompile, every level) · **Found:** 2026-10-14 (probing nested-function shapes for `fix5/nested-frame`; base `f6d53136` too) · **Reported by:** claude-opus-5-5

## Summary

Taking the address of a nested function stores the parent's frame in one
static chain slot per nested function (`__chain_<name>` in `.data`), and the
trampoline loads the chain from there.  A recursive parent (or a parent
re-entered from another thread or interrupt) overwrites the slot, so a
pointer made by an outer activation calls the nested function with the
innermost activation's frame:

```c
int call(int (*f)(void)) { return f(); }
int rec(int d)
{
  int mine = d;
  int get(void) { return mine; }
  int (*fp)(void) = get;
  int below = d > 0 ? rec(d - 1) : 0;
  return call(fp) * 10 + below;
}
```

## Reproducer

`repro/nested-fn-trampoline-chain-slot-not-reentrant.c`.  Expected (gcc)
`60`; tcc, every level: `0`.

## Root cause

`setup_nested_func_trampoline` (`frontend/gen/nested/nested.c`) allocates a
single 4-byte chain slot in `.data` per nested function, `INIT_CHAIN_SLOT`
stores the chain there when the address is taken, and the trampoline
(`gen_nested_func_trampoline`) is static code that loads it.  Nothing is
per-activation.

## Regression lock

None.

## Likely fix

Give each activation its own trampoline or descriptor: a small stack block
in the parent's frame holding the chain and the target (gcc's stack
trampolines need an executable stack, which YasOS may not allow), or a
descriptor `{code, chain}` that function-pointer calls recognise.  Either is
a design change, not a local fix.

# A nested function cannot call through its parent's function-pointer local

**Status:** open · **Severity:** correctness (compile error, every level) · **Found:** 2026-10-14 (probing nested-function shapes for `fix5/nested-frame`; base `f6d53136` too) · **Reported by:** claude-opus-5-5

## Summary

A GNU nested function that calls through a function-pointer local of its
parent does not compile:

```c
int g(int x) { return x + 1; }
int f4(int k)
{
  int (*sp)(int) = g;
  int n(int y) { return sp(y) + k; }
  return n(4);
}
```

```
warning: assignment makes pointer from integer without a cast
error: too few arguments to function
```

The same capture of an `int *` or a struct works, and so does `sp` declared
at file scope.

## Reproducer

`repro/nested-fn-captured-function-pointer-type.c`. Expected (gcc) `8`; tcc
fails to compile it at every level.

## Root cause

Not traced.  Probably: `prescan_captured_vars` (`frontend/gen/nested/nested.c`)
copies the parent symbol's `CType` into `NestedFunc::captured_types`, and the
nested function is compiled only after the parent's body, when the parent's
block-scope symbols are popped.  A pointer-to-function type's `ref` (the
function type `Sym` with its parameter list) lives on the parent's
`local_stack` and has been freed and reused by then; the nested function's
captured `sp` then has a garbage function type.  `compile_nested_functions`
already re-links the parent's typedefs and struct tags for the same reason,
but not anonymous type `Sym`s.

## Regression lock

None.

## Likely fix

Keep the type `Sym`s a capture's type refers to alive until the nested
functions are compiled (copy the type, or take them off the stack the way
completed struct tags survive `pop_local_syms`).

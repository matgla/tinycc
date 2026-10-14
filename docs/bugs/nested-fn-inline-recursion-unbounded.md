# Inlining a nested function that calls its enclosing nested function never ends

**Status:** open · **Severity:** correctness (compile hang, -O2/-Os) · **Found:** 2026-10-14 (adversarial probing for `fix5/nested-scope`; base `f6d53136` the same) · **Reported by:** claude-opus-5-5

## Summary

A GNU nested function `l2`, defined inside the nested function `l1`, calls
`l1`. At -O2 and -Os the auto-inliner expands `l1` into `l2` into `l1` and
so on. The compile does not finish (killed after minutes; it is still in
`unary_funcall_body` -> `nested_callee_captures_reachable`, dozens of
expansions deep). -O0 and -O1 compile it.

## Reproducer

`repro/nested-fn-inline-recursion-unbounded.c`:

```c
__attribute__((noinline)) int multi(int a)
{
  int l1(int y)
  {
    int l2(int z) { return z + (y > 0 ? l1(y - 1) : 0); }
    return l2(y * 10);
  }
  return l1(2) + a;
}
```

| compiler | result |
|---|---|
| gcc | `31` |
| tcc -O0 / -O1 | compiles (the -O0 value is not checked here) |
| tcc -O2 / -Os (base `f6d53136`, `fix5/nested-scope`) | does not finish |

## Root cause

`frontend/gen/builtin/call.c`, the nested-inline gate of
`unary_funcall_body` (around line 1968):

```c
(!tcc_state->in_inline_expansion ||
 call_func_sym->a.nested_func ||
 (tcc_state->inline_expansion_depth < INLINE_NEST_MAX &&
  !inline_expansion_in_progress(call_func_sym) && ...))
```

A nested callee skips both the `INLINE_NEST_MAX` depth cap and the
`inline_expansion_in_progress` cycle check, so an `l1`/`l2` cycle expands
until something else runs out.

## Regression lock

None.

## Likely fix

Put nested callees under the same cycle check and depth cap. The unmerged
`fix4/inliner-shadow-nested` (1ac04511) did that and its review then found
that recursive nested functions which read a captured aggregate took the
out-of-line path, which was broken
(nested-fn-aggregate-capture, fixed in round 5: `tests/ir_tests/bug_nested_parent_aggregate_capture.c`): retest that
first or together.

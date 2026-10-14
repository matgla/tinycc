# A nested function used as a `cleanup` handler is called without the static chain

**Status:** open · **Severity:** correctness (miscompile, every level) · **Found:** 2026-10-14 (adversarial probing for `fix5/nested-scope`; base `f6d53136` the same) · **Reported by:** claude-opus-5-5

## Summary

`__attribute__((cleanup(done)))` where `done` is a GNU nested function that
reads or writes its parent's locals: the cleanup call at the end of the
scope does not set the static chain (`sl`), so `done` reads its captures
through whatever `sl` holds. A captured parameter reads as 0 and a write to
a captured local is lost.

## Reproducer

`repro/nested-fn-cleanup-call-no-static-chain.c`:

```c
int acc;
__attribute__((noinline)) int cleanup_nf(int a)
{
  void done(int *p) { acc += *p * 10 + a; }
  {
    int x __attribute__((cleanup(done))) = 4;
    x++;
  }
  return acc;
}
__attribute__((noinline)) int cleanup_nf2(int a)
{
  int acc2 = 0;
  void done(int *p) { acc2 += *p; }
  {
    int x __attribute__((cleanup(done))) = 4;
  }
  return acc2;
}
```

| compiler | output |
|---|---|
| gcc | `52 4` |
| tcc, every level (base `f6d53136`, `fix5/nested-scope`) | `50 0` |

## Root cause

`frontend/gen/stmt/cleanup.c:try_call_scope_cleanup` emits the cleanup as
`FUNCPARAMVAL` + `FUNCCALLVOID` itself. The `SET_CHAIN` that
`frontend/gen/builtin/call.c` (around line 503) emits before a call to a
nested function, and the `nb_real_calls` count it keeps for the callee, are
not done there. `try_inline_cleanup_call` declines nested functions, so
every nested cleanup takes this path.

## Regression lock

None.

## Likely fix

Share the nested-callee part of the call path (SET_CHAIN, or no chain for a
sibling, and `nb_real_calls`) with the cleanup call.

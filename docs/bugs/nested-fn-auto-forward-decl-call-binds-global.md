# A call to a nested function before its definition, through an `auto` forward declaration, binds to a global function

**Status:** open · **Severity:** correctness (wrong symbol; link error, or a silent call of an unrelated global of that name), every level · **Found:** 2026-10-14 (round-5 review of `merge/lc-2026-10-14-r5`; base `f6d53136` the same) · **Reported by:** claude-opus-5-5

## Summary

GNU C lets a nested function be forward-declared with `auto` and called
before its definition. tcc binds a parent-body call or address-of made
between the `auto` prototype and the definition to the prototype's
**external** Sym: the object references the global symbol `later` (not the
mangled `later.N`) and passes no static chain. With no global `later` the
link fails; with one, the parent calls the wrong function.

A call after the definition, and calls from nested functions defined
after the prototype (`nested_define`'s `nested_protos` rebinding), are fine.

## Reproducer

`repro/nested-fn-auto-forward-decl-call-binds-global.c`:

```c
__attribute__((noinline)) int p(int x)
{
  auto int later(int);
  int r = later(2);                 /* binds to global `later` */
  int later(int v) { return v + x; }
  return r + later(x);              /* fine: the nested one */
}
```

| compiler | result |
|---|---|
| gcc | `8 17` |
| tcc, `-O0`/`-O1`/`-O2`/`-Os` (base `f6d53136`, merge `ad155d19`) | `tcc: error: undefined symbol 'later'` |

`-c` output shows `R_ARM_THM_JUMP24 later` in `p`. Taking the address
instead (`int (*fp)(int) = later;` before the definition) leaves
`R_ARM_ABS32 later`: no trampoline either.

## Root cause

- `frontend/gen/decl/btype.c` (`parse_btype`) drops `TOK_AUTO` without
  recording it, so a block-scope `auto` function prototype is parsed as an
  ordinary `extern` one.
- `frontend/gen/decl/decl.c` (~line 1505) declares it with `external_sym`
  and only notes it with `nested_note_block_proto(v)`. The parent body keeps
  generating code as it parses, so expressions before the definition resolve
  `later` to that external Sym (no `a.nested_func`, no `asm_label`
  mangling, so `call.c` emits no `SET_CHAIN`).
- The definition (`decl.c` ~line 1140) pushes a **new** Sym on
  `global_stack`; `nested_define` rebinds the name and patches the
  `def_vis` of sibling nested functions, but the parent's already-emitted
  references keep the external Sym.

## Regression lock

None.

## Likely fix

Record `auto` in `parse_btype` (an `AttributeDef`/storage flag). For a
block-scope `auto` function prototype, create the nested function's Sym up
front (static, `a.nested_func`, mangled `asm_label`, linked with
`nested_link` at the block's scope) and have the later definition in the
same block adopt that Sym instead of `sym_push2`-ing a fresh one, so calls
and address-of before the definition already get the static chain /
trampoline. An `auto` prototype with no definition in the block should be an
error (gcc: "nested function declared but never defined").

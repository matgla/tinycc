# A nested function naming more than 32 parent locals, statics, typedefs or enum constants loses the rest

**Status:** open · **Severity:** correctness (valid code rejected: compile error), every level · **Found:** 2026-10-14 (round-5 review of `merge/lc-2026-10-14-r5`; base `f6d53136` the same) · **Reported by:** claude-opus-5-5

## Summary

`NestedFunc` keeps its captures in fixed arrays of `MAX_CAPTURED_VARS`
(32, `include/tcc.h:1131`), and every place that fills them skips an entry
once the array is full, with no diagnostic. A nested function that uses a
33rd parent local, parameter, `static`, `typedef` or enumeration constant
then fails to compile with `'<name>' undeclared`. Exactly 32 works.

## Reproducer

`repro/nested-fn-capture-limit-silently-drops.c` (40 parent locals summed
by a nested function):

| compiler | result |
|---|---|
| gcc | `281` |
| tcc, `-O0`/`-O1`/`-O2`/`-Os` (base `f6d53136`, merge `ad155d19`) | `error: 'v40' undeclared` (the 33rd capture) |

The same with 40 block-scope `static int`s, 40 `typedef`s or an `enum` of
40 constants in the parent, each used by the nested function: `'s0'`, `'t0'`,
`'e0' undeclared` respectively (checked with `-c` on both crosses).

## Root cause

`frontend/gen/nested/nested.c`: the capture recorders guard with
`nb_captured < MAX_CAPTURED_VARS` (`prescan_captured_vars` ~856, the
grandparent path ~938, ~1070) and drop the rest; the parent-environment
copies do the same for `nb_parent_enums` / `nb_parent_typedefs` /
`nb_parent_statics` / `nb_parent_struct_tags` (~524-550, and
`frontend/gen/decl/decl.c` ~1239-1264). `NestedEnv` also sizes
`idents`/`static_prev`/`tag_prev` by the constant, and
`frontend/gen/inline/analysis.c` ~1270 (`genuine[MAX_CAPTURED_VARS]`)
assumes the bound.

## Regression lock

None.

## Likely fix

Turn the `captured_*`, `parent_*` arrays of `NestedFunc` (and `NestedEnv`'s
arrays, `shadowed_toks` in nested.c ~754, `genuine` in analysis.c) into
growable vectors freed in `nested_func_release`. At minimum, a
`tcc_error("too many captured variables")` instead of the silent drop.

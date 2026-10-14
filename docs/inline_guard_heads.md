# Guard heads: expanding a void body's test at its call sites

Knob: `TCC_DISABLE_PASS=inline:guard_head`.  -O2 only; never at -Os.

## What it does

A static void function that only tests before it does anything,

```c
static void deinit(struct Hdr *h)
{
  struct slice s;            /* declarations of locals */
  s = h->name_heap;          /* assignments to those locals */
  if (s.ptr != NULL) {       /* one if, no else */
    free(h->allocator, s);   /* the work */
    h->name_heap = EMPTY;
  }
  return;                    /* a tail that does nothing */
}
```

behaves exactly as

```c
{ struct slice s; s = h->name_heap; if (s.ptr != NULL) deinit(h); }
```

because the call repeats the test on the same memory: on the path that skips the
work nothing happened, and on the other the callee does all of it.  That body is
the function's *guard head*.  A call site that keeps the function out of line
expands the guard head instead of a plain call, so the call is made only when the
test holds.  The out-of-line body is unchanged.

Zig's `FileHeader.deinit` is this shape.  Its 11,316 kernel calls all found
`name_heap.ptr` null, and each cost a call, a push, two loads, a `cbz` and a pop.
The whole body was too large for the one-call wrapper class (cost 15 against
`WRAPPER_INLINE_OPS` 12).  Raising that limit for guarded wrappers was tried on
2026-10-13 and rejected: it inlined dozens of other bodies, and kernel `.text`
grew by 8.7 KB.  A guard head adds only the test and a branch at each site.

## Where

- `inline_body_guard_head` (`source/frontend/gen/inline/analysis.c`) checks the
  saved body's tokens (after macro expansion) and builds the head.  It builds one
  only for the shape above:
  - Every prefix statement is a declaration of plain locals, an assignment
    `local = expr;` to one of those locals, or `;`.  A statement is a
    declaration only when it starts with a type keyword or with a name the head
    does not bind itself whose binding is a file-scope typedef.  The scan runs
    after the body's scope is gone, so a parameter or local named like a
    typedef (`int T = 1; T * x;`) would otherwise turn a multiplication into a
    declaration, and the next `x = x + T;`, a write to the global `x`, into a
    replayed local assignment.  The declarations may have
    effect-free initializers.  Nothing else: no `static`, `volatile`, arrays,
    function declarators or brace initializers.
  - Expressions contain names, integer constants, casts and operators.  They have
    no assignment, increment, decrement or comma.  They have no call: a `(` after
    a name, `)` or `]` counts as one.
  - No `__func__` or `__FUNCTION__`: replayed in the caller, they would name the
    caller.
  - The test is `if (expr) { ... }` with no `else`.
  - The tail holds only `;`, `return;`, labels, and `goto`s to a label later in
    the tail.
  - Parameters are named integers or pointers.  The function is not variadic, and
    its name does not appear in its body or as a parameter name.

  Anything it does not recognise keeps the plain call.
- `gen_function` (`source/backend/generators/function.c`) registers the head
  (`guard_head_register`, `inline/emit.c`) under these conditions:
  - The function is static and not `noinline`.
  - Its definition is final (not weak).
  - It is not auto-inlined.
  - Its IR has no volatile access, which could be the test's own.
- At a call site (`source/frontend/gen/builtin/call.c`), the head is replayed by
  the ordinary token-replay expansion, with the same shadowing checks.  Inside
  the replay, the call to the function itself stays a plain call, because its
  expansion is in progress (`inline_expansion_in_progress`).
- The scan also records the head's *free names*: every name it reads that is not
  a parameter, one of its locals declared before that point, a member or a tag,
  plus the function's own name for the call.  These are resolved in the caller,
  so a site where any of them is bound locally (`guard_head_captures_local`)
  keeps the plain call.  The binding can be a variable, a parameter, a typedef
  or an enum constant, and it can belong to the caller or to an outer expansion.
  The general check (`inline_body_has_unsafe_shadowed_ident`) does not catch the
  outer-expansion case: inside an expansion it checks only enum constants.  So
  `if (g)` replayed under an outer `int g = 0` skipped the call, and `T v = *p;`
  under an outer `int T` did not compile.

- The parameters are bound as in any expansion.  For a K&R definition each
  promoted argument is converted to its parameter's type there, as the callee
  does on entry (`inline_knr_arg_needs_conversion`, `call.c`).
- A recompiled body that no longer has a head clears `SymAttr.guard_head`.
  Heads are found through a table keyed by symbol token
  (`TCCState.guard_head_by_tok`), so a call site's lookup does not scan them.

Tests: `tests/ir_tests/bug_guard_head_partial_inline.c`,
`bug_guard_head_caller_bindings.c`, `bug_guard_head_typedef_shadow.c` and
`bug_inline_knr_narrow_param.c` (runtime, all levels),
`bug_guard_head_func_name.c` and `test_codegen_asm.py::test_guard_head_*`.

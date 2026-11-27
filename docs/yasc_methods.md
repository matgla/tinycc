# yasc methods

Member functions for the yasc dialect, in the spirit of Zig: written in the
struct body, with an explicit `self`, compiled to plain C functions.
Implementation: `source/frontend/gen/decl/method.c`. Tutorial: chapter 7 of
`examples/yasc/README.md`. Tests: `tests/ir_tests/yasc_methods.c`,
`yasc_methods_mod*.c` (QEMU, every -O level), `test_yasc_methods.py`
(diagnostics, ownership checks, module interface and C header).

## Decisions

| Question | Decision |
|---|---|
| Where methods are written | In the struct body, with their bodies (Zig). With modules there is no header to keep declarations in. A prototype in the body, defined outside under the full name, also works. |
| How a method gets the object | An explicit first parameter (Zig), so the yasc qualifiers say what the method does with it: `view`, `owned`, `drop`, `?`, `const`. No hidden `this`. |
| Symbol | `TAG__NAME`: a plain C function. C calls and links it by that name; no mangling (as for modules). |
| Call syntax | `obj.m(...)` passes `&obj`, `p->m(...)` passes `p`; a by-value self gets a copy. `TAG.m(...)` (tag or typedef name) calls any method by name, and is the only way to call a method without a self. |

## Semantics

```c
struct vec
{
  int x, y;
  int len2(const struct vec *self) { return self->x * self->x + self->y * self->y; }
  void scale(struct vec *self, int k) { self->x *= k; self->y *= k; }
  struct vec add(struct vec self, struct vec o) { ... }   /* by value */
  struct vec make(int x, int y) { return (struct vec){x, y}; }   /* no self */
};

struct vec v = vec.make(3, 4);
v.len2();          /* vec__len2(&v) */
p->scale(2);       /* vec__scale(p, 2) */
v.add(w);          /* vec__add(v, w) */
vec.len2(&v);      /* the same function, by name */
```

- A member whose declarator is a function (`NAME(` at nesting level 0, NAME
  not a typedef or tag) is a method. Function pointers, bit-fields and every
  other member are fields as before.
- A method takes no room in the struct and does not change its layout.
- The self parameter is the first parameter when it is a pointer to the
  struct (any qualifiers) or the struct by value. Without one, `obj.m()` is
  an error that points at `TAG.m()`.
- `static`, `inline`, attributes: as for any function. `static` is C's
  `static` (internal linkage), not C++'s.
- Bodies are compiled after the struct is complete, and every method is
  declared before any body: a body uses every field and calls every method,
  in any order.
- Only in yasc files. Regular C (`-ccompat`, headers) keeps rejecting a
  function member ("invalid type").

Restrictions, each with its own error: the struct needs a tag; it must be
defined at file scope, not inside a function or another struct; a field and a
method cannot share a name; a method is declared once; one method per
declaration.

## How it works

1. **Struct body** (`yasc_method_member`, called by `struct_decl` for every
   member of a yasc struct): the member's tokens are saved up to its `;` or,
   for a body, its closing `}` (`ms_feed`: the same `{`-after-`)` rule as
   `yasc_export_capture`). A field is replayed at once and parsed as before.
   A method is kept, its name token rewritten to `TAG__NAME`.
2. **Replay** (`yasc_methods_flush`, at the start of the next file-scope
   declaration, from `decl()`): every kept method's prototype, then every
   body, as ordinary file-scope declarations. Each one starts with a marker,
   `__yasc_method TAG NAME` (`__yasc_method_decl` for the prototype of a
   method that has a body), and the replay is wrapped in yasc-on / previous
   dialect markers and ends with the line of the token already read.
3. **Registration** (`yasc_method_marker`, from `yasc_file_scope_decl`):
   TAG.NAME -> TAG__NAME in a small hash table; the tag gets
   `YASC_HAS_METHODS` (Sym.ymod).
4. **Calls**: the `.` / `->` postfix code (`unary.c`) asks
   `yasc_method_postfix` when the struct has methods; the object goes into
   `funcall_self_arg` and `unary_funcall` takes it as argument 0 before the
   parenthesized ones (`call.c`). `TAG.NAME` is resolved in `unary_primary`
   (and `parse_btype` leaves `typedef_name.` to the expression parser).

Because a method is an ordinary declaration once replayed, linkage, inline
expansion, module export, debug info and the lifetime checks need nothing
new. One exception: the passes that read saved bodies *by name* before they
are compiled -- unused-static pruning and called-once / few-callers inlining
(`inline/emit.c`) -- see a method call as `.NAME(` or `->NAME(`. They count it
as a mention of every method of that name (`yasc_method_name_of`): such a
method is never pruned while a body mentions its name, and is not inlined as
called-once.

## Ownership checks

The object is argument 0 of a normal call, so `-Wlifetime` treats it like any
argument: `p->destroy()` with `drop struct buf *owned self` moves `p`
(a second call is "freed twice", a later use "used after it was freed"), a
`?` pointer is reported when passed to a non-maybe self, and
`obj.destroy()` on a stack object is "a pointer that owns nothing is stored in
an owned pointer". For `p->m()` the pointer itself is passed, not `&*p`, so
its qualifiers are kept.

## Modules

`export struct` exports its methods. The struct's own piece of the interface
leaves them out (`yasc_methods_strip`); each method is a piece of its own,
prefixed with its marker, so importers register it: a prototype, or the whole
body as `static inline` for an `inline` method (the usual export rules). The
prototype replayed ahead of the bodies is no piece. A private struct's methods
stay private (internal linkage, as any private function).

`-fmodule-header=` writes the struct without its methods and each method as a
C declaration under its symbol name. An exported `static inline` body that
uses method-call syntax cannot be written as C: that is an error naming the
method (call the function `TAG__NAME` in such a body).

## Later

- Methods in structs defined inside another struct, and in anonymous structs
  through their typedef name.
- Nicer `-Wlifetime` wording for a method's self ("'p' may be null where
  'size' is called" rather than "argument 1 of 'buf__size'").
- Pruning that knows the receiver's type, instead of every method of the name.
- Not planned: overloading, virtual dispatch (use a function pointer field),
  implicit constructors and destructors (`defer p->destroy();` covers scope
  cleanup), a hidden `this`.

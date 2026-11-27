# @src() and default arguments

Where the code is, without `__FILE__`/`__LINE__` macros, and parameters a
caller may leave out, as in C++ (`std::source_location::current()` as a
default argument). Implementation: `source/frontend/gen/builtin/misc.c`
(`@src()`), `source/frontend/gen/decl/default_arg.c` (defaults), the call in
`source/frontend/gen/builtin/call.c`. Header: `include/yasc_src.h`. Tests:
`tests/ir_tests/yasc_src.c` (QEMU, every -O level), `yasc_src_gcc*.c` (a gcc
object reads tcc's values and makes one for tcc), `test_yasc_src.py`
(diagnostics, .rodata, the call's code, C headers, modules).

```c
#pragma yasc
#include <yasc_src.h>

void log_warn(const char *msg, yasc_src at = @src())
{
  printf("%s:%u: %s: %s\n", at.file, at.line, at.func, msg);
}

int parse(const char *s, int base = 10);

log_warn("disk full");       /* at: this line, this function */
parse("42");                 /* parse("42", 10) */
printf("%s:%u\n", @src().file, @src().line);
```

## Decisions

| Question | Decision |
|---|---|
| Name | `@src()`, one builtin returning a struct (Zig's `@src()`, C++'s `source_location`), not `@file()` / `@line()`: a function needs all of it at once. |
| Members | `file` (as `__FILE__`), `func` (as `__func__`, `""` at file scope), `module` (the yasc module, `""` outside one), `line` (as `__LINE__`). No column: tccpp tracks one only for `-fstack-usage`. |
| Caller's location | A default argument, as in C++: `@src()` written as a default is made at each call that leaves the parameter out, so it is the caller's location. The function is an ordinary C function with every parameter. |
| Representation | A value of two words, `{__fn, line}`: `__fn` points at the function's **header** `{file, func, module}`, one per function in `.rodata`; `at.file` / `.func` / `.module` read through it. Passed in two registers (AAPCS: a composite of two words), copied as any struct. |
| Cost of a call | The line as an immediate and the header's address, a literal-pool word every call in the function shares: at -O1 and up `ldr r1, =header; movs r2, #line` (`ra:struct_arg_split` takes the value out of its frame slot). Nothing per call in `.rodata`, nothing in RAM. |
| Header storage | `.rodata`, no RAM: its names are **self-relative words** (`VT_SELF_REL`), each the distance from the word itself to its string, filled in by the linker (`R_ARM_REL32`), never at load time. An ordinary pointer would put the header into each process's `.data` on YasOS (`-share-rodata` keeps const objects holding addresses out of the shared `.rodata`); a `__rodata_relative` one would mean nothing in another module (`libc.so` reading an application's header). |
| Header copies | None: no object, member, parameter, result or compound literal has the header's type, and `&at.file` is an error. Only the value's pointer to it travels. |
| Written out | `@src().line` / `.file` / `.func` / `.module` make no value at all: the constant and the string's address, the code `__LINE__` / `__FILE__` / `__func__` give. They work anywhere, at file scope too. |
| Static values | None: `static yasc_src g = @src();` would store the header's address in data, a load-time relocation (8 bytes of RAM). `@src()` itself is made in a function; a variable assigned from one at run time is fine. |
| Lifetimes | The names and `__fn` are `* view borrows(static)`: a yasc_src may be kept in a global, and `at.file` returned from a function declared `borrows(static)`. |
| Other defaults | Constant expressions only (a number, `nullptr`, a string literal, the address of a static object or a function), evaluated and converted where they are written, so the names in them mean what they meant there; no call site can capture them. |
| Where | yasc only (`#pragma yasc`, `-std=yasc`); calls fill them in in C code too. Only a function declaration's own parameters, only at file scope (not a typedef, a function pointer or a parameter of function type). |
| Redeclaring | A later declaration may add defaults or say one again identically; changing one is an error. Once a parameter has one, every later parameter has one too. |
| Calls through a pointer | Pass every argument, as in C++: the defaults belong to the function's name. |

## Semantics

- `@src()` is a value of type `struct __yasc_src` in a temporary of the
  function: at -O1 and up a frame slot of its own (as a compound literal's,
  which `ra:struct_arg_split` removes when the value is an argument), at -O0
  one shared temporary. `sizeof(@src())` and `typeof` make nothing.
- `struct __yasc_src` and `struct __yasc_src_fn` exist in every unit, made
  when first named (by `@src()` or by name); `<yasc_src.h>` gives the typedef
  `yasc_src`. They cannot be redefined. They are made under any open
  parameter list (`sym_sink_to_bottom`), which would otherwise pop them.
- A default argument is `= EXPR` after a parameter of a prototype or a
  definition: `int f(int a, int b = 2)`. A call that ends before a parameter
  with a default gets the default for it and for every one after it; a call
  that leaves out a parameter without one is "too few arguments" as before.
  `@src()` is the only non-constant default: not `&@src()` (a temporary's
  address) nor `@src().line` (that would be the declaration's line).
- The defaults are kept by the function's name, per unit. A prototype in a
  header and the definition without them is the usual shape; yasc methods
  (`int bump(struct counter *self, int by = 1)`) and exported module functions
  take them too -- the `.ymi` carries them to importers.
- A pointer parameter with a default is a view as any other (`T *p = d`).

## C headers and other compilers

`tcc -yasc-c-header` and `-fmodule-header=` leave the defaults out: the C
declaration lists every parameter, and C callers pass them all. `<yasc_src.h>`
serves both sides:

```c
log_warn("disk full", YASC_SRC);          /* tcc: @src(); gcc: made here */
printf("%s:%u\n", YASC_SRC_FILE(at), at.line);   /* reads any yasc_src */
```

Under gcc the structs have the same layout with the header's names as
integers (`__INTPTR_TYPE__`), `YASC_SRC_FILE()` / `_FUNC()` / `_MODULE()` add
the word's address back, and `YASC_SRC` fills a static header at the call
(gcc cannot make the distance between two objects a constant).

## Implementation notes

- The header is written by hand once per (file, function, module) in the unit
  (`src_header_sym`): the three names interned in the string-literal pool
  (`str_lit_pool_intern`), one ELF symbol per distinct name, three
  `R_ARM_REL32` relocations with addend 0, and one symbol on the header for
  the unit, so the function's calls share its literal-pool word. The
  relocations matter although both ends are in the same `.rodata`:
  `gc_unreferenced_statics` drops unreferenced statics and moves what follows
  them, fixing symbols and relocations but not a distance written by hand.
- `yasc_src_push` stores `__fn` and `line` into the temporary and pushes it
  as an lvalue; `yasc_src_header_member` turns `at.file` into `at.__fn->file`
  in `unary_member`; `self_rel_rvalue` reads the word (unary.c's postfix and
  end of unary, as for `__rodata_relative`); the const-object fold in
  `indir.c` skips `VT_SELF_REL` words, whose bytes are only an addend.
- Defaults: `post_type` calls `yasc_default_param` for `=`, which parses a
  conditional expression as a static initializer would, with `@src()`
  standing for a marker Sym; `decl()` binds the list to the declarator's name
  right after `type_decl` (`own_functype`), so parked or spilled prototypes
  (symtab.c) lose nothing. The call loop keeps going past `)` while the next
  parameter has a default (`DFLT_NEXT`), pushing the stored value -- or a new
  `@src()` for this call in place of the marker.
- A parameter's own pointer is a view when `=` follows its name, as when `,`
  or `)` does (`yasc_param_name_ends`).

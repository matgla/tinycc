# yasc modules

Status: design agreed 2026-11-20. Steps 1 (keywords, linkage, export checks), 2
(interface writer), 3 (`import`), 4 (`-fmodule-deps`), 5 (`-fmodule-header=`) and 6
(unchecked pointers from regular C) done; modules in `source/frontend/gen/decl/module.c`;
tests `tests/ir_tests/test_yasc_modules.py` (diagnostics, interface contents) and
`test_qemu.py::test_yasc_modules_run` (two modules and an importer, run on QEMU at
every -O level).
Branch `feat/yasc-modules`.

Tutorial: `examples/yasc/README.md` (the whole dialect, a runnable program per
chapter; `test_qemu.py::test_yasc_tutorial`).

Usage: `examples/yasc_modules` (two modules, a yasc program, a plain C program, a
Makefile; built and run by `test_qemu.py::test_yasc_modules_example`), the man page
`docs/man/yasc-modules.txt` (`man yasc-modules` on YasOS: yasos's
`scripts/man/build_man.py` takes `docs/man/*.txt`), and the "yasc modules" section
and options in `tcc-doc.texi` (`man tcc`).

yasc is safe by default, so it needs a clean boundary with legacy C. `#include`
stays textual C. Modules are a **language** feature (keywords, parsed after
preprocessing), so macros never cross a module boundary in either direction.

## Decisions

| Question | Decision |
|---|---|
| How imported names are seen | **Flat, plain C names.** `import net.socket;` puts the module's exports in the importer's file scope. Object symbols keep their C names (no mangling), so C code links against a yasc module directly. Two imports that export the same name are an error at the second import. |
| What a module exports | **Explicit `export`.** Private by default: a declaration without `export` gets internal linkage (as if `static`), so private helpers of two modules never clash at link time. Types, typedefs and enums need `export` too. |
| Where the interface comes from | **A binary interface file** (`.ymi`) written when the module is compiled and read by importers. Modules must be built before their importers. |

## Syntax

```c
module net.socket;                 /* first declaration of the file */

#include <stdint.h>                /* legacy C: unchecked pointers, see below */
import net.addr;                   /* another module */

export typedef struct sock sock_t; /* opaque to importers */
export sock_t * owned maybe sock_open(addr_t addr);
export void sock_close(sock_t * owned drop s);

struct sock { int fd; };            /* private: importers never see the body */
int parse_port(const char * view s) { ... }   /* private: internal linkage */
```

```c
import net.socket;

int main(void)
{
    sock_t * owned maybe s = sock_open(addr_any());
    ...
}
```

- `module`, `import` and `export` are keywords only in yasc (`-std=yasc`, `#pragma yasc`,
  or safe mode without `-ccompat`), and even there only where a file-scope declaration
  starts, in the main file, with no typedef of that name in scope. `int module;` or a
  parameter named `import` still compile, and headers are never affected.
- `module NAME;` makes the file a **module unit**. Only module units have the
  private-by-default rule. A yasc file without `module` (a program's `main.yc`) keeps
  C linkage rules and can `import` but not `export`.
- `import NAME;` is allowed at file scope only. Importing a module twice in one unit
  is a no-op (identity = module name). A module's imports are loaded before it, so
  importing `shapes` (which imports `geom`) gives you `geom` too. Modules that import
  each other are an error, naming the pair.
- `export` goes in front of a file-scope declaration: functions, globals, `typedef`,
  `struct`/`union`/`enum` definitions and tags, `enum` constants (via their enum).
  `export` with `static` is an error. `export inline` (or `export static inline`) puts
  the body in the interface, so importers can inline it.
- One file is one module. Dotted names map to paths: `net.socket` is `net/socket.ymi`.

## Checks at module compile time

- An exported declaration may only name exported types (or types from legacy
  headers / other modules):
  - a private struct/union by value, or a private enum: error (checked when the
    export is parsed, and again at the end of the unit, for a body defined later);
  - a private typedef name, or an enumerator of a private enum (e.g. an array
    size): error, checked on the interface tokens.
  Through a pointer a struct may stay opaque. Opaque handles are written as
  `export typedef struct sock sock_t;` with the struct body private.
- An `export inline` body may only use exported names (it is compiled in the
  importer): a private function or object, or `struct s` whose body is private, is
  an error. Names are looked up at file scope, so a local that shadows a private
  global in an inline body is reported too (rename it).
- `export static` is an error, except `export static inline` (no symbol; the body
  goes to importers). A plain `export inline` is also defined in the module (a global
  symbol, for C callers); yasc importers get its body, both are written to the
  interface as `static inline`.
- `extern` in a module names something outside it: not private, so an exported
  inline function may call it.
- `export` under `#pragma pack` is an error for now (the importer would need the
  same pack state); `__attribute__((packed))`, or a `#pragma pack` inside the
  struct body, travels with the tokens.
- Not yet: a private function that is declared but never defined.

## The interface file (`.ymi`)

The interface is a **token stream, not a serialised symbol table**: the exported
declarations after preprocessing, function bodies removed (except `export inline`),
`export` dropped. The importer replays it through the ordinary parser, the same way
`defer` replays its saved body. One parser, so an interface cannot mean something
different from the source, and there is no Sym/CType graph to serialise.

What it buys over parsing the source: no preprocessing, no `#include` searches (the
failed header opens that cost the board a lot of compile time), no function bodies,
and nothing that depends on the importer's macros.

Layout (little-endian):

```
magic "YMI1"
u32 format version (1)
str compiler id: "tcc VERSION ptrN longN ldN ucN fabiN" -- version and the ABI
    facts that change layout; a mismatch makes the reader say "rebuild it"
str module name
str module source file (for diagnostics: line markers refer to it)
u32 count, str each: includes -- the legacy headers the main file included:
    `<h>` as written (the importer searches its own paths), "h" as the absolute
    path found
u32 count, str each: imports (none until step 3)
u32 count, str each: strings -- the spellings of identifiers and keywords
u32 count, then that many u32 words: tokens
```

`str` is a u32 length and the bytes, no terminator. Token words:
- a punctuator or other token without a value: its code (< 256);
- an identifier or keyword: `256 + index into strings` (a unit numbers its
  identifiers in the order it meets them, so codes are not portable; keyword codes
  change with tcctok.h);
- a value token (0xc0..0xd4): its code, then its value: one word for int, char,
  float, line number and pack-replay tokens; two (low first) for long long and
  double -- long double is written as a double, which the target's is; strings
  and pp-numbers: the size with the NUL, then the bytes padded to a word.

Each export's piece of the interface is its tokens as written, after macro
expansion: an object gets `extern` and loses its initializer, a function
definition becomes its prototype, an inline definition keeps its body as
`static inline`. `tests/ir_tests/test_yasc_modules.py` has a decoder (`_load_ymi`).

- The writer replaces the file only when the bytes change, so editing a function body
  does not touch the `.ymi` and `make` does not rebuild every importer.
- `-MD` lists every `.ymi` read, so importers rebuild when an interface changes.

### Where it goes and how it is found

- Writing: `-fmodule-dir=DIR` (default: the directory of the `-o` object). Module
  `net.socket` writes `DIR/net/socket.ymi`.
- Reading: each `-fmodule-dir=` in order, then the object's directory, then the `-I`
  paths. Not found: "module 'a.b' not found: compile it first". Another compiler or
  ABI (the id differs), another format version, another module's file under that
  name, or a damaged file: an error saying which, with "rebuild module 'a.b'".
- Build order is the build system's job. To help, `tcc -fmodule-deps FILE...`
  prints what each file is and imports, without compiling or importing anything (no
  interface needs to exist yet): a `module NAME` line, then an `import NAME` line
  per import, in order. With several files, a `file PATH` line comes before each
  file's lines; `-o FILE` writes them to FILE. It runs the preprocessor, so `#if` and
  macros count, and looks only at file-scope declarations of the main file in yasc
  (a struct member or local named `import` is not one; plain C prints nothing).

### With make

Each object depends on the interfaces it imports; an interface comes with its
module's object. `-MD` adds every `.ymi` an object read (imports of imports too),
so later builds rebuild importers when an interface changes -- and only then: the
writer keeps an unchanged `.ymi` as it was.

```make
TCC = tcc
SRCS = main.c shapes.c geom.c
all: $(SRCS:.c=.o)
%.o: %.c
	$(TCC) -c $< -o $@ -MD -MF $*.d
# module a.b -> "a/b.ymi: x.o ;"   import a.b -> "x.o: a/b.ymi"
%.mdep: %.c
	$(TCC) -fmodule-deps $< | awk -v o=$*.o '{ i = $$2; gsub(/\./, "/", i); i = i ".ymi" } \
	  $$1 == "module" { print i ": " o " ;" } $$1 == "import" { print o ": " i }' > $@
include $(SRCS:.c=.mdep)
-include $(SRCS:.c=.d)
```

With `make -j` this builds geom, shapes, main in that order; a function body edited
in geom rebuilds geom.o only, a changed signature rebuilds its importers
(`test_yasc_modules.py::test_module_deps_orders_a_parallel_make`).

## Legacy C

- `#include` is unchanged: textual, C rules.
- **Which files are yasc** (`BufferedFile.yasc`): the main file of a yasc unit
  (`-std=yasc`, or safe mode without `-ccompat`), any file that says
  `#pragma yasc` (from the pragma on), a header found through `-iyasc DIR` (or
  `YASC_INCLUDE_PATH`; searched as `-I`, and a `"..."` header next to one is
  yasc too -- as if it began with the pragma), and an imported interface. An
  `#include`d file starts in regular C. `TCCState.yasc` follows the file and the macro
  being read (combined's per-file dialect: a macro defined in regular C,
  `MACRO_YASC` clear, expands as regular C between "yasc off/on" markers).
- A pointer written in regular C is **unchecked** when seen from yasc (like
  Kotlin platform types): its pointer node carries `YASC_UNCHECKED`, and it
  converts to and from `owned` and `view` pointers without a diagnostic; the
  flow checks (-Wlifetime) take it as neither maybe nor null -- checks start at
  the yasc variable it lands in. So `char * owned p = c_name();` and handing an
  owned result to a C function (`c_take(make())`) are silent, while the same
  with a pointer declared in yasc still warns. An owned variable passed to a
  C function's parameter says what happens to it (2026-11-23):
  `c_use(borrow(p))` lends it (p is still owned), `c_keep(move(p))` hands it
  over (p is moved there); a plain `c_use(p)` is -Wimplicit-move's "'p' is
  owned and goes to a regular-C parameter: write borrow(p) or move(p)" (a
  fix-it adds borrow). A call's owned result needs neither. Replaces the
  earlier "reads as `view maybe`" rule, which would have flagged every C call.
- The arguments written inside a regular-C macro are regular C too, and so is
  a yasc macro used inside one (its expansion is inside the markers): with
  tcc's function-like `va_arg`, `va_arg(ap, char *)`. A regular-C macro inside
  a yasc macro (its body or an argument) is regular C. A `*` is in the dialect
  it was written in: `PTR(char) p` with a regular-C `PTR` is an unchecked
  pointer, though `p` is yasc again; `PTR(char) view p` qualifies it.
- `tcc -E` keeps the dialects for the compile of its output: the line markers'
  flags 1/2 say where a header starts (regular C) and returns (the dialect of
  the file it returns to), and an `-iyasc` header gets its `#pragma yasc`. A
  regular-C macro's expansion in yasc code is written between
  `_Pragma("tcc c_begin")` and `_Pragma("tcc c_end")`, inline (the lines
  stay); compiling the output, `#pragma tcc c_begin` / `c_end` (also through
  `_Pragma`) turn the dialect off and on as the markers do, kept in a saved
  body as those markers, so the output compiles to the source's object with
  its diagnostics (test_yasc_c_macros.py).
- The pointers a regular-C macro writes inside yasc code (`NULL`, `offsetof`, a
  header's `((char *)p)`) are unchecked too. `pp_in_c()` (tccpp.c) answers "is
  the token just read regular C, in a unit with yasc in it?"
  (`tcc_yasc_unit`).
- Only units that are yasc mark anything: a regular C unit (`-ccompat`, the
  self-host build) gets the same pointer nodes and code as before. In a unit
  that turns yasc with a `#pragma yasc` after its `#include`s, the headers read
  before the pragma are not marked: put the pragma first, or use `-std=yasc`.
- Bodies saved and compiled later (static inline functions of a header,
  deferred bodies) are replayed from the main file, so their locals count as
  yasc; it does not matter today, since C code has no owned pointers to check.
- **GCC attributes refine it** (`attr_apply_pointer_facts`, attribute.c): a C
  header that says what it means is believed over "unchecked".
  - `malloc` (or `malloc(dealloc, n)`): the result is `owned maybe` -- `owned`
    with `returns_nonnull`. Storing it in a view or discarding it warns, and so
    does a pointer that is not `maybe` (the flow checks); `char * owned maybe p =
    malloc(n);` is the yasc spelling.
  - `malloc(dealloc, n)`: the deallocator's n-th parameter is
    `owned maybe drop`, so freeing a pointer yasc code does not own warns.
  - `nonnull(i, ...)` / `nonnull`: those parameters are marked
    (`SymAttr.nonnull`) and a null constant for one is a `-Wnonnull` warning, in
    every unit (in `-Wall`, so in safe mode). As with GCC, every declaration's
    `nonnull` counts: a redeclaration or the definition adds its marks to the
    earlier ones (`patch_type`, attr_merge.c). "Not `maybe`" means more once the
    flow checks exist.
  - yasos libc (branch feat/yasc-attributes, `sys/_attrs.h`) puts them on its
    allocators (`malloc`..`strdup` with `free`, `fopen` with `fclose`, `opendir`
    with `closedir`, `popen` with `pclose`) and `nonnull` on the functions that
    read a whole string. `__has_attribute` reports the three.
  - `yasc_view` / `yasc_maybe` on a struct member (`attr_apply_member_facts`,
    tcc only): its pointer is `view`, with `yasc_maybe` `view maybe`, so yasc
    code dereferences it without `unsafe` and the flow checks see what is stored
    in it.  A struct has one definition, so a yasc copy of it in a side header
    (as `sys/yasc/*.h` redeclares functions) is not possible; the attribute
    keeps it in the C header.  The outer pointer only, after the member's
    declarator (or in the specifiers, for every declarator); on a member that is
    not a pointer, or `yasc_maybe` alone, a warning.  yasos libc spells them
    `__yl_view` / `__yl_view_maybe` (empty where `__has_attribute` says no), on
    the syscall contexts (`sys/syscall.h`): `view` where libc points the member
    at its own object, `view maybe` where it passes on its caller's pointer.
    test_yasc_member_view.py.
- Not yet: `access(write_only, n)` as `out`, `warn_unused_result`, and side
  files of annotations for C libraries we cannot edit.
- **Headers a module includes**: their declarations are needed by the exported
  signatures (`size_t`, `FILE *`), so the `.ymi` records the legacy headers the
  module's file scope included, and the importer includes them before replaying
  the tokens (include-once applies). Their macros therefore do reach the
  importer: legacy headers are legacy. The module's own `#define`s never do.
- **C code calling a module**: compiling the module with `-fmodule-header=DIR` also
  writes `DIR/a/b.h`, a C99 header (gcc `-std=c99 -Wall -Wextra -pedantic` clean):
  - an include guard `YASC_MODULE_A_B_H`, the module's `#include`s, and
    `#include <c/d.h>` for each module it imports (so C callers use `-I DIR`, and
    generate the imported modules' headers into the same DIR);
  - the exported declarations, with the yasc qualifiers (`owned view maybe out
    drop`) left out -- C has no use for them, and defining them as empty macros
    would break C code that uses `out` or `view` as names;
  - a plain `export inline` as its prototype (the module defines it), an
    `export static inline` with its body; a body that uses `defer` cannot be C: an
    error, only with `-fmodule-header=`;
  - numbers keep their type (`4000000000ul`, `0xff...ull` as decimal with its
    suffix), floating constants are written back with enough digits.
  Written only when its bytes change, like the interface. yasc code should import
  the module rather than include its header: the header has no qualifiers.

## Not in the first version

- Re-export (`export import`). Imports are transitively visible for now: what a
  module's interface imports is also visible to its importer. Hiding them needs a
  scope per import.
- Several files per module, module partitions.
- Exporting macros. Use `enum`, `static const`/`constexpr`, and `export inline`.
- Qualified access (`socket.open`): rejected in favour of flat C names.

## Implementation steps

1. **Done.** Keywords `module`, `import`, `export` in yasc; `module NAME;` sets the unit's
   module name; private-by-default linkage in module units; `export` checks (private type
   in an exported signature, `export static`, private-then-export). Until the dialect is
   per file, "the module" means the main file: declarations from an `#include` keep C
   linkage. Sym bits `YASC_MODULE_OWN` / `YASC_EXPORT` (tcc.h) mark what the module
   declared and exported.
2. **Done.** Interface writer. `export` saves the declaration's tokens (up to its
   ';', or to a function body's '{', which stays in the file) and replays them into
   decl(); the saved tokens become the export's piece. The main file's `#include`s
   are recorded (`pp_note_main_include`, tccpp.c). At the end of the unit every
   export is checked again and the `.ymi` written (tmp + rename, only when the
   bytes change) to the first `-fmodule-dir=` or the object's directory.
3. **Done.** Reader. `import NAME;` finds the `.ymi`, checks its header, imports its
   imports, interns its strings, then pushes a small buffer of the recorded
   `#include` lines ending in `__yasc_import_end`, pushed like an included file and
   parsed by a nested decl() that stops at that marker, and replays the tokens
   into another nested decl() up to TOK_EOF. Both sit above the importer's file on
   the include stack, so nothing they declare counts as the importer's own (a
   module importing another does not make its declarations private). Tokens
   replayed from a string are never macro-expanded, so the importer's macros
   cannot change an interface. Diagnostics inside an interface name the module's
   source file and line, "In file included from" the import's line. `-MD` lists
   every `.ymi` read.
4. **Done.** `-fmodule-deps` (`yasc_module_scan`): runs as `-E` (no object, output to
   `-o` or stdout) but reads the file as a compile would (no `#pragma` passthrough)
   and prints the module/import lines. `-fmodule-dir=` came with step 2, `-MD` with
   step 3. Both options are in `tcc -hh`.
5. **Done.** `-fmodule-header=DIR`: the C header is built next to the interface, one
   export at a time (`yasc_cheader` in `yasc_export_finish`), and written by
   `yasc_header_write`. Run test: `test_qemu.py::test_yasc_modules_c_caller` (a plain
   C main through the generated headers).
6. **Done.** Unchecked pointers from regular C: the per-file dialect flag, the
   `YASC_UNCHECKED` pointer qualifier (set in `declarator.c` when `pp_in_c()`),
   `yasc_verify_assign` skipping a pointer unchecked on either side, and
   `MACRO_C` on macros defined in regular C (their expansion counted in
   `pp_c_macro` while it is read). Tests: `test_yasc_ownership.py` (the legacy C
   section), `test_yasc_modules.py::test_import_brings_unchecked_c`. Then GCC
   attributes refining it (`malloc`, `malloc(dealloc, n)`, `nonnull`,
   `returns_nonnull`) and `-Wnonnull`: `test_yasc_ownership.py` (attributes).

Tests: `tests/ir_tests` module pairs (module + importer, compiled in order, run on
QEMU) and error tests in `yasc_errors.c` style for each check above.

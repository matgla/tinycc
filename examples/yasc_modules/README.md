# yasc modules: an example

Chapter 8 of the yasc tutorial (`../yasc/README.md`), which covers the rest of
the dialect: ownership, `defer`, errors, C libraries and methods.

Two modules and two programs that use them:

| File | What it is |
|---|---|
| `vec.c` | module `lib.vec`: a vector type, an enum, an exported object, functions, a private helper, an `export inline` |
| `shapes.c` | module `shapes`: imports `lib.vec`, adds rectangles and an `export static inline` |
| `main.c` | a yasc program: `import shapes;` (which brings `lib.vec` too) |
| `c_main.c` | a plain C program using the same modules through generated C headers |
| `Makefile` | builds it all, in the right order, from what the sources say |

The full reference is `docs/yasc_modules.md` (in the tinycc tree) and
`man yasc-modules` on YasOS.

## Build and run

On YasOS (or with any tcc that can link for your target):

```sh
make
./demo
./c_demo
```

`main.expect` and `c_main.expect` hold what the two programs print. The tinycc
test suite builds and runs both on QEMU
(`tests/ir_tests/test_qemu.py::test_yasc_modules_example`).

With the cross compiler on a PC, `make modules TCC=/path/to/armv8m-tcc` builds the
modules alone: objects, interfaces and C headers.

## What happens

Compiling a module writes its interface next to the object:

```sh
tcc -c vec.c -o vec.o        # also writes lib/vec.ymi   (module lib.vec)
tcc -c shapes.c -o shapes.o  # reads lib/vec.ymi, writes shapes.ymi
tcc -c main.c -o main.o      # reads shapes.ymi, and lib/vec.ymi through it
tcc main.o shapes.o vec.o -o demo
```

So a module is compiled before the files that import it. `tcc -fmodule-deps FILE`
prints what a file is and imports, without compiling anything:

```sh
$ tcc -fmodule-deps shapes.c
module shapes
import lib.vec
```

The Makefile turns those lines into rules: `import lib.vec` makes `shapes.o`
depend on `lib/vec.ymi`, and `module lib.vec` says that `lib/vec.ymi` comes with
`vec.o`. `-MD` adds every interface a compile read, so editing a module rebuilds
its importers -- but only when the interface changes: an edited function body
leaves the `.ymi` untouched, and only that module's object is rebuilt.

### Exports

Everything at file scope in a module is private unless it says `export`:

- `vec_add`, `vec_ops`, `vec2_t`, `enum axis` are exported; `count` is not, so
  it gets internal linkage and no other module's `count` clashes with it.
- Symbols keep their C names: `vec_add` is `vec_add` in the object file.
- `export inline int vec_dot(...)` gives importers the body to inline, and the
  module also defines it for C callers.
- `export static inline int rect_width(...)` has no symbol at all: its body
  goes to importers and into the C header.
- An export cannot use a private type by value, but a struct can stay opaque
  behind a pointer: `export typedef struct handle handle_t;` with the body
  private.

### Macros do not cross an import

`import` is a keyword, not a preprocessor directive: an interface holds the
module's declarations after preprocessing, and the importer's macros never
change them. The module's own `#define`s never reach the importer; the headers
the module `#include`d are included for the importer too, macros and all.

### C callers

`-fmodule-header=include` (in the Makefile's module rule) also writes
`include/lib/vec.h` and `include/shapes.h`, plain C99 headers of the interfaces.
`c_main.c` includes them, compiled with `-ccompat` (regular C) and `-Iinclude`.
gcc takes them too.

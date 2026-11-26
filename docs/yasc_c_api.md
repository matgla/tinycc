# The C API beside the yasc one: `c_api(T)`

A yasc library often wants a stricter type than its C API can have. POSIX says
`char *dlerror(void)`, but the text belongs to libc and nobody may write it:
yasc wants `const char * borrows(static) maybe`. Before `c_api`, the yasc header
had to keep the C type, and the definition needed
`unsafe_cast<ptr(char *), const>(...)` to return a constant string.

`c_api(T)` lets one declaration state both types:

```c
const char * borrows(static) maybe c_api(char *) dlerror(void);
int execv(const char *path, const char * view const c_api(char * const) argv[]);
long strtol(const char *s, out const char * borrows(s) c_api(char *) * maybe endptr, int base);
struct passwd { const char * view c_api(char *) pw_name; /* ... */ };
```

## What it replaces

`c_api(T)` comes after a `*` and replaces **everything to its left** in that
declarator: the base type and the `*`s before it. Everything **to its right**
comes from the yasc declaration: more `*`s, the name, `[ ]`, the parameter list.

| yasc declaration | yasc type | C type |
|---|---|---|
| `const char * borrows(static) maybe c_api(char *) dlerror(void)` | result `const char *` | `char *dlerror(void)` |
| `const char * view const c_api(char * const) argv[]` (a parameter) | `const char * const argv[]` | `char * const argv[]` |
| `out const char * borrows(s) c_api(char *) * maybe endptr` | `const char **` | `char **endptr`: only the inner level is replaced |

In yasc, `c_api` after a `*` is always this word, as `borrows` is. The definition
does not need to repeat it.

## What T may differ in

T and the type it replaces must be the same except for `const` and `volatile`
at some pointer level, and the yasc words. The ABI stays the same, so no code is
generated for the conversion. The compiler refuses:

- a pointer for something that is not one: `void * c_api(int)`;
- another base type: `int * c_api(long *)`. `int` and `long` have the same size
  on ARM but not on the x86_64 target, so tcc compares the types, not their
  sizes. `char` vs `int`, `void *` vs `T *`, different array sizes and an
  array vs a pointer are refused too;
- a C type that tells C callers the target is const where the yasc declaration
  may write it: `inout char * c_api(const char *) p`, or a member
  `char * view c_api(const char *) name`. A function's result may say more
  `const` in C than in yasc, because C only reads it.

## What the compiler does with it

- **In yasc:** the body and every yasc caller use the yasc type. In
  [libc's dl.c](../../libc/dl.c), `dlerror` returns its `const` buffer without a
  cast.
- **A regular-C declaration in a yasc unit** (an unconverted header, a
  `#pragma tcc c_begin` section) is compared with the C type, in either order.
  The symbol keeps the yasc type.
- **Function pointers:** the function converts to a pointer of exactly its C
  type (`struct ops { char *(*err)(void); } o = { dlerror };`). Any other type
  is still an incompatible pointer.
- **Two yasc declarations** that give different `c_api(...)` are an error.
- **`tcc -yasc-c-header`** (docs/yasc_c_headers.md) writes T in place of the
  replaced part. On a prototype it also adds
  `__yl_c_api(n, the yasc type without its words)`, where n is 0 for the result
  and k for parameter k:

  ```c
  char *dlerror(void) __yl_c_api(0, const char *);
  int execv(const char *path, char * const argv[]) __yl_c_api(2, const char * const []);
  long strtol(const char *s, char **endptr, int base) __yl_c_api(2, const char * *);
  ```

  libc's `<sys/_attrs.h>` turns `__yl_c_api` into tcc's
  `__attribute__((yasc_c_api(n, T)))`. gcc, clang and an older tcc see nothing.
- **`-fmodule-header`** does not write C for it yet: it reports an error. Declare
  such a function in a yasc header instead.

## `-Wyasc-c-api`: C callers that write through it

In a regular-C unit, the attribute tells tcc what the yasc declaration says.
`-Wyasc-c-api` (on by default, also under `-ccompat`; `-Wno-yasc-c-api` turns it
off) reports:

```c
char *e = dlerror();
e[0] = 'x';            /* writing through the result of 'dlerror': its yasc declaration says it is read-only */
strcpy(dlerror(), "x"); /* the result of 'dlerror' goes to parameter 1 of 'strcpy', which may write through it */
char *end;
strtol(s, &end, 10);
*end = 0;              /* writing through what 'strtol' stored through its argument 2 */
```

**What counts as a source:** the result of a function whose yasc declaration
returns a pointer to const, and what such a function stores through an `out`
argument (`&end`).

**What it follows:** temps, variables, the frame's address-taken locals, and
pointer arithmetic (`e + 2`). It does not follow memory reached through other
pointers.

**What it reports:** a store through such a pointer, and passing it to a
parameter that is a pointer to non-const. Reading it, or passing it to
`printf`, `strlen` or another const parameter, is fine.

**How it walks the code:** the raw IR of each function (`ir/capi_check.c`), in
the order the code is written. A value set on one branch counts after the join
(the warning means "may write"), and a loop's back edge is not followed. The
check runs only in a unit that met a `yasc_c_api` attribute, so plain C pays
nothing.

## Tests

`tests/ir_tests/test_yasc_c_api.py` covers what is accepted and refused, the
written header, the C callers at -O0 and -O2, and a written header read by a C
unit and by a yasc unit.
